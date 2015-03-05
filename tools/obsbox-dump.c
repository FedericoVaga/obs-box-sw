/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 * License: GPL v3
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <linux/zio-user.h>

#define ZPATH_BUF_SET "/sys/bus/zio/devices/obsbox-%04x/cset0/current_buffer"
#define ZPATH_BUF_VMALLOC_SIZE "/sys/bus/zio/devices/obsbox-%04x/cset0/chan0/buffer/max-buffer-kb"
#define ZPATH_PAGE_SIZE "/sys/bus/zio/devices/obsbox-%04x/cset0/trigger/post-samples"
#define ZPATH_BUF_FLUSH "/sys/bus/zio/devices/obsbox-%04x/cset0/chan0/buffer/flush"
#define ZPATH_ALARMS "/sys/bus/zio/devices/obsbox-%04x/cset0/chan0/alarms"
#define ZPATH_CMD_RUN "/sys/bus/zio/devices/obsbox-%04x/cset0/ob-run"
#define ZPATH_ACQ_MODE "/sys/bus/zio/devices/obsbox-%04x/cset0/ob-streaming-enable"
#define ZPATH_TRG_EN "/sys/bus/zio/devices/obsbox-%04x/cset0/trigger/enable"

#define ZPATH_CDEV_DATA "/dev/zio/obsbox-%04x-0-0-data"
#define ZPATH_CDEV_CTRL "/dev/zio/obsbox-%04x-0-0-ctrl"

/* Where the buffer will be mapped */
static void *mmapaddr;
static uint32_t vmalloc_size = 0;

static void help()
{
	fprintf(stderr,
		"Use: \"obsbox-dump -d 0x<devid> [OPTIONS]\"\n");
	fprintf(stderr, "devid: board device id\n");
	fprintf(stderr, " -r <number>: number of byte to show at the beginning/end\n");
	fprintf(stderr, " -p <number>: acquisition page_size\n");
	fprintf(stderr, " -n <number>: number of blocks to acquire\n");
	fprintf(stderr, " -v <number>: allocate <number>Bytes with vmalloc\n");
	fprintf(stderr, " -s: enable streaming\n");
	fprintf(stderr, " -m: use mmap to read data from a vmalloc buffer (it will not work with kmalloc)\n");
	exit(1);
}

/**
 * it writes to sysfs attribute
 */
#define OBD_W_BUF_LEN 16
static int obd_write_cfg(const char *fmt, uint32_t devid, uint32_t value)
{
	char path[128], val[OBD_W_BUF_LEN] = {0};
	int fd, ret;

	snprintf(path, 128, fmt, devid);
	fd = open(path, O_WRONLY);
	if (!fd)
		return -1;

	snprintf(val, OBD_W_BUF_LEN, "%d", value);
	ret = write(fd, val, OBD_W_BUF_LEN);
	close(fd);

	return (ret == OBD_W_BUF_LEN ? 0 : -1);
}

/**
 * Set the buffer type ((k|v)malloc)
 */
static int obd_buffer_type_set(const char *fmt, uint32_t devid, char *type)
{
	char path[128];
	int fd, ret;

	snprintf(path, 128, fmt, devid);
	fd = open(path, O_WRONLY);
	if (!fd)
		return -1;

	ret = write(fd, type, strlen(type));
	close(fd);

	return (ret == strlen(type) ? 0 : -1);
}


/**
 * Setup vmalloc allocation
 */
static int obd_set_vmalloc(uint32_t devid, uint32_t size)
{
	int err;

	err = obd_buffer_type_set(ZPATH_BUF_SET, devid, "vmalloc");
	if (err)
		return -1;
	return obd_write_cfg(ZPATH_BUF_VMALLOC_SIZE, devid, size/1024);
}

static int obd_set_kmalloc(uint32_t devid)
{
	return obd_buffer_type_set(ZPATH_BUF_SET, devid, "kmalloc");
}

/**
 * Configure basic acquisition
 */
static int obd_configuration(uint32_t devid, int streaming, uint32_t size,
			     uint32_t vmalloc_size)
{
	int ret = 0;

	/* Stop acquisition */
	obd_write_cfg(ZPATH_CMD_RUN, devid, 0);
	/* Disable the trigger for a safe configuration */
	ret |= obd_write_cfg(ZPATH_TRG_EN, devid, 0);
	if (vmalloc_size)
		ret |= obd_set_vmalloc(devid, vmalloc_size);
	else
		ret |= obd_set_kmalloc(devid);
	if (ret) {
		fprintf(stderr, "Cannot set buffer type: %s\n",
			strerror(errno));
		goto out;
	}
	/* Clear previous alarms */
	ret |= obd_write_cfg(ZPATH_ALARMS, devid, 0xFF);
	/* Remove blocks from previous acquisition */
	ret |= obd_write_cfg(ZPATH_BUF_FLUSH, devid, 1);
	/* Configure acquisition mode: 1 streaming, 0 single shot */
	ret |= obd_write_cfg(ZPATH_ACQ_MODE, devid, streaming);
	/* Setting up page-size */
	ret |= obd_write_cfg(ZPATH_PAGE_SIZE, devid, size);
	/* Enable trigger again so we can acquire */
	ret |= obd_write_cfg(ZPATH_TRG_EN, devid, 1);
out:
	return ret;
}


/**
 * Print data from buffer
 */
void print_buffer(uint8_t *buf, int start, int end)
{
	int j;

	for (j = start; j < end; j++) {
		if (!(j & 0xf) || j == start)
			printf("Data [0x%08x]:", j * 16);
		printf(" %02x", buf[j]);
		if ((j & 0xf) == 0xf || j == end - 1)
			putchar('\n');
	}
	putchar('\n');
}


/**
 * Read and dump data from the driver
 * @return number of byte read, -1 on error and errno is appropriately set.
 */
static int obd_block_dump(uint32_t devid, int fdd, int fdc,
			  unsigned int reduce, unsigned int dommap)
{
	struct zio_control zctrl;
	struct timeval tv = {1, 0};
	fd_set ctrl_set;
	uint8_t *buf;
	int n, i;

	/* Wait until a block is ready */
	FD_ZERO(&ctrl_set);
	FD_SET(fdc, &ctrl_set);
	i = select(fdc + 1, &ctrl_set, NULL, NULL, &tv);
	if (i < 0) {
		fprintf(stderr, "obd-dump: select(): %s\n",strerror(errno));
		return -1;
	}
	if (i == 0) /* timeout */
		return 0;


	/* Read the block control information */
	n = read(fdc, &zctrl, sizeof(struct zio_control));
	if (n != sizeof(struct zio_control)) {
		fprintf(stderr, "obd-dump: cannot read zio control\n");
	        return -1;
	}

	/* check the status */
	if (zctrl.zio_alarms & (ZIO_ALARM_LOST_BLOCK | ZIO_ALARM_LOST_TRIGGER)) {
		fprintf(stderr,
			"obd-dump: something went wrong during acquisition\n");
		/* clear the alarm */
		obd_write_cfg(ZPATH_ALARMS, devid, 0xFF);
	}

	/* read data */
	if (dommap) {
		/* mmap way */
		buf = mmapaddr + zctrl.mem_offset;
		n = zctrl.nsamples * zctrl.ssize;
		printf("mmap %p offset 0x%x n %d - buf %p\n",
		       mmapaddr, zctrl.mem_offset, n, buf);

		fprintf(stderr, "mmap out of range %d >= %d\n",
				zctrl.mem_offset + n, vmalloc_size);
	} else {
		/* read way */
		buf = malloc(zctrl.nsamples * zctrl.ssize);
		if (!buf) {
			fprintf(stderr, "obd-dump: cannot allocate buffer\n");
			return -1;
		}
		n = read(fdd, buf, zctrl.nsamples * zctrl.ssize);
	}


	/* report data to stdout */
	fprintf(stdout, "Page number %d\n", zctrl.seq_num);
	if (reduce == -1) {
		print_buffer(buf, 0, n);
	} else {
		print_buffer(buf, 0, reduce);
		printf("    ...\n\n");
		print_buffer(buf, n - reduce, n);
	}

	if (!dommap)
		free(buf);

	return n;
}

#define DUMP_TRY 10
int main(int argc, char **argv)
{
	char c, path[128];
	int ret, streaming = 0, dommap = 0, n = -1, fdd, fdc;
	int reduce, try = DUMP_TRY;
	uint32_t devid, page_size;

	/* Parse options */
	while ((c = getopt (argc, argv, "hd:r:p:n:sv:m")) != -1)
	{
		switch(c)
		{
		case 'h':
			help();
			break;
		case 'r':
			ret = sscanf(optarg, "%d", &reduce);
			if (ret != 1)
				help();
			break;
		case 'd':
			ret = sscanf(optarg, "0x%x", &devid);
			if (ret != 1)
				help();
			break;
		case 'p':
			ret = sscanf(optarg, "%u", &page_size);
			if (ret != 1)
				help();
			break;
		case 'n':
			ret = sscanf(optarg, "%d", &n);
			if (ret != 1)
				help();
			break;
		case 's':
			streaming = 1;
			break;
		case 'v':
			ret = sscanf(optarg, "%d", &vmalloc_size);
			if (ret != 1)
				help();
			break;
		case 'm':
			dommap = 1;
			break;
		default:
			help(argv[0]);
		}
	}

	/* Configure the acquisition */
	ret = obd_configuration(devid, streaming, page_size, vmalloc_size);
	if (ret){
		fprintf(stderr,
			"Something wrong during the configuration: %s\n",
			strerror(errno));
		exit(1);
	}

	if (!streaming && n == -1)
		n = 1;

	/* Open ZIO char-devices */
	snprintf(path, 128, ZPATH_CDEV_DATA, devid);
	fdd = open(path, O_RDONLY);
	snprintf(path, 128, ZPATH_CDEV_CTRL, devid);
	fdc = open(path, O_RDONLY);

	if (!fdd || !fdc)
		goto out;

	if (dommap) {
		if (!vmalloc_size) {
			fprintf(stderr,
				"mmap(2) works only with vmalloc allocation\n");
			goto out;
		}
		mmapaddr = mmap(0, vmalloc_size, PROT_READ, MAP_SHARED,
				fdd, 0);
		if (mmapaddr == MAP_FAILED) {
			fprintf(stderr, "Cannot mmap buffer: %s\n",
				strerror(errno));
			goto out;
		}
	}

	if (streaming) {
		/*
		 * In streaming mode we start the acquisition only one time
		 * before the acquisition
		 */
		obd_write_cfg(ZPATH_CMD_RUN, devid, 1);
		fprintf(stdout, "Start acquisition in streaming mode\n");
	}
	while (n && try) {
		if (!streaming) {
			/*
			 * In case of single-shot mode we have to start
			 * the acquisition for every block
			 */
			obd_write_cfg(ZPATH_CMD_RUN, devid, 1);
		}

		ret = obd_block_dump(devid, fdd, fdc, reduce, dommap);
		if (ret < 0)
			break;
		if (ret == 0) {
			try--;
			fprintf(stderr, "(try %d)\n", DUMP_TRY - try);
			continue;
		} else {
			try = DUMP_TRY;
		}
		if (n > 0)
			n--;
	}

	if (!try)
		fprintf(stderr, "Fail %d times to acquire a page\n", DUMP_TRY);
	if (dommap && vmalloc_size)
		munmap(mmapaddr, vmalloc_size);
	close(fdd);
	close(fdc);
	obd_write_cfg(ZPATH_CMD_RUN, devid, 0);
	exit(0);

out:
	obd_write_cfg(ZPATH_CMD_RUN, devid, 0);
	exit(1);
}

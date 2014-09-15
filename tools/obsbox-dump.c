/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <linux/zio-user.h>


#define ZPATH_PAGE_SIZE "/sys/bus/zio/devices/obsbox-%04x/cset0/trigger/post-samples"
#define ZPATH_BUF_FLUSH "/sys/bus/zio/devices/obsbox-%04x/cset0/chan0/buffer/flush"
#define ZPATH_ALARMS "/sys/bus/zio/devices/obsbox-%04x/cset0/chan0/alarms"
#define ZPATH_CMD_RUN "/sys/bus/zio/devices/obsbox-%04x/cset0/ob-run"
#define ZPATH_ACQ_MODE "/sys/bus/zio/devices/obsbox-%04x/cset0/ob-streaming-enable"
#define ZPATH_TRG_EN "/sys/bus/zio/devices/obsbox-%04x/cset0/trigger/enable"

#define ZPATH_CDEV_DATA "/dev/zio/obsbox-%04x-0-0-data"
#define ZPATH_CDEV_CTRL "/dev/zio/obsbox-%04x-0-0-ctrl"

static void help()
{
	fprintf(stderr,
		"Use: \"obsbox-dump -d 0x<devid> [OPTIONS]\"\n");
	fprintf(stderr, "devid: board device id\n");
	fprintf(stderr, " -r <number>: number of byte to show at the beginning/end\n");
	fprintf(stderr, " -p <number>: acquisition page_size\n");
	fprintf(stderr, " -n <number>: number of blocks to acquire\n");
	fprintf(stderr, " -s: enable streaming\n");
	exit(1);
}

/**
 * it writes to sysfs attribute
 */
static int obd_write_cfg(const char *fmt, uint32_t devid, uint32_t value)
{
	char path[128], val[8];
	int fd, ret;

	snprintf(path, 128, fmt, devid);
	fd = open(path, O_WRONLY);
	if (!fd)
		return -1;

	snprintf(val, 8, "%d", value);
	ret = write(fd, val, 8);
	close(fd);

	return (ret == 8 ? 0 : -1);
}


/**
 *
 */
static int obd_configuration(uint32_t devid, int streaming, uint32_t size)
{
	int ret = 0;

	/* Stop acquisition */
	obd_write_cfg(ZPATH_CMD_RUN, devid, 0);
	/* Disable the trigger for a safe configuration */
	ret |= obd_write_cfg(ZPATH_TRG_EN, devid, 0);
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

	return ret;
}

void print_buffer(uint8_t *buf, int start, int end)
{
	int j;

	for (j = start; j < end; j++) {
		if (!(j & 0xf) || j == start)
			printf("Data:");
		printf(" %02x", buf[j]);
		if ((j & 0xf) == 0xf || j == end - 1)
			putchar('\n');
	}
	putchar('\n');
}

static int obd_block_dump(uint32_t devid, int fdd, int fdc, unsigned int reduce)
{
	struct zio_control zctrl;
	fd_set ctrl_set;
	uint8_t *buf;
	int n, i;

	/* Wait until a block is ready */
	FD_ZERO(&ctrl_set);
	FD_SET(fdc, &ctrl_set);
	i = select(fdc + 1, &ctrl_set, NULL, NULL, NULL);
	if (i < 0 && errno == EINTR)
		return i;
	if (i < 0) {
		fprintf(stderr, "obd-dump: select(): %s\n",strerror(errno));
		return -1;
	}

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
	buf = malloc(zctrl.nsamples * zctrl.ssize);
	if (!buf) {
		fprintf(stderr, "obd-dump: cannot allocate buffer\n");
		return -1;
	}
	n = read(fdd, buf, zctrl.nsamples * zctrl.ssize);

	/* report data to stdout */
	if (reduce == -1) {
		print_buffer(buf, 0, n);
	} else {
		print_buffer(buf, 0, reduce);
		printf("    ...\n\n");
		print_buffer(buf, n - reduce, n);
	}

	return 0;
}


int main(int argc, char **argv)
{
	char c, path[128];
	int ret, streaming = 0, n = -1, fdd, fdc;
	int reduce;
	uint32_t devid, page_size;

	/* Parse options */
	while ((c = getopt (argc, argv, "hd:r:p:n:s")) != -1)
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
		default:
			help(argv[0]);
		}
	}

	/* Configure the acquisition */
	ret = obd_configuration(devid, streaming, page_size);
	if (ret){
		fprintf(stderr, "Something wrong during the configuration: %s\n", strerror(errno));
		exit(1);
	}

	if (!streaming)
		n = 1;

	/* Open ZIO char-devices */
	snprintf(path, 128, ZPATH_CDEV_DATA, devid);
	fdd = open(path, O_RDONLY);
	snprintf(path, 128, ZPATH_CDEV_CTRL, devid);
	fdc = open(path, O_RDONLY);

	if (!fdd || !fdc)
		goto out;

	obd_write_cfg(ZPATH_CMD_RUN, devid, 1);
	while (n) {
		ret = obd_block_dump(devid, fdd, fdc, reduce);
		if (ret < 0)
			break;
		if (n > 0)
			n--;
	}

	close(fdd);
	close(fdc);
	obd_write_cfg(ZPATH_CMD_RUN, devid, 0);
	exit(0);

out:
	obd_write_cfg(ZPATH_CMD_RUN, devid, 0);
	exit(1);
}
/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 * License: GPL v2
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/fmc.h>
#include <linux/fmc-sdb.h>
#include <linux/zio.h>
#include <linux/zio-dma.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#include "obsbox.h"

/**
 * Parameters
 */
static struct zio_attribute ob_cset_ext_zattr[] = {
	/*
	 * 0: stop
	 * 1: start/restart
	 */
	ZIO_PARAM_EXT("ob-run", ZIO_WO_PERM, OB_PARM_RUN, 0),
	/*
	 * 0: single shot
	 * 1: streaming
	 */
	ZIO_PARAM_EXT("ob-streaming-enable", ZIO_RW_PERM, OB_PARM_STREAM, 1),
};

void ob_acquisition_command(struct ob_dev *ob, uint32_t cmd)
{
	struct zio_cset *cset = &ob->zdev->cset[0];
	int err;

	if (cmd == 0) { /* Stop the acquisition */
		dev_dbg(ob->fmc->hwdev, "Stop acquisition\n");
		ob_disable_irq(ob);
		zio_trigger_abort_disable(cset, 0);
	} else { /* Start the acquisition */
		/* Reset statistics counter */
		ob->done = 0;
		ob->c_err = 0;
		ob->errors = 0;

		/* reset CSR */
		ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_RST_GTP], 1);
		/* FIXME set RST_GTP to 0 again?*/

		/* Configure page-size */
		err = ob_set_page_size(ob, cset->ti->nsamples);
		if (err)
			return;
		/*
		 * HACK
		 * due to a FPGA problem whe must wait a little bit after
		 * configuring the page size and after disabling
		 * the interrupts
		 */
		udelay(10000);
		ob_disable_irq(ob);
		udelay(10000);

		/* Arm the ZIO trigger (we are self timed) */
		zio_arm_trigger(cset->ti);
		if (!(cset->ti->flags & ZIO_TI_ARMED))
			return;
		/* Finally enable the acquisition by enabling the interrupts */
		dev_dbg(ob->fmc->hwdev, "Start acquisition\n");
		ob_enable_irq(ob);
	}
	/* FIXME It was necessary on old firmware, is still necessary? */
	/*ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_TX_DIS], cmd);*/
}


/**
 * FIXME HACK
 * It is a timer function. It is used to perform commands from sysfs. This is
 * necessary to avoid sysfs file freeze (I don't know the reasons)
 */
static void ob_start(unsigned long arg)
{
	struct ob_dev *ob = (void *)arg;

	ob_acquisition_command(ob, !!(ob->flags & OB_FLAG_RUNNING) );
}


/**
 * It sets parameters' values
 */
static int ob_conf_set(struct device *dev, struct zio_attribute *zattr,
			 uint32_t usr_val)
{
	struct zio_cset *cset = to_zio_cset(dev);
	struct ob_dev *ob = cset->zdev->priv_d;

	switch(zattr->id) {
	case OB_PARM_RUN: /* Run/Stop acquisition */
		spin_lock(&ob->lock);
		if (usr_val == 0)
			ob->flags &= ~OB_FLAG_RUNNING;
		else
			ob->flags |= OB_FLAG_RUNNING;
		spin_unlock(&ob->lock);
		mod_timer(&ob->tm, jiffies + msecs_to_jiffies(1000));
		break;
	case OB_PARM_STREAM: /* Enable/Disable streaming */
		spin_lock(&cset->lock);
		if (usr_val)
			cset->flags |= ZIO_CSET_SELF_TIMED;
		else
			cset->flags &= ~ZIO_CSET_SELF_TIMED;
		spin_unlock(&cset->lock);
		break;
	}

	return 0;
}

/**
 * It gets information about the current parameter status
 */
static int ob_info_get(struct device *dev, struct zio_attribute *zattr,
		       uint32_t *usr_val)
{
	struct zio_cset *cset = to_zio_cset(dev);
	struct ob_dev *ob = cset->zdev->priv_d;

	switch(zattr->id) {
	case OB_PARM_RUN:
		*usr_val = !!(ob->flags & OB_FLAG_RUNNING);
		break;
	case OB_PARM_STREAM: /* Enable/Disable streaming */
		*usr_val = !!(cset->flags & ZIO_CSET_SELF_TIMED);
		break;
	}

	return 0;
}

static const struct zio_sysfs_operations ob_s_op = {
	.conf_set = ob_conf_set,
	.info_get = ob_info_get,
};

/**
 * It runs on trigger arm in order to prepare the hardware to the acquisition
 */
static int ob_input_cset(struct zio_cset *cset)
{
	/* Nothing special to do, just wait for interrupts */
	return -EAGAIN;
}


static int ob_probe(struct zio_device *zdev)
{
	struct ob_dev *ob = zdev->priv_d;
	struct zio_attribute_set *zset;

	dev_dbg(&zdev->head.dev, "%s:%d\n", __func__, __LINE__);

	/* Check if hardware supports 64-bit DMA */
	if (dma_set_mask(ob->fmc->hwdev, DMA_BIT_MASK(64))) {
		/* Check if hardware supports 32-bit DMA */
		if (dma_set_mask(ob->fmc->hwdev, DMA_BIT_MASK(32))) {
			dev_err(ob->fmc->hwdev,
				"32-bit DMA addressing not available\n");
			return -EINVAL;
		}
	}

	/* Save also the pointer to the real zio_device */
	ob->zdev = zdev;
	spin_lock_init(&ob->lock);
	setup_timer(&ob->tm, ob_start, (unsigned long)ob);

	/* Enable streaming by default - let do it here to avoid autostart */
	ob->zdev->cset[0].flags |= ZIO_CSET_SELF_TIMED;

	/* HACK - Update the post-samples manualy */
	zset = &zdev->cset->ti->zattr_set;
	zset->std_zattr[ZIO_ATTR_TRIG_POST_SAMP].value = OB_MIN_PAGE_SIZE;
	zdev->cset->ti->nsamples = OB_MIN_PAGE_SIZE;
	ob->cur_page_size = zdev->cset->ti->nsamples;
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_SIZE],
		  ob->cur_page_size);

	/* Enable DMA and OBS interrupts */
	ob_init_irq(ob);

	return 0;
}

static int ob_remove(struct zio_device *zdev)
{
	struct ob_dev *ob = zdev->priv_d;

	ob_exit_irq(ob);

	return 0;
}


/**
 * The OBS-BOX device hierarchy is really simple:
 * - 1 channel set
 * - 1 channel
 * By default it uses the 'vmalloc' buffer to allow large buffers.  There is no
 * particular trigger-requirement, so the trigger user is fine.
 */
static struct zio_cset ob_cset[] = {
	{
		.raw_io = ob_input_cset,
		.ssize = 1,
		.n_chan = 1,
		.flags =  ZIO_CSET_TYPE_ANALOG |
			  ZIO_DIR_INPUT,
		.zattr_set = {
			.ext_zattr = ob_cset_ext_zattr,
			.n_ext_attr = ARRAY_SIZE(ob_cset_ext_zattr),
		},
	}
};

struct zio_device ob_tmpl = {
	.owner = THIS_MODULE,
	.s_op = &ob_s_op,
	.flags = 0,
	.cset = ob_cset,
	.n_cset = ARRAY_SIZE(ob_cset),
	.preferred_trigger = "user",
	.preferred_buffer = "vmalloc",
};

static const struct zio_device_id ob_table[] = {
	{"obsbox", &ob_tmpl},
	{},
};


struct zio_driver ob_driver = {
	.driver = {
		.name	= "obsbox",
		.owner	= THIS_MODULE,
	},
	.id_table	= ob_table,
	.probe		= ob_probe,
	.remove		= ob_remove,
};

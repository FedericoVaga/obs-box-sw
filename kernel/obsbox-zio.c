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
	ZIO_ATTR_EXT("aligned", ZIO_RO_PERM, OB_ALIGNED, 0),
	/*
	 * 0: stop
	 * 1: start/restart
	 */
	ZIO_PARAM_EXT("ob-run", ZIO_RW_PERM, OB_PARM_RUN, 0),
	/*
	 * 0: single shot
	 * 1: streaming
	 */
	ZIO_PARAM_EXT("ob-streaming-enable", ZIO_RW_PERM, OB_PARM_STREAM, 1),
};


/**
 * Align the serdes
 */
static void ob_align(struct ob_dev *ob)
{
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_RST_CDR], 1);
	udelay(2000);
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_RST_ALG], 1);
	udelay(2000);
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_RST_BUF], 1);
	udelay(2000);
}


/**
 * Wait until the serdes is aligned
 * @return 0 on succes, -1 on error (not aligned)
 */
#define OB_ALIGN_TRY 30
#define OB_ALIGN_UDELAY 1000
static int ob_wait_aligned(struct ob_dev *ob)
{
	int i;
	uint32_t val;

	for (i = 0; i < OB_ALIGN_TRY; ++i) {
		val = ob_readl(ob, ob->base_obs_core,
			       &ob_regs[ACQ_STS_SFP_ALIGNED]);
		if (val)
			return 0;
		udelay(OB_ALIGN_UDELAY);
	}

	dev_warn(&ob->zdev->head.dev,
		 "SERDES interface alignment: fail after %dus\n",
		 (OB_ALIGN_TRY * OB_ALIGN_UDELAY));
	return -EPERM;
}


int ob_acquisition_command(struct ob_dev *ob, uint32_t cmd)
{
	struct zio_cset *cset = &ob->zdev->cset[0];
	unsigned long flags;
	int err;

	spin_lock_irqsave(&ob->lock, flags);
	if (cmd == 0)
		ob->flags &= ~OB_FLAG_RUNNING;
	else
		ob->flags |= OB_FLAG_RUNNING;
	ob->flags &= ~OB_FLAG_STOPPING;
	spin_unlock_irqrestore(&ob->lock, flags);

	/*
	 * Disable the interrupt and abort any previous acquisition
	 * in order to allow us to configure
	 */
	ob_disable_irq(ob);
	zio_trigger_abort_disable(cset, 0);
	if (!cmd)
		return 0;

	/* Start the acquisition */

	/* Reset statistics counter */
	ob->done = 0;
	ob->c_err = 0;
	ob->errors = 0;

	/* reset CSR */
	ob_align(ob);
	err = ob_wait_aligned(ob);
	if (err) {
		dev_err(ob->fmc->hwdev, "Cannot align acquisition\n");
		return err;
	}

	/* Configure page-size */
	err = ob_set_page_size(ob, cset->ti->nsamples);
	if (err) {
		dev_err(ob->fmc->hwdev, "Cannot set acquisition page size %d\n",
			cset->ti->nsamples);
		return err;
	}

	/* Arm the ZIO trigger (we are self timed) */
	zio_arm_trigger(cset->ti);
	if (!(cset->ti->flags & ZIO_TI_ARMED))
		return -EBUSY;

	ob_enable_irq(ob);

	return 0;
}


/**
 * It sets parameters' values
 */
static int ob_conf_set(struct device *dev, struct zio_attribute *zattr,
			 uint32_t usr_val)
{
	struct zio_cset *cset = to_zio_cset(dev);
	struct ob_dev *ob = cset->zdev->priv_d;
	unsigned long flags;
	int err = 0;

	switch(zattr->id) {
	case OB_PARM_RUN: /* Run/Stop acquisition */
		dev_dbg(ob->fmc->hwdev, "%s acquisition\n",
			usr_val ? "Start" : "Stop");
		if (usr_val) {
			err = ob_acquisition_command(ob, 1);
		} else {
			if (ob->flags & OB_FLAG_STOPPING) {
				err = -EBUSY;
				dev_warn(ob->fmc->hwdev,
					 "Acquisition stop already programmed\n");
				break;
			}
			spin_lock_irqsave(&ob->lock, flags);
			ob->flags |= OB_FLAG_STOPPING;
			spin_unlock_irqrestore(&ob->lock, flags);
		}
		break;
	case OB_PARM_STREAM: /* Enable/Disable streaming */
		/* Disable acquisition when mode change */
		err = ob_acquisition_command(ob, 0);

		spin_lock(&cset->lock);
		if (usr_val)
			cset->flags |= ZIO_CSET_SELF_TIMED;
		else
			cset->flags &= ~ZIO_CSET_SELF_TIMED;
		spin_unlock(&cset->lock);
		break;
	}

	return err;
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
	case OB_ALIGNED:
		*usr_val = !!ob_readl(ob, ob->base_obs_core,
				      &ob_regs[ACQ_STS_SFP_ALIGNED]);
		break;
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

	/* Enable streaming by default - let do it here to avoid autostart */
	ob->zdev->cset[0].flags |= ZIO_CSET_SELF_TIMED;

	/* HACK - Update the post-samples manualy */
	zset = &zdev->cset->ti->zattr_set;
	zset->std_zattr[ZIO_ATTR_TRIG_POST_SAMP].value = OB_MIN_PAGE_SIZE;
	zdev->cset->ti->nsamples = OB_MIN_PAGE_SIZE;
	ob->cur_page_size = zdev->cset->ti->nsamples;
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_SIZE],
		  ob->cur_page_size);
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_TX_DIS], 0);

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
	/* Take the version from ZIO git sub-module */
	.min_version = ZIO_VERSION(__ZIO_MIN_MAJOR_VERSION,
				   __ZIO_MIN_MINOR_VERSION,
				   0), /* Change it if you use new features from
					  a specific patch */
};

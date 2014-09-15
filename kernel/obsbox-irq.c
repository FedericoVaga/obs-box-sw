/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/fmc.h>
#include <linux/fmc-sdb.h>
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#include "obsbox.h"
#include "zio-helpers.h"

static int gncore_dma_fill(struct zio_dma_sg *zsg)
{
	struct gncore_dma_item *item = (struct gncore_dma_item *)zsg->page_desc;
	struct scatterlist *sg = zsg->sg;
	struct zio_channel *chan = zsg->zsgt->chan;
	struct ob_dev *ob = chan->cset->zdev->priv_d;
	dma_addr_t tmp;

	/* Prepare DMA item */
	item->start_addr = zsg->dev_mem_off;
	item->dma_addr_l = sg_dma_address(sg) & 0xFFFFFFFF;
	item->dma_addr_h = (uint64_t)sg_dma_address(sg) >> 32;
	item->dma_len = sg_dma_len(sg);

	if (!sg_is_last(sg)) {/* more transfers */
		/* uint64_t so it works on 32 and 64 bit */
		tmp = zsg->zsgt->dma_page_desc_pool;
		tmp += (zsg->zsgt->page_desc_size * (zsg->page_idx + 1));
		item->next_addr_l = ((uint64_t)tmp) & 0xFFFFFFFF;
		item->next_addr_h = ((uint64_t)tmp) >> 32;
		item->attribute = 0x1;	/* more items */
	} else {
		item->attribute = 0x0;	/* last item */
	}

	/* The first item is written on the device */
	if (zsg->page_idx == 0) {
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_ADDR], item->start_addr);
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_ADDR_L], item->dma_addr_l);
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_ADDR_H], item->dma_addr_h);
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_LEN], item->dma_len);
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_NEXT_L], item->next_addr_l);
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_NEXT_H], item->next_addr_h);
		/* Set that there is a next item */
		ob_writel(ob, ob->base_dma_core,
			  &ob_regs[DMA_BR_LAST], item->attribute);
	}

	dev_dbg(zsg->zsgt->hwdev, "configure DMA item %d (block %d)"
		"(addr: 0x%llx len: %d)(dev off: 0x%x) (next item: 0x%x)\n",
		zsg->page_idx, zsg->block_idx, (long long)sg_dma_address(sg),
		sg_dma_len(sg), zsg->dev_mem_off, item->next_addr_l);

	return 0;
}

static void ob_run_dma(struct ob_dev *ob, struct zio_cset *cset)
{
	struct zio_block *blocks[1];
	struct timespec t[5];
	int err, i;

	getnstimeofday(&t[0]);
	blocks[0] = cset->chan[0].active_block;
	if (unlikely(!blocks[0])) {
		/* Invalid block - trigger has done */
		dev_err(ob->fmc->hwdev, "zio block is missing\n");
		goto out;
	}

	/* We are busy, we are starting DMA */
	spin_lock(&cset->lock);
	cset->flags |= ZIO_CSET_HW_BUSY;
	spin_unlock(&cset->lock);

	getnstimeofday(&t[1]);


	/* There is only one channel, so one blocks to transfer */
	ob->zdma = zio_dma_alloc_sg(&cset->chan[0], ob->fmc->hwdev,
				    blocks, 1, GFP_ATOMIC);
	if (IS_ERR(ob->zdma)) {
		dev_err(ob->fmc->hwdev, "ZIO cannot allocate DMA memory\n");
	        goto out_alloc;
	}

	getnstimeofday(&t[2]);

	/* Set the correct device memory offset - is a single shot state machine */
	ob->zdma->sg_blocks[0].dev_mem_off = ob->last_acq_page;

	err = zio_dma_map_sg(ob->zdma, sizeof(struct gncore_dma_item),
			     gncore_dma_fill);
	if (err) {
		dev_err(ob->fmc->hwdev, "ZIO cannot map DMA memory (%d)\n", err);
	        goto out_map;
	}
	getnstimeofday(&t[3]);
	/* Start DMA transfer */
	ob_writel(ob, ob->base_dma_core, &ob_regs[DMA_CTL_START], 1);
	getnstimeofday(&t[4]);

	for (i = 0; i < 5; ++i) {
		pr_info("%s:%d [%d] %d.%09d", __func__, __LINE__, i, t[i].tv_sec, t[i].tv_nsec);
	}
	return;

out_map:
	zio_dma_free_sg(ob->zdma);
out_alloc:
	spin_lock(&cset->lock);
	cset->flags &= ~ZIO_CSET_HW_BUSY;
	spin_unlock(&cset->lock);
out:
	zio_trigger_data_done(cset);
	ob->errors++;
}

static void ob_get_irq_status(struct ob_dev *ob, int irq_core_base,
			      enum obsbox_registers reg,
			      uint32_t *irq_status)
{
	struct timespec t;

	/* Get current interrupts status */
	*irq_status = ob_readl(ob, irq_core_base, &ob_regs[reg]);
	dev_dbg(ob->fmc->hwdev,
		"IRQ 0x%x fired an interrupt. IRQ status register: 0x%x\n",
		irq_core_base, *irq_status);
	getnstimeofday(&t);
	pr_info("%s:%d 0x%x %d.%09d", __func__, __LINE__, irq_core_base, t.tv_sec, t.tv_nsec);
	if (*irq_status)
		/* Clear current interrupts status */
		ob_writel(ob, irq_core_base, &ob_regs[reg], *irq_status );
}


/**
 * It handles the DMA interrupts. On DMA done, notify to ZIO that the
 * trigger run is over and store the block of data.
 */
irqreturn_t ob_dma_irq_handler(int irq_core_base, void *dev_id)
{
	struct fmc_device *fmc = dev_id;
	struct ob_dev *ob = fmc_get_drvdata(fmc);
	struct zio_cset *cset = ob->zdev->cset;
	struct timespec t;
	uint32_t status;
	int rearm;

	ob_get_irq_status(ob, irq_core_base, IRQ_DMA_SRC, &status);
	if (unlikely(!status))
		return IRQ_NONE;

	zio_dma_unmap_sg(ob->zdma);
	zio_dma_free_sg(ob->zdma);

	spin_lock(&cset->lock);
	cset->flags &= ~ZIO_CSET_HW_BUSY;
	spin_unlock(&cset->lock);
	getnstimeofday(&t);
	pr_info("%s:%d %d.%09d", __func__, __LINE__, t.tv_sec, t.tv_nsec);

	if (status & GNCORE_IRQ_DMA_DONE) {
		rearm = zio_trigger_data_done(cset);
		dev_dbg(ob->fmc->hwdev, "Page acquired at 0x%x\n",
			ob->last_acq_page);
		ob->done++;
		/* Stop acquisition if not streaming mode */
		if (!rearm) {
			dev_dbg(ob->fmc->hwdev,
				"Single shot mode, Stop acquisition");
			ob_acquisition_command(ob, 0);
		}
	} else {
		dev_err(ob->fmc->hwdev,
			"Error during DMA transmission (counter %d)\n",
			ob->errors);
		ob->errors++;
		ob_acquisition_command(ob, 0);
	}

	/* ack the irq */
	ob->fmc->op->irq_ack(ob->fmc);

	return IRQ_HANDLED;
}

/**
 * It handles the OBS interrupts. If the hardware is not busy in a DMA transfer,
 * then start a new DMA transfer for the last page.
 */
irqreturn_t ob_core_irq_handler(int irq_core_base, void *dev_id)
{
	struct fmc_device *fmc = dev_id;
	struct ob_dev *ob = fmc_get_drvdata(fmc);
	uint32_t status, page;

	ob_get_irq_status(ob, irq_core_base, IRQ_ACQ_SRC, &status);
	if (unlikely(!(status & OBS_IRQ_ACQ)))
		return IRQ_NONE;

	page = ob_readl(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_ADDR]);

	/* If the hardware is busy, do not waste other time */
	if(ob->zdev->cset[0].flags & ZIO_CSET_HW_BUSY) {
		dev_warn(ob->fmc->hwdev, "PAGE (0x%x) LOST - HW BUSY\n", page);
		return IRQ_HANDLED;
	}

	if (likely((ob->zdev->cset->ti->flags & ZIO_TI_ARMED))) {
		ob->last_acq_page = page;
		dev_dbg(ob->fmc->hwdev, "Page ready at 0x%x\n",
			 ob->last_acq_page);
		ob_run_dma(ob, &ob->zdev->cset[0]);
		ob->c_err = 0;
	} else {
		dev_warn(ob->fmc->hwdev,
			 "ZIO trigger not configured, page lost (0x%x)\n",
			 page);
		ob->errors++;
		ob->c_err++;
		if(ob->c_err > 10) {
			dev_err(ob->fmc->hwdev,
				"ZIO is not armed for %d consecutive times. Stop acquisition\n",
				ob->c_err);
			ob_acquisition_command(ob, 0);
		}
	}

	/* ack the irq */
	ob->fmc->op->irq_ack(ob->fmc);
	return IRQ_HANDLED;
}

/**
 * It registers DMA and OBS interrupts
 */
int ob_init_irq(struct ob_dev *ob)
{
	int err;

	/* Disable IRQ to prevent spurious interrupts */
	ob_disable_irq(ob);

	ob->fmc->irq = ob->base_dma_irq;
	err = ob->fmc->op->irq_request(ob->fmc, ob_dma_irq_handler,
					"obs-box-dma",
					0 /*VIC is used */);
	if (err)
		return err;

	ob->fmc->irq = ob->base_obs_irq;
        return ob->fmc->op->irq_request(ob->fmc, ob_core_irq_handler,
					"obs-box-core",
					0 /*VIC is used */);
}


/**
 * It releases DMA and OBS interrupts
 */
void ob_exit_irq(struct ob_dev *ob)
{
  	ob->fmc->irq = ob->base_obs_irq;
	ob->fmc->op->irq_free(ob->fmc);

	ob->fmc->irq = ob->base_dma_irq;
	ob->fmc->op->irq_free(ob->fmc);
}

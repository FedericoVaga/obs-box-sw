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

	if (sg_is_last(sg) && zsg->block_idx == zsg->zsgt->n_blocks - 1) {
		item->attribute = 0x0;	/* last item */
	} else {
		/* more transfers */
		/* uint64_t so it works on 32 and 64 bit */
		tmp = zsg->zsgt->page_desc_pool_dma_next;
		item->next_addr_l = ((uint64_t)tmp) & 0xFFFFFFFF;
		item->next_addr_h = ((uint64_t)tmp) >> 32;
		item->attribute = 0x1;	/* more items */
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

	dev_vdbg(zsg->zsgt->hwdev, "DMA item %d (block %d)\n"
		 "    addr   0x%x\n"
		 "    addr_l 0x%x\n"
		 "    addr_h 0x%x\n"
		 "    length %d\n"
		 "    next_l 0x%x\n"
		 "    next_h 0x%x\n"
		 "    last   0x%x\n",
		 zsg->page_idx, zsg->block_idx,
		 item->start_addr, item->dma_addr_l, item->dma_addr_h,
		 item->dma_len, item->next_addr_l, item->next_addr_h,
		 item->attribute);

	return 0;
}

static void ob_run_dma(struct ob_dev *ob, struct zio_cset *cset)
{
	struct zio_block *blocks[1];
	unsigned long flags;
	uint32_t acq_page;
	int err;

	blocks[0] = cset->chan[0].active_block;
	if (unlikely(!blocks[0])) {
		/* Invalid block - trigger has done */
		dev_err(ob->fmc->hwdev, "zio block is missing\n");
		goto out;
	}

	/* If the hardware is busy, do not waste other time */
	if(cset->flags & ZIO_CSET_HW_BUSY) {
		dev_warn(ob->fmc->hwdev,
			 "PAGE LOST - DMA running\n");
		goto out;
	}

	/* We are busy, we are starting DMA */
	spin_lock_irqsave(&cset->lock, flags);
	cset->flags |= ZIO_CSET_HW_BUSY;
	spin_unlock_irqrestore(&cset->lock, flags);

	/*
	 * Get the address now so we can print it before the dma mapping.
	 * So, on debugging we can still mesure the dma mapping time without
	 * print delays
	 */
	acq_page = ob_readl(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_ADDR]);
	dev_dbg(ob->fmc->hwdev,	"Acquisition of page 0x%x in block %p\n",
		acq_page, blocks[0]);

	/* There is only one channel, so one blocks to transfer */
	ob->zdma = zio_dma_alloc_sg(&cset->chan[0], ob->fmc->hwdev,
				    blocks, 1, GFP_ATOMIC);
	if (IS_ERR(ob->zdma)) {
		dev_err(ob->fmc->hwdev, "ZIO cannot allocate DMA memory\n");
	        goto out_alloc;
	}

	/* Set the correct device memory offset
	   (is a single shot state machine) */
	ob->zdma->sg_blocks[0].dev_mem_off = acq_page;

	err = zio_dma_map_sg(ob->zdma, sizeof(struct gncore_dma_item),
			     gncore_dma_fill);
	if (err) {
		dev_err(ob->fmc->hwdev, "ZIO cannot map DMA memory (%d)\n", err);
	        goto out_map;
	}

	/* Start DMA transfer */
	ob_writel(ob, ob->base_dma_core, &ob_regs[DMA_CTL_START], 1);
	ob->c_err = 0;

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
	/* Get current interrupts status */
	*irq_status = ob_readl(ob, irq_core_base, &ob_regs[reg]);
	if (*irq_status) {
		dev_vdbg(ob->fmc->hwdev,
			"IRQ 0x%x fired an interrupt. IRQ status register: 0x%x\n",
			irq_core_base, *irq_status);
		/* Clear current interrupts status */
		ob_writel(ob, irq_core_base, &ob_regs[reg], *irq_status );
	}
}


/**
 * Kill the acquisition if there are too many errors
 */
void ob_check_errors(struct ob_dev *ob)
{
	if(unlikely(ob->c_err < 5 && ob->errors < 20))
		return;

	dev_err(ob->fmc->hwdev,
		"We got %d global errors, %d are consecutive\n",
		ob->errors, ob->c_err);
	ob_acquisition_command(ob, 0);
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
	uint32_t status;
	int rearm;

	ob_get_irq_status(ob, irq_core_base, IRQ_DMA_SRC, &status);
	if (!status)
		return IRQ_NONE;
	dev_dbg(ob->fmc->hwdev, "Page acquired in block %p\n",
		cset->chan->active_block);

	zio_dma_unmap_sg(ob->zdma);
	zio_dma_free_sg(ob->zdma);

	/* DMA is over */
	if (unlikely(!(status & GNCORE_IRQ_DMA_DONE))) {
		zio_dma_error(ob->zdma);
		ob->errors++;
		ob->c_err++;
	}

	/* The block is not used anymore by the hardware */
	spin_lock(&cset->lock);
	cset->flags &= ~ZIO_CSET_HW_BUSY;
	spin_unlock(&cset->lock);

	/* The acquisition is over (error or not) */
	rearm = zio_trigger_data_done(cset);

	if (likely(status & GNCORE_IRQ_DMA_DONE)) {
		/* Count a succesful acquisition */
		ob->done++;
		if (!rearm) {
			/* Stop acquisition if not streaming mode */
			dev_dbg(ob->fmc->hwdev,
				"Single shot mode, Stop acquisition");
			ob_acquisition_command(ob, 0);
		}
	}

	ob_check_errors(ob);
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
	uint32_t status;

	ob_get_irq_status(ob, irq_core_base, IRQ_ACQ_SRC, &status);
	if (!(status & OBS_IRQ_ACQ))
		return IRQ_NONE;

	if (unlikely(!(ob->zdev->cset->ti->flags & ZIO_TI_ARMED))) {
		/* ZIO was not ready for this shot */
		dev_warn(ob->fmc->hwdev,
			 "ZIO trigger not configured, page lost\n");
		ob->errors++;
		ob->c_err++;
		goto out;
	}

	/* Everything is fine, we have a page: run the DMA transfer*/
	ob_run_dma(ob, &ob->zdev->cset[0]);

out:
	ob_check_errors(ob);
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

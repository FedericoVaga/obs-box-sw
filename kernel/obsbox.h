/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 * License: GPL v2
 */

#ifndef __OBS_BOX_H__
#define __OBS_BOX_H__
#include <linux/fmc.h>
#include "zio-helpers.h"

#define OB_DEFAULT_GATEWARE "fmc/spec-rf-obs-box.bin"
#define OB_MAX_PAGE_SIZE 0x8000000 /* 128MB - half the SPEC memory */
#define OB_MIN_PAGE_SIZE 0x0000800 /* 1MB  */

#define OB_FLAG_RUNNING (1 << 0) /* Acquisition is running */
#define OB_FLAG_STREAMING (1 << 0) /* Streaming is enabled */

#define GNCORE_IRQ_DMA_DONE (1 << 0)
#define GNCORE_IRQ_DMA_ERR (1 << 1)
#define GNCORE_IRQ_DMA_ALL (GNCORE_IRQ_DMA_DONE | GNCORE_IRQ_DMA_ERR)

#define OBS_IRQ_TRG (1 << 0)
#define OBS_IRQ_ACQ (1 << 1)
#define OBS_IRQ_ALL (OBS_IRQ_TRG|OBS_IRQ_ACQ)

struct gncore_dma_item {
	uint32_t start_addr;	/* 0x00 */
	uint32_t dma_addr_l;	/* 0x04 */
	uint32_t dma_addr_h;	/* 0x08 */
	uint32_t dma_len;	/* 0x0C */
	uint32_t next_addr_l;	/* 0x10 */
	uint32_t next_addr_h;	/* 0x14 */
	uint32_t attribute;	/* 0x18 */
	uint32_t reserved;	/* ouch */
};

struct ob_dev {
	struct fmc_device *fmc;
	struct zio_device *hwzdev;
	struct zio_device *zdev;

	struct zio_dma_sgt *zdma;

	unsigned int cur_page_size;
	unsigned int last_acq_page;
	unsigned long flags;

	unsigned int errors;
	unsigned int c_err; /**< consectutive errors */
	unsigned int done;

	struct timer_list tm;
	struct spinlock lock;

	/* Base addresses */
	unsigned int base_vic;
	unsigned int base_dma_core;
	unsigned int base_dma_irq;
	unsigned int base_obs_core;
	unsigned int base_obs_irq;
};

struct zio_field_desc {
	unsigned long offset; /* related to its component base */
	uint32_t mask; /* bit mask a register field */
	int is_bitfield; /* whether it maps  full register or a field */
};

enum obsbox_parameters {
	OB_PARM_RUN,
	OB_PARM_STREAM,
};

enum obsbox_registers {
	ACQ_CTRL_TX_DIS,
	ACQ_CTRL_RST_GTP,
	ACQ_CTRL_RST_RX,
	ACQ_CTRL_RST_TX,
	ACQ_CTRL_RST_CDR,
	ACQ_CTRL_RST_ALG,
	ACQ_CTRL_RST_BUF,
	ACQ_PAGE_SIZE,
	ACQ_PAGE_ADDR,
	ACQ_MARK_ADDR,
	IRQ_ACQ_DISABLE_MASK,
	IRQ_ACQ_ENABLE_MASK,
	IRQ_ACQ_MASK_STATUS,
	IRQ_ACQ_SRC,
	IRQ_DMA_DISABLE_MASK,
	IRQ_DMA_ENABLE_MASK,
	IRQ_DMA_MASK_STATUS,
	IRQ_DMA_SRC,
	DMA_CTL_SWP,
	DMA_CTL_ABORT,
	DMA_CTL_START,
	DMA_STA,
	DMA_ADDR,
	DMA_ADDR_L,
	DMA_ADDR_H,
	DMA_LEN,
	DMA_NEXT_L,
	DMA_NEXT_H,
	DMA_BR_DIR,
	DMA_BR_LAST,
};

extern const struct zio_field_desc ob_regs[];
extern struct zio_device ob_tmpl;
extern struct zio_driver ob_driver;

/* obsbox-irq.c */
extern int ob_init_irq(struct ob_dev *ob);
extern void ob_exit_irq(struct ob_dev *ob);
/* obsbox-zio.c*/
extern void ob_acquisition_command(struct ob_dev *ob, uint32_t cmd);


static inline uint32_t ob_readl(struct ob_dev *ob,
				unsigned int base_off,
				const struct zio_field_desc *field)
{
	uint32_t cur;

	cur = fmc_readl(ob->fmc, base_off+field->offset);
	if (field->is_bitfield) {
		/* apply mask and shift right accordlying to the mask */
		cur &= field->mask;
		cur /= (field->mask & -(field->mask));
	} else {
		cur &= field->mask; /* bitwise and with the mask */
	}
	return cur;
}

static inline void ob_writel(struct ob_dev *ob,
				unsigned int base_off,
				const struct zio_field_desc *field,
				uint32_t usr_val)
{
	uint32_t cur, val;

	val = usr_val;
	/* Read current register value first if it's a bitfield */
	if (field->is_bitfield) {
		cur = fmc_readl(ob->fmc, base_off+field->offset);
		/* */
		cur &= ~field->mask; /* clear bits according to the mask */
		val = usr_val * (field->mask & -(field->mask));
		if (val & ~field->mask)
			dev_warn(ob->fmc->hwdev,
				"addr 0x%lx: value 0x%x doesn't fit mask 0x%x\n",
				base_off+field->offset, val, field->mask);
		val &= field->mask;
		val |= cur;
	}
	fmc_writel(ob->fmc, val, base_off+field->offset);
}


static inline void ob_enable_irq(struct ob_dev *ob)
{
	dev_info(ob->fmc->hwdev, "Enable interrupts\n");
	ob_writel(ob, ob->base_obs_irq, &ob_regs[IRQ_ACQ_ENABLE_MASK],
		  OBS_IRQ_ACQ);
	ob_writel(ob, ob->base_dma_irq, &ob_regs[IRQ_DMA_ENABLE_MASK],
		  GNCORE_IRQ_DMA_ALL);

	pr_info("%s:%d CTRL 0x%x IRQ MASK 0x%x, 0x%x PAGE 0x%x\n", __func__, __LINE__,
		ob_readl(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_TX_DIS]),
		ob_readl(ob, ob->base_obs_irq, &ob_regs[IRQ_ACQ_MASK_STATUS]),
		ob_readl(ob, ob->base_dma_irq, &ob_regs[IRQ_DMA_MASK_STATUS]),
		ob_readl(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_SIZE]));
}

static inline void ob_disable_irq(struct ob_dev *ob)
{
	dev_info(ob->fmc->hwdev, "Disable interrupts\n");
	ob_writel(ob, ob->base_obs_irq, &ob_regs[IRQ_ACQ_DISABLE_MASK],
		  OBS_IRQ_ALL);
	ob_writel(ob, ob->base_dma_irq, &ob_regs[IRQ_DMA_DISABLE_MASK],
		  GNCORE_IRQ_DMA_ALL);

	pr_info("%s:%d CTRL 0x%x IRQ MASK 0x%x, 0x%x PAGE 0x%x\n", __func__, __LINE__,
		ob_readl(ob, ob->base_obs_core, &ob_regs[ACQ_CTRL_TX_DIS]),
		ob_readl(ob, ob->base_obs_irq, &ob_regs[IRQ_ACQ_MASK_STATUS]),
		ob_readl(ob, ob->base_dma_irq, &ob_regs[IRQ_DMA_MASK_STATUS]),
		ob_readl(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_SIZE]));
}

static inline int ob_set_page_size(struct ob_dev *ob, uint32_t val)
{
	struct zio_cset *cset = &ob->zdev->cset[0];

	if (val < OB_MIN_PAGE_SIZE || val >= OB_MAX_PAGE_SIZE) {
		dev_err(&cset->zdev->head.dev,
		        "invalid page_size %d, min %d max %d\n",
			val, OB_MIN_PAGE_SIZE, OB_MAX_PAGE_SIZE);
		return -EINVAL;
	}

	ob->cur_page_size = val;
	ob_writel(ob, ob->base_obs_core, &ob_regs[ACQ_PAGE_SIZE],
		  ob->cur_page_size);
	return 0;
}

#endif

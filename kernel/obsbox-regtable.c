/*
 * Copyright (c) CERN 2014
 * Author: Federico Vaga <federico.vaga@cern.ch>
 */

#include "obsbox.h"

const struct zio_field_desc ob_regs[] = {
	/* CSR */
	[ACQ_CTRL_TX_DIS] =      {0x00, 0x00000001, 1},
	[ACQ_PAGE_SIZE] =        {0x18, 0xFFFFFFFF, 0},
	[ACQ_PAGE_ADDR] =        {0x1C, 0xFFFFFFFF, 0},
	[ACQ_MARK_ADDR] =        {0x20, 0xFFFFFFFF, 0},
	/* ACQ IRQ */
	[IRQ_ACQ_DISABLE_MASK] = {0x00, 0x00000003, 0},
	[IRQ_ACQ_ENABLE_MASK] =  {0x04, 0x00000003, 0},
	[IRQ_ACQ_MASK_STATUS] =  {0x08, 0x00000003, 0},
	[IRQ_ACQ_SRC] =	         {0x0C, 0x00000003, 0},
	/* DMA IRQ */
	[IRQ_DMA_DISABLE_MASK] = {0x00, 0x00000003, 0},
	[IRQ_DMA_ENABLE_MASK] =  {0x04, 0x00000003, 0},
	[IRQ_DMA_MASK_STATUS] =  {0x08, 0x00000003, 0},
	[IRQ_DMA_SRC] =	         {0x0C, 0x00000003, 0},
	/* DMA */
	[DMA_CTL_SWP] =		 {0x00, 0x0000000C, 1},
	[DMA_CTL_ABORT] =	 {0x00, 0x00000002, 1},
	[DMA_CTL_START] =	 {0x00, 0x00000001, 1},
	[DMA_STA] =		 {0x04, 0x00000007, 0},
	[DMA_ADDR] =		 {0x08, 0xFFFFFFFF, 0},
	[DMA_ADDR_L] =		 {0x0C, 0xFFFFFFFF, 0},
	[DMA_ADDR_H] =		 {0x10, 0xFFFFFFFF, 0},
	[DMA_LEN] =		 {0x14, 0xFFFFFFFF, 0},
	[DMA_NEXT_L] =		 {0x18, 0xFFFFFFFF, 0},
	[DMA_NEXT_H] =		 {0x1C, 0xFFFFFFFF, 0},
	[DMA_BR_DIR] =		 {0x20, 0x00000002, 1},
	[DMA_BR_LAST] =		 {0x20, 0x00000001, 1},
};

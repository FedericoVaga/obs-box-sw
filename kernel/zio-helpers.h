/*
 * Copyright CERN 2014
 * Author: Federico Vaga <federico.vaga@gmail.com>
 * License: GPL v2
 *
 * handle DMA mapping
 */

#ifndef ZIO_HELPERS_H_
#define ZIO_HELPERS_H_

#include <linux/zio.h>
#include <linux/scatterlist.h>

/**
 * It describe a zio block to be mapped with sg
 * @block: is the block to map
 * @first_nent: it tells the index of the first DMA transfer corresponding to
 *              the start of this block
 * @dev_mem_off: device memory offset where retrieve data for this block
 */
struct zio_blocks_sg {
	struct zio_block *block;
	unsigned int first_nent;
	unsigned long dev_mem_off;
};

/**
 * it describes the DMA sg mapping
 * @hwdev: the low level driver which will do DMA
 * @sg_blocks: one or more blocks to map
 * @n_blocks: number of blocks to map
 * @sgt: scatter gather table
 * @page_desc_size: size of the transfer descriptor
 * @page_desc_pool: vector of transfer descriptors
 * @dma_page_desc_pool: dma address of the vector of transfer descriptors
 */
struct zio_dma_sgt {
	struct zio_channel *chan;
	struct device *hwdev;
	struct zio_blocks_sg *sg_blocks;
	unsigned int n_blocks;
	struct sg_table sgt;
	size_t page_desc_size;
	void *page_desc_pool;
	dma_addr_t dma_page_desc_pool;
};

/**
 * It describes the current page-mapping
 * @zsgt: link to the generic descriptor
 * @sg: scatterlist of the current page
 * @dev_mem_off: device memory offset where start I/O
 * @page_desc: private structure describing the HW page-mapping
 * @block_idx: index of the last mapped block
 * @page_idx: index of the last mapped page
 */
struct zio_dma_sg {
	struct zio_dma_sgt *zsgt;
	struct scatterlist *sg;

	uint32_t dev_mem_off;
	void *page_desc;

	unsigned int block_idx;
	unsigned int page_idx;
};

extern struct zio_dma_sgt *zio_dma_alloc_sg(struct zio_channel *chan,
					    struct device *hwdev,
					    struct zio_block **blocks,
					   unsigned int n_blocks,
					   gfp_t gfp);
extern void zio_dma_free_sg(struct zio_dma_sgt *zdma);
extern int zio_dma_map_sg(struct zio_dma_sgt *zdma, size_t page_desc_size,
			  int (*fill_desc)(struct zio_dma_sg *zsg));
extern void zio_dma_unmap_sg(struct zio_dma_sgt *zdma);

#endif /* ZIO_HELPERS_H_ */

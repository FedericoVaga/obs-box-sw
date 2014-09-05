/*
 * Copyright CERN 2014
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * handle DMA mapping
 */

#ifndef ZIO_HELPERS_H_
#define ZIO_HELPERS_H_

#include <linux/zio.h>
#include <linux/scatterlist.h>

/*
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

/*
 * it describes the DMA sg mapping
 * @hwdev: the low level driver which will do DMA
 * @sg_blocks: one or more blocks to map
 * @n_blocks: number of blocks to map
 * @sgt: scatter gather table
 * @page_desc_size: size of the transfer descriptor
 * @page_desc_pool: vector of transfer descriptors
 * @dma_page_desc_pool: dma address of the vector of transfer descriptors
 */
struct zio_dma_sg {
	struct zio_channel *chan;
	struct device *hwdev;
	struct zio_blocks_sg *sg_blocks;
	unsigned int n_blocks;
	struct sg_table sgt;
	size_t page_desc_size;
	void *page_desc_pool;
	dma_addr_t dma_page_desc_pool;
};

extern struct zio_dma_sg *zio_dma_alloc_sg(struct zio_channel *chan,
					   struct device *hwdev,
					   struct zio_block **blocks,
					   unsigned int n_blocks,
					   gfp_t gfp);
extern void zio_dma_free_sg(struct zio_dma_sg *zdma);
extern int zio_dma_map_sg(struct zio_dma_sg *zdma, size_t page_desc_size,
				int (*fill_desc)(struct zio_dma_sg *zdma,
						int page_idx,
						int block_idx, void *page_desc,
						uint32_t dev_mem_offset,
						struct scatterlist *sg));
extern void zio_dma_unmap_sg(struct zio_dma_sg *zdma);

#endif /* ZIO_HELPERS_H_ */

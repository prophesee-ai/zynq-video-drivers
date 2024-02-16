/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Prophesee Video DMA
 *
 * Copyright (C) Prophesee S.A.
 */

#ifndef PSEE_DMA_H
#define PSEE_DMA_H

#include <linux/dmaengine.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-v4l2.h>

struct dma_chan;
struct psee_composite_device;

/**
 * struct psee_pipeline - Xilinx Video IP pipeline structure
 * @pipe: media pipeline
 * @lock: protects the pipeline @stream_count
 * @use_count: number of DMA engines using the pipeline
 * @stream_count: number of DMA engines currently streaming
 * @num_dmas: number of DMA engines in the pipeline
 * @output: DMA engine at the output of the pipeline
 */
struct psee_pipeline {
	struct media_pipeline pipe;

	struct mutex lock;
	unsigned int use_count;
	unsigned int stream_count;

	unsigned int num_dmas;
	struct psee_dma *output;
};

static inline struct psee_pipeline *to_psee_pipeline(struct media_entity *e)
{
	return container_of(e->pipe, struct psee_pipeline, pipe);
}

/**
 * struct psee_dma - Video DMA interface to PS Host
 * @list: list entry in a composite device dmas list
 * @video: V4L2 video device associated with the DMA channel
 * @pad: media pad for the video device entity
 * @psee_dev: composite device the DMA channel belongs to
 * @pipe: pipeline belonging to the DMA channel
 * @port: composite device DT node port number for the DMA channel
 * @lock: protects the @queue field
 * @queue: vb2 buffers queue
 * @sequence: V4L2 buffers sequence number
 * @transfer_size: Size of the DMA buffers, =maximum transfer size
 * @queued_bufs: list of queued buffers
 * @queued_lock: protects the buf_queued list
 * @dma: DMA engine channel
 * @iomem: Mapping of the IP registers in the kernel space
 * @iosize: size of the mapped register bank (in byte)
 */
struct psee_dma {
	struct list_head list;
	struct video_device video;
	struct media_pad pad;

	struct psee_composite_device *psee_dev;
	struct psee_pipeline pipe;
	unsigned int port;

	struct mutex lock;

	struct vb2_queue queue;
	unsigned int sequence;
	u32 transfer_size;

	struct list_head queued_bufs;
	spinlock_t queued_lock;

	void __iomem *iomem;
	resource_size_t iosize;
	struct dma_chan *dma;
};

#define to_psee_dma(vdev)	container_of(vdev, struct psee_dma, video)

int psee_dma_init(struct psee_composite_device *psee_dev, struct psee_dma *dma,
		  enum v4l2_buf_type type, unsigned int port, struct resource *io_space);
void psee_dma_cleanup(struct psee_dma *dma);

#endif /* PSEE_DMA_H */

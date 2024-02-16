// SPDX-License-Identifier: GPL-2.0-only
/*
 * Prophesee Video DMA
 *
 * Copyright (C) Prophesee S.A.
 */

#include <linux/dma/xilinx_dma.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#include "psee-dma.h"
#include "psee-composite.h"
#include "psee-format.h"

#define PSEE_DMA_DEF_WIDTH		1280
#define PSEE_DMA_DEF_HEIGHT		720

/* Minimum and maximum widths are expressed in bytes */
#define PSEE_DMA_MIN_WIDTH		1U
#define PSEE_DMA_MAX_WIDTH		65535U
#define PSEE_DMA_MIN_HEIGHT		1U
#define PSEE_DMA_MAX_HEIGHT		8191U

#define DEFAULT_PACKET_LENGTH		(1 << 20)

#define REG_PACKETIZER_VERSION		(0x0)
#define REG_PACKETIZER_CONTROL		(0x4)
#define REG_PACKETIZER_PACKET_LENGTH	(0x8)

/*
 * Register related operations
 */
static inline u32 read_reg(struct psee_dma *dma, u32 addr)
{
	return ioread32(dma->iomem + addr);
}

static inline void write_reg(struct psee_dma *dma, u32 addr, u32 value)
{
	iowrite32(value, dma->iomem + addr);
}

/* -----------------------------------------------------------------------------
 * Helper functions
 */

static u32 mediabus_to_pixel(unsigned int code)
{
	u32 pix;

	switch (code) {
	case MEDIA_BUS_FMT_PSEE_EVT2:
		pix = V4L2_PIX_FMT_PSEE_EVT2;
		break;
	case MEDIA_BUS_FMT_PSEE_EVT21ME:
		pix = V4L2_PIX_FMT_PSEE_EVT21ME;
		break;
	case MEDIA_BUS_FMT_PSEE_EVT21:
		pix = V4L2_PIX_FMT_PSEE_EVT21;
		break;
	case MEDIA_BUS_FMT_PSEE_EVT3:
		pix = V4L2_PIX_FMT_PSEE_EVT3;
		break;
	default:
		pix = 0;
		break;
	}
	return pix;
}

static struct v4l2_subdev *
psee_dma_remote_subdev(struct media_pad *local, u32 *pad)
{
	struct media_pad *remote;

	remote = media_entity_remote_pad(local);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return NULL;

	if (pad)
		*pad = remote->index;

	return media_entity_to_v4l2_subdev(remote->entity);
}

static int psee_dma_verify_format(struct psee_dma *dma)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_subdev *subdev;

	/* We don't store format, the link shall just be up */
	subdev = psee_dma_remote_subdev(&dma->pad, &fmt.pad);
	if (subdev == NULL)
		return -EPIPE;

	return 0;
}

/* -----------------------------------------------------------------------------
 * Pipeline Stream Management
 */

/**
 * psee_pipeline_start_stop - Start ot stop streaming on a pipeline
 * @pipe: The pipeline
 * @start: Start (when true) or stop (when false) the pipeline
 *
 * Walk the entities chain starting at the pipeline output video node and start
 * or stop all of them.
 *
 * Return: 0 if successful, or the return value of the failed video::s_stream
 * operation otherwise.
 */
static int psee_pipeline_start_stop(struct psee_pipeline *pipe, bool start)
{
	struct psee_dma *dma = pipe->output;
	struct media_entity *entity;
	struct media_pad *pad;
	struct v4l2_subdev *subdev;
	int ret;

	entity = &dma->video.entity;
	while (1) {
		pad = &entity->pads[0];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		pad = media_entity_remote_pad(pad);
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			break;

		entity = pad->entity;
		subdev = media_entity_to_v4l2_subdev(entity);

		ret = v4l2_subdev_call(subdev, video, s_stream, start);
		if (start && ret < 0 && ret != -ENOIOCTLCMD)
			return ret;
	}

	return 0;
}

/**
 * psee_pipeline_set_stream - Enable/disable streaming on a pipeline
 * @pipe: The pipeline
 * @on: Turn the stream on when true or off when false
 *
 * The pipeline is shared between all DMA engines connect at its input and
 * output. While the stream state of DMA engines can be controlled
 * independently, pipelines have a shared stream state that enable or disable
 * all entities in the pipeline. For this reason the pipeline uses a streaming
 * counter that tracks the number of DMA engines that have requested the stream
 * to be enabled.
 *
 * When called with the @on argument set to true, this function will increment
 * the pipeline streaming count. If the streaming count reaches the number of
 * DMA engines in the pipeline it will enable all entities that belong to the
 * pipeline.
 *
 * Similarly, when called with the @on argument set to false, this function will
 * decrement the pipeline streaming count and disable all entities in the
 * pipeline when the streaming count reaches zero.
 *
 * Return: 0 if successful, or the return value of the failed video::s_stream
 * operation otherwise. Stopping the pipeline never fails. The pipeline state is
 * not updated when the operation fails.
 */
static int psee_pipeline_set_stream(struct psee_pipeline *pipe, bool on)
{
	int ret = 0;

	mutex_lock(&pipe->lock);

	if (on) {
		if (pipe->stream_count == pipe->num_dmas - 1) {
			ret = psee_pipeline_start_stop(pipe, true);
			if (ret < 0)
				goto done;
		}
		pipe->stream_count++;
	} else {
		if (--pipe->stream_count == 0)
			psee_pipeline_start_stop(pipe, false);
	}

done:
	mutex_unlock(&pipe->lock);
	return ret;
}

static int psee_pipeline_validate(struct psee_pipeline *pipe,
				  struct psee_dma *start)
{
	struct media_graph graph;
	struct media_entity *entity = &start->video.entity;
	struct media_device *mdev = entity->graph_obj.mdev;
	unsigned int num_inputs = 0;
	unsigned int num_outputs = 0;
	int ret;

	mutex_lock(&mdev->graph_mutex);

	/* Walk the graph to locate the video nodes. */
	ret = media_graph_walk_init(&graph, mdev);
	if (ret) {
		mutex_unlock(&mdev->graph_mutex);
		return ret;
	}

	media_graph_walk_start(&graph, entity);

	while ((entity = media_graph_walk_next(&graph))) {
		struct psee_dma *dma;

		if (entity->function != MEDIA_ENT_F_IO_V4L)
			continue;

		dma = to_psee_dma(media_entity_to_video_device(entity));

		if (dma->pad.flags & MEDIA_PAD_FL_SINK) {
			pipe->output = dma;
			num_outputs++;
		} else {
			num_inputs++;
		}
	}

	mutex_unlock(&mdev->graph_mutex);

	media_graph_walk_cleanup(&graph);

	/* We need exactly one output and zero or one input. */
	if (num_outputs != 1 || num_inputs > 1)
		return -EPIPE;

	pipe->num_dmas = num_inputs + num_outputs;

	return 0;
}

static void __psee_pipeline_cleanup(struct psee_pipeline *pipe)
{
	pipe->num_dmas = 0;
	pipe->output = NULL;
}

/**
 * psee_pipeline_cleanup - Cleanup the pipeline after streaming
 * @pipe: the pipeline
 *
 * Decrease the pipeline use count and clean it up if we were the last user.
 */
static void psee_pipeline_cleanup(struct psee_pipeline *pipe)
{
	mutex_lock(&pipe->lock);

	/* If we're the last user clean up the pipeline. */
	if (--pipe->use_count == 0)
		__psee_pipeline_cleanup(pipe);

	mutex_unlock(&pipe->lock);
}

/**
 * psee_pipeline_prepare - Prepare the pipeline for streaming
 * @pipe: the pipeline
 * @dma: DMA engine at one end of the pipeline
 *
 * Validate the pipeline if no user exists yet, otherwise just increase the use
 * count.
 *
 * Return: 0 if successful or -EPIPE if the pipeline is not valid.
 */
static int psee_pipeline_prepare(struct psee_pipeline *pipe,
				 struct psee_dma *dma)
{
	int ret;

	mutex_lock(&pipe->lock);

	/* If we're the first user validate and initialize the pipeline. */
	if (pipe->use_count == 0) {
		ret = psee_pipeline_validate(pipe, dma);
		if (ret < 0) {
			__psee_pipeline_cleanup(pipe);
			goto done;
		}
	}

	pipe->use_count++;
	ret = 0;

done:
	mutex_unlock(&pipe->lock);
	return ret;
}

/* -----------------------------------------------------------------------------
 * videobuf2 queue operations
 */

/**
 * struct psee_dma_buffer - Video DMA buffer
 * @buf: vb2 buffer base object
 * @queue: buffer list entry in the DMA engine queued buffers list
 * @dma: DMA channel that uses the buffer
 */
struct psee_dma_buffer {
	struct vb2_v4l2_buffer buf;
	struct list_head queue;
	struct psee_dma *dma;
};

#define to_psee_dma_buffer(vb)	container_of(vb, struct psee_dma_buffer, buf)

static void psee_dma_complete(void *param, const struct dmaengine_result *result)
{
	struct psee_dma_buffer *buf = param;
	struct psee_dma *dma = buf->dma;

	spin_lock(&dma->queued_lock);
	list_del(&buf->queue);
	spin_unlock(&dma->queued_lock);

	buf->buf.field = V4L2_FIELD_NONE;
	buf->buf.sequence = dma->sequence++;
	buf->buf.vb2_buf.timestamp = ktime_get_ns();
	vb2_set_plane_payload(&buf->buf.vb2_buf, 0, dma->transfer_size - result->residue);
	vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_DONE);
}

static int
psee_dma_queue_setup(struct vb2_queue *vq,
		     unsigned int *nbuffers, unsigned int *nplanes,
		     unsigned int sizes[], struct device *alloc_devs[])
{
	struct psee_dma *dma = vb2_get_drv_priv(vq);

	/* Make sure the image size is large enough. */
	if (*nplanes)
		return sizes[0] < dma->transfer_size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = dma->transfer_size;

	return 0;
}

static int psee_dma_buffer_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct psee_dma *dma = vb2_get_drv_priv(vb->vb2_queue);
	struct psee_dma_buffer *buf = to_psee_dma_buffer(vbuf);

	buf->dma = dma;

	return 0;
}

static void psee_dma_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct psee_dma *dma = vb2_get_drv_priv(vb->vb2_queue);
	struct psee_dma_buffer *buf = to_psee_dma_buffer(vbuf);
	struct dma_async_tx_descriptor *desc;
	enum dma_transfer_direction dir;
	dma_addr_t addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	size_t size;
	u32 flags;

	if (dma->queue.type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
		dir = DMA_DEV_TO_MEM;
	} else {
		flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
		dir = DMA_MEM_TO_DEV;
	}

	//size = vb2_plane_size(vb, 0);
	size = dma->transfer_size;

	desc = dmaengine_prep_slave_single(dma->dma, addr, size, dir, flags);
	if (!desc) {
		dev_err(dma->psee_dev->dev, "Failed to prepare DMA transfer\n");
		vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}
	desc->callback_result = psee_dma_complete;
	desc->callback_param = buf;

	spin_lock_irq(&dma->queued_lock);
	list_add_tail(&buf->queue, &dma->queued_bufs);
	spin_unlock_irq(&dma->queued_lock);

	dmaengine_submit(desc);

	if (vb2_is_streaming(&dma->queue))
		dma_async_issue_pending(dma->dma);
}

static int psee_dma_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct psee_dma *dma = vb2_get_drv_priv(vq);
	struct psee_dma_buffer *buf, *nbuf;
	struct psee_pipeline *pipe;
	int ret;

	dma->sequence = 0;

	/*
	 * Start streaming on the pipeline. No link touching an entity in the
	 * pipeline can be activated or deactivated once streaming is started.
	 *
	 * Use the pipeline object embedded in the first DMA object that starts
	 * streaming.
	 */
	pipe = dma->video.entity.pipe
	     ? to_psee_pipeline(&dma->video.entity) : &dma->pipe;

	ret = media_pipeline_start(&dma->video.entity, &pipe->pipe);
	if (ret < 0)
		goto error;

	/* Verify that the configured format matches the output of the
	 * connected subdev.
	 */
	ret = psee_dma_verify_format(dma);
	if (ret < 0)
		goto error_stop;

	ret = psee_pipeline_prepare(pipe, dma);
	if (ret < 0)
		goto error_stop;

	/* Start the DMA engine. This must be done before starting the blocks
	 * in the pipeline to avoid DMA synchronization issues.
	 */
	dma_async_issue_pending(dma->dma);

	/* Start the pipeline. */
	psee_pipeline_set_stream(pipe, true);

	return 0;

error_stop:
	media_pipeline_stop(&dma->video.entity);

error:
	/* Give back all queued buffers to videobuf2. */
	spin_lock_irq(&dma->queued_lock);
	list_for_each_entry_safe(buf, nbuf, &dma->queued_bufs, queue) {
		vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_QUEUED);
		list_del(&buf->queue);
	}
	spin_unlock_irq(&dma->queued_lock);

	return ret;
}

static void psee_dma_stop_streaming(struct vb2_queue *vq)
{
	struct psee_dma *dma = vb2_get_drv_priv(vq);
	struct psee_pipeline *pipe = to_psee_pipeline(&dma->video.entity);
	struct psee_dma_buffer *buf, *nbuf;

	/* Stop the pipeline. */
	psee_pipeline_set_stream(pipe, false);

	/* Stop and reset the DMA engine. */
	dmaengine_terminate_all(dma->dma);

	/* Cleanup the pipeline and mark it as being stopped. */
	psee_pipeline_cleanup(pipe);
	media_pipeline_stop(&dma->video.entity);

	/* Give back all queued buffers to videobuf2. */
	spin_lock_irq(&dma->queued_lock);
	list_for_each_entry_safe(buf, nbuf, &dma->queued_bufs, queue) {
		vb2_buffer_done(&buf->buf.vb2_buf, VB2_BUF_STATE_ERROR);
		list_del(&buf->queue);
	}
	spin_unlock_irq(&dma->queued_lock);
}

static const struct vb2_ops psee_dma_queue_qops = {
	.queue_setup = psee_dma_queue_setup,
	.buf_prepare = psee_dma_buffer_prepare,
	.buf_queue = psee_dma_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = psee_dma_start_streaming,
	.stop_streaming = psee_dma_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int
psee_dma_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct psee_dma *dma = to_psee_dma(vfh->vdev);

	cap->capabilities = dma->psee_dev->v4l2_caps | V4L2_CAP_STREAMING |
			    V4L2_CAP_DEVICE_CAPS;

	strscpy(cap->driver, "psee-dma", sizeof(cap->driver));
	strscpy(cap->card, dma->video.name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%pOFn:%u",
		 dma->psee_dev->dev->of_node, dma->port);

	return 0;
}

static int
__psee_dma_get_format(struct psee_dma *dma, struct v4l2_pix_format *pix)
{
	/* This IP does no format conversion, whatever is requested, output
	 * will be the same as the input
	 */
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_subdev *subdev;
	int ret;

	subdev = psee_dma_remote_subdev(&dma->pad, &fmt.pad);
	if (subdev == NULL)
		return -EPIPE;

	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &fmt);
	if (ret < 0)
		return ret == -ENOIOCTLCMD ? -EINVAL : ret;

	/* "The media bus pixel codes describe image formats as flowing over
	 * physical busses (both between separate physical components and inside
	 * SoC devices). This should not be confused with the V4L2 pixel formats
	 * that describe, using four character codes, image formats as stored in
	 * memory.". Well, here we dump the bus content into memory
	 */
	pix->pixelformat = mediabus_to_pixel(fmt.format.code);
	if (!pix->pixelformat)
		dev_warn(dma->psee_dev->dev,
			"Could not translate format code 0x%x to pixel code\n",
			fmt.format.code);
	v4l2_fill_pix_format(pix, &fmt.format);

	/* The packetizer uses arbitrary transfer size */
	pix->sizeimage = dma->transfer_size;
	/* and there is no per line padding, there isn't even lines */
	pix->bytesperline = 0;
	return 0;
}

static int
psee_dma_enum_format(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct v4l2_fh *vfh = file->private_data;
	struct psee_dma *dma = to_psee_dma(vfh->vdev);
	struct v4l2_pix_format pix;
	int ret;

	// We can only output our input
	if (f->index != 0)
		return -EINVAL;

	ret = __psee_dma_get_format(dma, &pix);
	f->pixelformat = pix.pixelformat;

	return ret;
}

static int
psee_dma_get_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct psee_dma *dma = to_psee_dma(vfh->vdev);

	return __psee_dma_get_format(dma, &format->fmt.pix);
}

static int
psee_dma_try_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct psee_dma *dma = to_psee_dma(vfh->vdev);

	return __psee_dma_get_format(dma, &format->fmt.pix);
}

static int
psee_dma_set_format(struct file *file, void *fh, struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct psee_dma *dma = to_psee_dma(vfh->vdev);

	if (vb2_is_busy(&dma->queue))
		return -EBUSY;

	/* Make sure counter pattern is disabled */
	write_reg(dma, REG_PACKETIZER_CONTROL, 0);
	/* Set packet size to image size in bus words */
	write_reg(dma, REG_PACKETIZER_PACKET_LENGTH, dma->transfer_size / 8);

	return __psee_dma_get_format(dma, &format->fmt.pix);
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int psee_dma_g_register(struct file *file, void *fh, struct v4l2_dbg_register *reg)
{
	struct psee_dma *dev = video_drvdata(file);

	if (reg->match.addr > 0)
		return -EINVAL;

	/* check if the address is aligned */
	if (reg->reg & 3ul)
		return -EINVAL;

	/* check if the provided address is in the mapped space */
	if (reg->reg >= dev->iosize)
		return -EINVAL;

	reg->val = read_reg(dev, reg->reg);
	reg->size = 4;
	return 0;
}

static int psee_dma_s_register(struct file *file, void *fh, const struct v4l2_dbg_register *reg)
{
	struct psee_dma *dev = video_drvdata(file);

	if (reg->match.addr > 0)
		return -EINVAL;

	/* check if the address is aligned */
	if (reg->reg & 3ul)
		return -EINVAL;

	/* check if the provided address is in the mapped space */
	if (reg->reg >= dev->iosize)
		return -EINVAL;

	write_reg(dev, reg->reg, reg->val);
	return 0;
}

static int psee_dma_g_chip_info(struct file *file, void *fh, struct v4l2_dbg_chip_info *chip)
{
	struct psee_dma *dev = video_drvdata(file);

	if (chip->match.addr > 0)
		return -EINVAL;
	strscpy(chip->name, dev->video.v4l2_dev->name, sizeof(chip->name));
	return 0;
}
#endif

static const struct v4l2_ioctl_ops psee_dma_ioctl_ops = {
	.vidioc_querycap		= psee_dma_querycap,
	.vidioc_enum_fmt_vid_cap	= psee_dma_enum_format,
	.vidioc_g_fmt_vid_cap		= psee_dma_get_format,
	.vidioc_s_fmt_vid_cap		= psee_dma_set_format,
	.vidioc_try_fmt_vid_cap		= psee_dma_try_format,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.vidioc_g_register		= psee_dma_g_register,
	.vidioc_s_register		= psee_dma_s_register,
	.vidioc_g_chip_info		= psee_dma_g_chip_info,
#endif
};

/* -----------------------------------------------------------------------------
 * V4L2 file operations
 */

static const struct v4l2_file_operations psee_dma_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

/* -----------------------------------------------------------------------------
 * Video DMA Core
 */

int psee_dma_init(struct psee_composite_device *psee_dev, struct psee_dma *dma,
		  enum v4l2_buf_type type, unsigned int port, struct resource *io_space)
{
	char name[16];
	int ret;

	dma->psee_dev = psee_dev;
	dma->port = port;
	mutex_init(&dma->lock);
	mutex_init(&dma->pipe.lock);
	INIT_LIST_HEAD(&dma->queued_bufs);
	spin_lock_init(&dma->queued_lock);

	/* This is hard-coded for now, te be re-evaluated when supporting planar-formats */
	dma->transfer_size = DEFAULT_PACKET_LENGTH;

	/* Initialize the media entity... */
	dma->pad.flags = type == V4L2_BUF_TYPE_VIDEO_CAPTURE
		       ? MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&dma->video.entity, 1, &dma->pad);
	if (ret < 0)
		goto error;

	/* ... and the video node... */
	dma->video.fops = &psee_dma_fops;
	dma->video.v4l2_dev = &psee_dev->v4l2_dev;
	dma->video.queue = &dma->queue;
	snprintf(dma->video.name, sizeof(dma->video.name), "%pOFn %s %u",
		 psee_dev->dev->of_node,
		 type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? "output" : "input",
		 port);
	dma->video.vfl_type = VFL_TYPE_VIDEO;
	dma->video.vfl_dir = type == V4L2_BUF_TYPE_VIDEO_CAPTURE
			   ? VFL_DIR_RX : VFL_DIR_TX;
	dma->video.release = video_device_release_empty;
	dma->video.ioctl_ops = &psee_dma_ioctl_ops;
	dma->video.lock = &dma->lock;
	dma->video.device_caps = V4L2_CAP_STREAMING;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		dma->video.device_caps |= V4L2_CAP_VIDEO_CAPTURE;
	else
		dma->video.device_caps |= V4L2_CAP_VIDEO_OUTPUT;

	video_set_drvdata(&dma->video, dma);

	/* ... and the buffers queue... */
	/* Don't enable VB2_READ and VB2_WRITE, as using the read() and write()
	 * V4L2 APIs would be inefficient. Testing on the command line with a
	 * 'cat /dev/video?' thus won't be possible, but given that the driver
	 * anyway requires a test tool to setup the pipeline before any video
	 * stream can be started, requiring a specific V4L2 test tool as well
	 * instead of 'cat' isn't really a drawback.
	 */
	dma->queue.type = type;
	dma->queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dma->queue.lock = &dma->lock;
	dma->queue.drv_priv = dma;
	dma->queue.buf_struct_size = sizeof(struct psee_dma_buffer);
	dma->queue.ops = &psee_dma_queue_qops;
	dma->queue.mem_ops = &vb2_dma_contig_memops;
	dma->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
				   | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	dma->queue.dev = dma->psee_dev->dev;
	ret = vb2_queue_init(&dma->queue);
	if (ret < 0) {
		dev_err(dma->psee_dev->dev, "failed to initialize VB2 queue\n");
		goto error;
	}

	/* ... and the DMA channel. */
	snprintf(name, sizeof(name), "port%u", port);
	dma->dma = dma_request_chan(dma->psee_dev->dev, name);
	if (IS_ERR(dma->dma)) {
		ret = PTR_ERR(dma->dma);
		if (ret != -EPROBE_DEFER)
			dev_err(dma->psee_dev->dev, "no VDMA channel found\n");
		goto error;
	}

	/* Map the DMA packetizer registers */
	dma->iomem = devm_ioremap_resource(psee_dev->dev, io_space);
	if (IS_ERR(dma->iomem)) {
		dev_err(dma->psee_dev->dev, "Missing DMA packetizer iomem\n");
		ret = PTR_ERR(dma->iomem);
		goto error;
	}
	dma->iosize = resource_size(io_space);

	/* Make sure counter pattern is disabled */
	write_reg(dma, REG_PACKETIZER_CONTROL, 0);
	/* Set packet size to image size in bus words */
	write_reg(dma, REG_PACKETIZER_PACKET_LENGTH, dma->transfer_size / 8);

	ret = video_register_device(&dma->video, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		dev_err(dma->psee_dev->dev, "failed to register video device\n");
		goto error;
	}

	return 0;

error:
	psee_dma_cleanup(dma);
	return ret;
}

void psee_dma_cleanup(struct psee_dma *dma)
{
	if (video_is_registered(&dma->video))
		video_unregister_device(&dma->video);

	if (!IS_ERR_OR_NULL(dma->dma))
		dma_release_channel(dma->dma);

	media_entity_cleanup(&dma->video.entity);

	mutex_destroy(&dma->lock);
	mutex_destroy(&dma->pipe.lock);
}

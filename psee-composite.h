/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Prophesee Video IP Composite Device
 *
 * Copyright (C) Prophesee S.A.
 */

#ifndef PSEE_COMPOSITE_H
#define PSEE_COMPOSITE_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

/**
 * struct psee_composite_device - Prophesee Video IP device structure
 * @v4l2_dev: V4L2 device
 * @media_dev: media device
 * @dev: (OF) device
 * @notifier: V4L2 asynchronous subdevs notifier
 * @dmas: list of DMA channels at the pipeline output and input
 * @v4l2_caps: V4L2 capabilities of the whole device (see VIDIOC_QUERYCAP)
 * @lock: This is to ensure all dma path entities acquire same pipeline object
 */
struct psee_composite_device {
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	struct platform_device *platform_dev;
	struct device *dev;

	struct v4l2_async_notifier notifier;

	struct list_head dmas;
	u32 v4l2_caps;
};

int psee_graph_pipeline_start_stop(struct psee_composite_device *pdev,
				   struct psee_pipeline *pipe, bool on);

#endif /* PSEE_COMPOSITE_H */

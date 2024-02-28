// SPDX-License-Identifier: GPL-2.0-only
/*
 * Prophesee Video IP Composite Device
 *
 * Copyright (C) Prophesee S.A.
 * Derivated from xilinx-vipp
 */

#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of_reserved_mem.h>

#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#include "psee-dma.h"
#include "psee-composite.h"

/**
 * struct psee_graph_entity - Entity in the video graph
 * @asd: subdev asynchronous registration information
 * @entity: media entity, from the corresponding V4L2 subdev
 * @subdev: V4L2 subdev
 * @streaming: status of the V4L2 subdev if streaming or not
 */
struct psee_graph_entity {
	struct v4l2_async_subdev asd; /* must be first */
	struct media_entity *entity;
	struct v4l2_subdev *subdev;
	bool streaming;
};

static inline struct psee_graph_entity *
to_psee_entity(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct psee_graph_entity, asd);
}

/* -----------------------------------------------------------------------------
 * Graph Management
 */

static struct psee_graph_entity *
psee_graph_find_entity(struct psee_composite_device *pdev,
		       const struct fwnode_handle *fwnode)
{
	struct psee_graph_entity *entity;
	struct v4l2_async_subdev *asd;

	list_for_each_entry(asd, &pdev->notifier.asd_list, asd_list) {
		entity = to_psee_entity(asd);
		if (entity->asd.match.fwnode == fwnode)
			return entity;
	}

	return NULL;
}

static struct psee_graph_entity *
psee_graph_find_entity_from_media(struct psee_composite_device *pdev,
				  struct media_entity *entity)
{
	struct psee_graph_entity *psee_entity;
	struct v4l2_async_subdev *asd;

	list_for_each_entry(asd, &pdev->notifier.asd_list, asd_list) {
		psee_entity = to_psee_entity(asd);
		if (psee_entity->entity == entity)
			return psee_entity;
	}

	return NULL;
}

static int psee_graph_build_one(struct psee_composite_device *pdev,
				struct psee_graph_entity *entity)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct media_entity *local = entity->entity;
	struct media_entity *remote;
	struct media_pad *local_pad;
	struct media_pad *remote_pad;
	struct psee_graph_entity *ent;
	struct v4l2_fwnode_link link;
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	dev_dbg(pdev->dev, "creating links for entity %s\n", local->name);

	while (1) {
		/* Get the next endpoint and parse its link. */
		ep = fwnode_graph_get_next_endpoint(entity->asd.match.fwnode,
						    ep);
		if (ep == NULL)
			break;

		dev_dbg(pdev->dev, "processing endpoint %p\n", ep);

		ret = v4l2_fwnode_parse_link(ep, &link);
		if (ret < 0) {
			dev_err(pdev->dev, "failed to parse link for %p\n",
				ep);
			continue;
		}

		/* Skip sink ports, they will be processed from the other end of
		 * the link.
		 */
		if (link.local_port >= local->num_pads) {
			dev_err(pdev->dev, "invalid port number %u for %p\n",
				link.local_port, link.local_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		local_pad = &local->pads[link.local_port];

		if (local_pad->flags & MEDIA_PAD_FL_SINK) {
			dev_dbg(pdev->dev, "skipping sink port %p:%u\n",
				link.local_node, link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Skip DMA engines, they will be processed separately. */
		if (link.remote_node == of_fwnode_handle(pdev->dev->of_node)) {
			dev_dbg(pdev->dev, "skipping DMA port %p:%u\n",
				link.local_node, link.local_port);
			v4l2_fwnode_put_link(&link);
			continue;
		}

		/* Find the remote entity. */
		ent = psee_graph_find_entity(pdev, link.remote_node);
		if (ent == NULL) {
			dev_err(pdev->dev, "no entity found for %p\n",
				link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		remote = ent->entity;

		if (link.remote_port >= remote->num_pads) {
			dev_err(pdev->dev, "invalid port number %u on %p\n",
				link.remote_port, link.remote_node);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		remote_pad = &remote->pads[link.remote_port];

		v4l2_fwnode_put_link(&link);

		/* Create the media link. */
		dev_dbg(pdev->dev, "creating %s:%u -> %s:%u link\n",
			local->name, local_pad->index,
			remote->name, remote_pad->index);

		ret = media_create_pad_link(local, local_pad->index,
					       remote, remote_pad->index,
					       link_flags);
		if (ret < 0) {
			dev_err(pdev->dev,
				"failed to create %s:%u -> %s:%u link\n",
				local->name, local_pad->index,
				remote->name, remote_pad->index);
			break;
		}
	}

	fwnode_handle_put(ep);
	return ret;
}

static struct psee_dma *
psee_graph_find_dma(struct psee_composite_device *pdev, unsigned int port)
{
	struct psee_dma *dma;

	list_for_each_entry(dma, &pdev->dmas, list) {
		if (dma->port == port)
			return dma;
	}

	return NULL;
}

static int psee_graph_build_dma(struct psee_composite_device *pdev)
{
	u32 link_flags = MEDIA_LNK_FL_ENABLED;
	struct device_node *node = pdev->dev->of_node;
	struct media_entity *source;
	struct media_entity *sink;
	struct media_pad *source_pad;
	struct media_pad *sink_pad;
	struct psee_graph_entity *ent;
	struct v4l2_fwnode_link link;
	struct device_node *ep = NULL;
	struct psee_dma *dma;
	int ret = 0;

	dev_dbg(pdev->dev, "creating links for DMA engines\n");

	while (1) {
		/* Get the next endpoint and parse its link. */
		ep = of_graph_get_next_endpoint(node, ep);
		if (ep == NULL)
			break;

		dev_dbg(pdev->dev, "processing endpoint %pOF\n", ep);

		ret = v4l2_fwnode_parse_link(of_fwnode_handle(ep), &link);
		if (ret < 0) {
			dev_err(pdev->dev, "failed to parse link for %pOF\n",
				ep);
			continue;
		}

		/* Find the DMA engine. */
		dma = psee_graph_find_dma(pdev, link.local_port);
		if (dma == NULL) {
			dev_err(pdev->dev, "no DMA engine found for port %u\n",
				link.local_port);
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		dev_dbg(pdev->dev, "creating link for DMA engine %s\n",
			dma->video.name);

		/* Find the remote entity. */
		ent = psee_graph_find_entity(pdev, link.remote_node);
		if (ent == NULL) {
			dev_err(pdev->dev, "no entity found for %pOF\n",
				to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
			ret = -ENODEV;
			break;
		}

		if (link.remote_port >= ent->entity->num_pads) {
			dev_err(pdev->dev, "invalid port number %u on %pOF\n",
				link.remote_port,
				to_of_node(link.remote_node));
			v4l2_fwnode_put_link(&link);
			ret = -EINVAL;
			break;
		}

		if (dma->pad.flags & MEDIA_PAD_FL_SOURCE) {
			source = &dma->video.entity;
			source_pad = &dma->pad;
			sink = ent->entity;
			sink_pad = &sink->pads[link.remote_port];
		} else {
			source = ent->entity;
			source_pad = &source->pads[link.remote_port];
			sink = &dma->video.entity;
			sink_pad = &dma->pad;
		}

		v4l2_fwnode_put_link(&link);

		/* Create the media link. */
		dev_dbg(pdev->dev, "creating %s:%u -> %s:%u link\n",
			source->name, source_pad->index,
			sink->name, sink_pad->index);

		ret = media_create_pad_link(source, source_pad->index,
					       sink, sink_pad->index,
					       link_flags);
		if (ret < 0) {
			dev_err(pdev->dev,
				"failed to create %s:%u -> %s:%u link\n",
				source->name, source_pad->index,
				sink->name, sink_pad->index);
			break;
		}
	}

	of_node_put(ep);
	return ret;
}

static int psee_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct psee_composite_device *pdev =
		container_of(notifier, struct psee_composite_device, notifier);
	struct psee_graph_entity *entity;
	struct v4l2_async_subdev *asd;
	int ret;

	dev_dbg(pdev->dev, "notify complete, all subdevs registered\n");

	/* Create links for every entity. */
	list_for_each_entry(asd, &pdev->notifier.asd_list, asd_list) {
		entity = to_psee_entity(asd);
		ret = psee_graph_build_one(pdev, entity);
		if (ret < 0)
			return ret;
	}

	/* Create links for DMA channels. */
	ret = psee_graph_build_dma(pdev);
	if (ret < 0)
		return ret;

	ret = v4l2_device_register_subdev_nodes(&pdev->v4l2_dev);
	if (ret < 0)
		dev_err(pdev->dev, "failed to register subdev nodes\n");

	return media_device_register(&pdev->media_dev);
}

static int psee_graph_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *unused)
{
	struct psee_composite_device *pdev =
		container_of(notifier, struct psee_composite_device, notifier);
	struct psee_graph_entity *entity;
	struct v4l2_async_subdev *asd;

	/* Locate the entity corresponding to the bound subdev and store the
	 * subdev pointer.
	 */
	list_for_each_entry(asd, &pdev->notifier.asd_list, asd_list) {
		entity = to_psee_entity(asd);

		if (entity->asd.match.fwnode != subdev->fwnode)
			continue;

		if (entity->subdev) {
			dev_err(pdev->dev, "duplicate subdev for node %p\n",
				entity->asd.match.fwnode);
			return -EINVAL;
		}

		dev_dbg(pdev->dev, "subdev %s bound\n", subdev->name);
		entity->entity = &subdev->entity;
		entity->subdev = subdev;
		return 0;
	}

	dev_err(pdev->dev, "no entity for subdev %s\n", subdev->name);
	return -EINVAL;
}

static const struct v4l2_async_notifier_operations psee_graph_notify_ops = {
	.bound = psee_graph_notify_bound,
	.complete = psee_graph_notify_complete,
};

static int psee_graph_parse_one(struct psee_composite_device *pdev,
				struct fwnode_handle *fwnode)
{
	struct fwnode_handle *remote;
	struct fwnode_handle *ep = NULL;
	int ret = 0;

	dev_dbg(pdev->dev, "parsing node %p\n", fwnode);

	while (1) {
		struct psee_graph_entity *xge;

		ep = fwnode_graph_get_next_endpoint(fwnode, ep);
		if (ep == NULL)
			break;

		dev_dbg(pdev->dev, "handling endpoint %p\n", ep);

		remote = fwnode_graph_get_remote_port_parent(ep);
		if (remote == NULL) {
			ret = -EINVAL;
			goto err_notifier_cleanup;
		}

		fwnode_handle_put(ep);

		/* Skip entities that we have already processed. */
		if (remote == of_fwnode_handle(pdev->dev->of_node) ||
		    psee_graph_find_entity(pdev, remote)) {
			fwnode_handle_put(remote);
			continue;
		}

		xge = v4l2_async_notifier_add_fwnode_subdev(
			&pdev->notifier, remote,
			struct psee_graph_entity);
		fwnode_handle_put(remote);
		if (IS_ERR(xge)) {
			ret = PTR_ERR(xge);
			goto err_notifier_cleanup;
		}
	}

	return 0;

err_notifier_cleanup:
	v4l2_async_notifier_cleanup(&pdev->notifier);
	fwnode_handle_put(ep);
	return ret;
}

static int psee_graph_parse(struct psee_composite_device *pdev)
{
	struct psee_graph_entity *entity;
	struct v4l2_async_subdev *asd;
	int ret;

	/*
	 * Walk the links to parse the full graph. Start by parsing the
	 * composite node and then parse entities in turn. The list_for_each
	 * loop will handle entities added at the end of the list while walking
	 * the links.
	 */
	ret = psee_graph_parse_one(pdev, of_fwnode_handle(pdev->dev->of_node));
	if (ret < 0)
		return 0;

	list_for_each_entry(asd, &pdev->notifier.asd_list, asd_list) {
		entity = to_psee_entity(asd);
		ret = psee_graph_parse_one(pdev, entity->asd.match.fwnode);
		if (ret < 0) {
			v4l2_async_notifier_cleanup(&pdev->notifier);
			break;
		}
	}

	return ret;
}

static int psee_graph_dma_init_one(struct psee_composite_device *pdev,
				   struct device_node *node)
{
	struct psee_dma *dma;
	enum v4l2_buf_type type;
	unsigned int index;
	int ret;

	of_property_read_u32(node, "reg", &index);

	/* Originally there was a direction information on each port, to manage the DMA accordingly,
	 * but now the binding states that there is exactly one port, acting as input.
	 * Another may be added to inject data in the pipeline
	 */
	type = index == 0 ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;

	dma = devm_kzalloc(pdev->dev, sizeof(*dma), GFP_KERNEL);
	if (dma == NULL)
		return -ENOMEM;

	ret = psee_dma_init(pdev, dma, type, index,
		platform_get_resource(pdev->platform_dev, IORESOURCE_MEM, index));
	if (ret < 0) {
		dev_err(pdev->dev, "%pOF initialization failed\n", node);
		return ret;
	}

	list_add_tail(&dma->list, &pdev->dmas);

	pdev->v4l2_caps |= type == V4L2_BUF_TYPE_VIDEO_CAPTURE
			 ? V4L2_CAP_VIDEO_CAPTURE : V4L2_CAP_VIDEO_OUTPUT;

	return 0;
}

static int psee_graph_dma_init(struct psee_composite_device *pdev)
{
	struct device_node *ports;
	struct device_node *port;
	int ret;

	ports = of_get_child_by_name(pdev->dev->of_node, "ports");
	if (ports == NULL) {
		dev_err(pdev->dev, "ports node not present\n");
		return -EINVAL;
	}

	for_each_child_of_node(ports, port) {
		ret = psee_graph_dma_init_one(pdev, port);
		if (ret < 0) {
			of_node_put(port);
			return ret;
		}
	}

	return 0;
}

static void psee_graph_cleanup(struct psee_composite_device *pdev)
{
	struct psee_dma *dmap;
	struct psee_dma *dma;

	v4l2_async_notifier_unregister(&pdev->notifier);
	v4l2_async_notifier_cleanup(&pdev->notifier);

	list_for_each_entry_safe(dma, dmap, &pdev->dmas, list) {
		psee_dma_cleanup(dma);
		list_del(&dma->list);
	}
}

static int psee_graph_init(struct psee_composite_device *pdev)
{
	int ret;

	/* Init the DMA channels. */
	ret = psee_graph_dma_init(pdev);
	if (ret < 0) {
		dev_err(pdev->dev, "DMA initialization failed\n");
		goto done;
	}

	/* Parse the graph to extract a list of subdevice DT nodes. */
	ret = psee_graph_parse(pdev);
	if (ret < 0) {
		dev_err(pdev->dev, "graph parsing failed\n");
		goto done;
	}

	if (list_empty(&pdev->notifier.asd_list)) {
		dev_err(pdev->dev, "no subdev found in graph\n");
		ret = -ENOENT;
		goto done;
	}

	/* Register the subdevices notifier. */
	pdev->notifier.ops = &psee_graph_notify_ops;

	ret = v4l2_async_notifier_register(&pdev->v4l2_dev, &pdev->notifier);
	if (ret < 0) {
		dev_err(pdev->dev, "notifier registration failed\n");
		goto done;
	}

	ret = 0;

done:
	if (ret < 0)
		psee_graph_cleanup(pdev);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Controller and V4L2
 */

static void psee_composite_v4l2_cleanup(struct psee_composite_device *pdev)
{
	v4l2_device_unregister(&pdev->v4l2_dev);
	media_device_unregister(&pdev->media_dev);
	media_device_cleanup(&pdev->media_dev);
}

static int psee_composite_v4l2_init(struct psee_composite_device *pdev)
{
	int ret;

	pdev->media_dev.dev = pdev->dev;
	strscpy(pdev->media_dev.model, "Prophesee Video Pipeline",
		sizeof(pdev->media_dev.model));
	pdev->media_dev.hw_revision = 0;

	media_device_init(&pdev->media_dev);

	pdev->v4l2_dev.mdev = &pdev->media_dev;
	ret = v4l2_device_register(pdev->dev, &pdev->v4l2_dev);
	if (ret < 0) {
		dev_err(pdev->dev, "V4L2 device registration failed (%d)\n",
			ret);
		media_device_cleanup(&pdev->media_dev);
		return ret;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static int psee_composite_probe(struct platform_device *platform_dev)
{
	struct psee_composite_device *pdev;
	int ret;

	pdev = devm_kzalloc(&platform_dev->dev, sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return -ENOMEM;

	pdev->dev = &platform_dev->dev;
	pdev->platform_dev = platform_dev;
	INIT_LIST_HEAD(&pdev->dmas);
	v4l2_async_notifier_init(&pdev->notifier);

	ret = psee_composite_v4l2_init(pdev);
	if (ret < 0)
		return ret;

	ret = psee_graph_init(pdev);
	if (ret < 0)
		goto error;

	ret = of_reserved_mem_device_init(&platform_dev->dev);
	if (ret)
		dev_dbg(&platform_dev->dev, "of_reserved_mem_device_init: %d\n", ret);

	ret = dma_set_mask_and_coherent(&platform_dev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&platform_dev->dev, "dma_set_mask_and_coherent: %d\n", ret);
		goto error;
	}

	platform_set_drvdata(platform_dev, pdev);

	dev_info(pdev->dev, "device registered\n");

	return 0;

error:
	psee_composite_v4l2_cleanup(pdev);
	return ret;
}

static int psee_composite_remove(struct platform_device *platform_dev)
{
	struct psee_composite_device *pdev = platform_get_drvdata(platform_dev);

	psee_graph_cleanup(pdev);
	psee_composite_v4l2_cleanup(pdev);

	return 0;
}

static const struct of_device_id psee_composite_of_id_table[] = {
	{ .compatible = "psee,axi4s-packetizer" },
	{ }
};
MODULE_DEVICE_TABLE(of, psee_composite_of_id_table);

static struct platform_driver psee_composite_driver = {
	.driver = {
		.name = "psee-video",
		.of_match_table = psee_composite_of_id_table,
	},
	.probe = psee_composite_probe,
	.remove = psee_composite_remove,
};

module_platform_driver(psee_composite_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Prophesee");
MODULE_DESCRIPTION("psee-video - media/v4l2 driver for Prophesee video IP");

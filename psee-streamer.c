// SPDX-License-Identifier: GPL-2.0-only
/*
 * Prophesee Generic driver for streaming IPs
 *
 * This driver provides minimal functionality for Prophesee streaming blocks:
 * - Streaming its input to its output un-altered
 * - Clear any internal memory between to streamings
 * - Propagate media format information
 *
 * Copyright (C) Prophesee S.A.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>

#define PAD_SINK 0
#define PAD_SOURCE 1

#define REG_VERSION		(0x0)

#define REG_CONTROL		(0x4)
#define BIT_ENABLE		BIT(0)
#define BIT_BYPASS		BIT(1)
#define BIT_CLEAR		BIT(2)

/**
 * struct psee_streamer - Prophesee generic structure of a streaming IP
 * @subdev: V4L2 subdev
 * @pads: media pads
 * @formats: V4L2 media bus formats
 * @dev: (OF) device
 * @iomem: device I/O register space remapped to kernel virtual memory
 * @clk: video core clock
 */
struct psee_streamer {
	struct v4l2_subdev subdev;
	struct media_pad pads[2];
	struct v4l2_mbus_framefmt formats[2];
	struct device *dev;
	void __iomem *iomem;
	resource_size_t iosize;
	struct clk *clk;
};

static inline struct psee_streamer *to_streamer(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct psee_streamer, subdev);
}

/*
 * Register related operations
 */
static inline u32 read_reg(struct psee_streamer *streamer, u32 addr)
{
	return ioread32(streamer->iomem + addr);
}

static inline void write_reg(struct psee_streamer *streamer, u32 addr, u32 value)
{
	iowrite32(value, streamer->iomem + addr);
}

/*
 * V4L2 Subdevice Video Operations
 */

static int s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct psee_streamer *streamer = to_streamer(subdev);
	u32 control = read_reg(streamer, REG_CONTROL);

	if (!enable) {
		control &= ~BIT_ENABLE;
		control |= BIT_CLEAR;
	} else {
		control &= ~BIT_CLEAR;
		control |= BIT_ENABLE;
	}

	write_reg(streamer, REG_CONTROL, control);

	return 0;
}

/*
 * V4L2 Subdevice Pad Operations
 */

static struct v4l2_mbus_framefmt *
__get_pad_format(struct psee_streamer *streamer, struct v4l2_subdev_state *sd_state,
	unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&streamer->subdev, sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &streamer->formats[pad];
		break;
	default:
		format = NULL;
		break;
	}

	return format;
}

static int enum_mbus_code(struct v4l2_subdev *subdev, struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_mbus_code_enum *code)
{
	struct v4l2_mbus_framefmt *format;

	if (code->index)
		return -EINVAL;

	format = v4l2_subdev_get_try_format(subdev, sd_state, code->pad);

	code->code = format->code;

	return 0;
}

static int get_format(struct v4l2_subdev *subdev, struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_format *fmt)
{
	struct psee_streamer *streamer = to_streamer(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __get_pad_format(streamer, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int set_format(struct v4l2_subdev *subdev, struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_format *fmt)
{
	struct psee_streamer *streamer = to_streamer(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __get_pad_format(streamer, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == PAD_SINK) {
		/* Save the new format */
		*format = fmt->format;
		/* If the IP is not in bypass, someone tempered it, let that someone deal with the
		 * format setting and propagation
		 */
		if (read_reg(streamer, REG_CONTROL) & BIT_BYPASS)
			return 0;
		/* Propagate the format to the source pad */
		format = __get_pad_format(streamer, sd_state, PAD_SOURCE, fmt->which);
		*format = fmt->format;
	} else if ((read_reg(streamer, REG_CONTROL) & BIT_BYPASS)) {
		/* pad is SOURCE and IP is in bypass */
		struct v4l2_mbus_framefmt *input_format;

		input_format = __get_pad_format(streamer, sd_state, PAD_SINK, fmt->which);
		/* in passthrough, output is always the same as input */
		*format = *input_format;
		/* Let the userspace know about it */
		fmt->format = *input_format;
	} else {
		/* Someone with root powers changed the IP configuration, don't try to guess the
		 * actual output and let the root user deal with the settings
		 */
		*format = fmt->format;
	}

	return 0;
}

/*
 * V4L2 Subdevice Operations
 */
static int log_status(struct v4l2_subdev *sd)
{
	struct psee_streamer *streamer = to_streamer(sd);
	struct device *dev = streamer->dev;
	u32 control = read_reg(streamer, REG_CONTROL);

	dev_info(dev, "***** Passthrough driver *****\n");
	dev_info(dev, "Version = 0x%x\n", read_reg(streamer, REG_VERSION));
	dev_info(dev, "Control = %s %s %s(0x%x)\n",
		control & BIT_ENABLE ? "ENABLED" : "DISABLED",
		control & BIT_BYPASS ? "BYPASSED" : "ENGAGED",
		control & BIT_CLEAR ? "CLEARING " : "",
		control);
	dev_info(dev, "I/O space = 0x%llx\n", streamer->iosize);
	return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
	struct psee_streamer *streamer = to_streamer(sd);

	/* check if the address is aligned */
	if (reg->reg & 3ul)
		return -EINVAL;

	/* check if the provided address is in the mapped space */
	if (reg->reg >= streamer->iosize)
		return -EINVAL;

	reg->size = 4;
	reg->val = read_reg(streamer, reg->reg);
	return 0;
}

static int s_register(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg)
{
	struct psee_streamer *streamer = to_streamer(sd);

	/* check if the address is aligned */
	if (reg->reg & 3ul)
		return -EINVAL;

	/* check if the provided address is in the mapped space */
	if (reg->reg >= streamer->iosize)
		return -EINVAL;

	write_reg(streamer, reg->reg, reg->val);
	return 0;
}
#endif

static const struct v4l2_subdev_core_ops core_ops = {
	.log_status = log_status,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = g_register,
	.s_register = s_register,
#endif
};

static const struct v4l2_subdev_video_ops video_ops = {
	.s_stream = s_stream,
};

static const struct v4l2_subdev_pad_ops pad_ops = {
	.enum_mbus_code		= enum_mbus_code,
	.get_fmt		= get_format,
	.set_fmt		= set_format,
	.link_validate		= v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops ops = {
	.core	= &core_ops,
	.video	= &video_ops,
	.pad	= &pad_ops,
};

/*
 * Media Operations
 */

static const struct media_entity_operations media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * Platform Device Driver
 */

static int parse_of(struct psee_streamer *streamer)
{
	struct device *dev = streamer->dev;
	struct device_node *node = dev->of_node;
	struct device_node *ports;
	struct device_node *port;
	u32 port_id;
	int ret;

	ports = of_get_child_by_name(node, "ports");
	if (ports == NULL)
		ports = node;

	/* Get the format description for each pad */
	for_each_child_of_node(ports, port) {
		if (port->name && (of_node_cmp(port->name, "port") == 0)) {
			ret = of_property_read_u32(port, "reg", &port_id);
			if (ret < 0) {
				dev_err(dev, "no reg in DT");
				return ret;
			}

			if (port_id != 0 && port_id != 1) {
				dev_err(dev, "invalid reg in DT");
				return -EINVAL;
			}
		}
	}

	return 0;
}

static int probe(struct platform_device *pdev)
{
	struct psee_streamer *streamer;
	struct v4l2_subdev *subdev;
	struct resource *io_space;
	int ret;

	streamer = devm_kzalloc(&pdev->dev, sizeof(*streamer), GFP_KERNEL);
	if (!streamer)
		return -ENOMEM;

	streamer->dev = &pdev->dev;

	ret = parse_of(streamer);
	if (ret < 0)
		return ret;

	io_space = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	streamer->iomem = devm_ioremap_resource(streamer->dev, io_space);
	if (IS_ERR(streamer->iomem))
		return PTR_ERR(streamer->iomem);
	streamer->iosize = resource_size(io_space);

	streamer->clk = devm_clk_get(streamer->dev, NULL);
	if (IS_ERR(streamer->clk))
		return PTR_ERR(streamer->clk);

	clk_prepare_enable(streamer->clk);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &streamer->subdev;
	v4l2_subdev_init(subdev, &ops);
	/* It may not be the right function, but at least it's pixel in/pixel out */
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV;
	subdev->dev = &pdev->dev;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, streamer);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	streamer->pads[PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	streamer->pads[PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, streamer->pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, streamer);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&subdev->entity);
	clk_disable_unprepare(streamer->clk);
	return ret;
}

static int remove(struct platform_device *pdev)
{
	struct psee_streamer *streamer = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &streamer->subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);

	clk_disable_unprepare(streamer->clk);

	return 0;
}

static const struct of_device_id of_id_table[] = {
	{ .compatible = "psee,passthrough" },
	{ }
};
MODULE_DEVICE_TABLE(of, of_id_table);

static struct platform_driver passthrough_driver = {
	.driver			= {
		.name		= "psee-streamer",
		.of_match_table	= of_id_table,
	},
	.probe			= probe,
	.remove			= remove,
};

module_platform_driver(passthrough_driver);

MODULE_DESCRIPTION("Prophesee Passthrough Driver");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Prophesee Driver for AXI4-Stream tkeep handler
 *
 * See dt-binding for the IP purpose
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

#define REG_CONFIG		(0x8)
#define WORD_ORDER_SWAP		BIT(0)

/**
 * struct psee_tkhdlr - Prophesee generic structure of a streaming IP
 * @subdev: V4L2 subdev
 * @pads: media pads
 * @formats: V4L2 media bus formats
 * @dev: (OF) device
 * @iomem: device I/O register space remapped to kernel virtual memory
 * @clk: video core clock
 */
struct psee_tkhdlr {
	struct v4l2_subdev subdev;
	struct media_pad pads[2];
	struct v4l2_mbus_framefmt formats[2];
	struct device *dev;
	void __iomem *iomem;
	resource_size_t iosize;
	struct clk *clk;
};

static inline struct psee_tkhdlr *to_tkhdlr(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct psee_tkhdlr, subdev);
}

/*
 * Register related operations
 */
static inline u32 read_reg(struct psee_tkhdlr *tkhdlr, u32 addr)
{
	return ioread32(tkhdlr->iomem + addr);
}

static inline void write_reg(struct psee_tkhdlr *tkhdlr, u32 addr, u32 value)
{
	iowrite32(value, tkhdlr->iomem + addr);
}

/*
 * V4L2 Subdevice Video Operations
 */

static int s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct psee_tkhdlr *tkhdlr = to_tkhdlr(subdev);
	u32 control = read_reg(tkhdlr, REG_CONTROL);

	if (!enable) {
		control &= ~BIT_ENABLE;
		control |= BIT_CLEAR;
	} else {
		control &= ~BIT_CLEAR;
		control |= BIT_ENABLE;
	}

	write_reg(tkhdlr, REG_CONTROL, control);

	return 0;
}

/*
 * V4L2 Subdevice Pad Operations
 */

static struct v4l2_mbus_framefmt *
__get_pad_format(struct psee_tkhdlr *tkhdlr, struct v4l2_subdev_state *sd_state,
	unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *format;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		format = v4l2_subdev_get_try_format(&tkhdlr->subdev, sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		format = &tkhdlr->formats[pad];
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
	struct psee_tkhdlr *tkhdlr = to_tkhdlr(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __get_pad_format(tkhdlr, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

static int set_format(struct v4l2_subdev *subdev, struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_format *fmt)
{
	struct psee_tkhdlr *tkhdlr = to_tkhdlr(subdev);
	struct v4l2_mbus_framefmt *format;

	format = __get_pad_format(tkhdlr, sd_state, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	if (fmt->pad == PAD_SINK) {
		u32 config = read_reg(tkhdlr, REG_CONFIG);

		/* Save the new format */
		*format = fmt->format;
		/* Propagate the format to the source pad */
		format = __get_pad_format(tkhdlr, sd_state, PAD_SOURCE, fmt->which);
		*format = fmt->format;
		if ((fmt->format.code == MEDIA_BUS_FMT_PSEE_EVT21ME) &&
			(config & WORD_ORDER_SWAP)) {
			/* The IP is set to convert EVT21ME into actual EVT21 */
			format->code = MEDIA_BUS_FMT_PSEE_EVT21;
		} else {
			/* In any other case, leave the data as-is */
			write_reg(tkhdlr, REG_CONFIG, 0);
		}
	} else {
		struct v4l2_mbus_framefmt *input_format;

		input_format = __get_pad_format(tkhdlr, sd_state, PAD_SINK, fmt->which);
		/* Output format is always mostly input format */
		*format = *input_format;
		if ((input_format->code == MEDIA_BUS_FMT_PSEE_EVT21ME) &&
			(fmt->format.code == MEDIA_BUS_FMT_PSEE_EVT21)) {
			/* Swap the word order to get straight EVT2.1 */
			write_reg(tkhdlr, REG_CONFIG, WORD_ORDER_SWAP);
			format->code = MEDIA_BUS_FMT_PSEE_EVT21;
		} else {
			/* Don't alter the input data */
			write_reg(tkhdlr, REG_CONFIG, 0);
		}
		/* Let the userspace know about it */
		fmt->format = *format;
	}

	return 0;
}

/*
 * V4L2 Subdevice Operations
 */
static int log_status(struct v4l2_subdev *sd)
{
	struct psee_tkhdlr *tkhdlr = to_tkhdlr(sd);
	struct device *dev = tkhdlr->dev;
	u32 control = read_reg(tkhdlr, REG_CONTROL);

	dev_info(dev, "***** Tkeep driver *****\n");
	dev_info(dev, "Version = 0x%x\n", read_reg(tkhdlr, REG_VERSION));
	dev_info(dev, "Control = %s %s %s(0x%x)\n",
		control & BIT_ENABLE ? "ENABLED" : "DISABLED",
		control & BIT_BYPASS ? "BYPASSED" : "ENGAGED",
		control & BIT_CLEAR ? "CLEARING " : "",
		control);
	dev_info(dev, "Config = 0x%x\n", read_reg(tkhdlr, REG_CONFIG));
	return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
	struct psee_tkhdlr *tkhdlr = to_tkhdlr(sd);

	/* check if the address is aligned */
	if (reg->reg & 3ul)
		return -EINVAL;

	/* check if the provided address is in the mapped space */
	if (reg->reg >= tkhdlr->iosize)
		return -EINVAL;

	reg->size = 4;
	reg->val = read_reg(tkhdlr, reg->reg);
	return 0;
}

static int s_register(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg)
{
	struct psee_tkhdlr *tkhdlr = to_tkhdlr(sd);

	/* check if the address is aligned */
	if (reg->reg & 3ul)
		return -EINVAL;

	/* check if the provided address is in the mapped space */
	if (reg->reg >= tkhdlr->iosize)
		return -EINVAL;

	write_reg(tkhdlr, reg->reg, reg->val);
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

static int parse_of(struct psee_tkhdlr *tkhdlr)
{
	struct device *dev = tkhdlr->dev;
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
	struct psee_tkhdlr *tkhdlr;
	struct v4l2_subdev *subdev;
	struct resource *io_space;
	int ret;

	tkhdlr = devm_kzalloc(&pdev->dev, sizeof(*tkhdlr), GFP_KERNEL);
	if (!tkhdlr)
		return -ENOMEM;

	tkhdlr->dev = &pdev->dev;

	ret = parse_of(tkhdlr);
	if (ret < 0)
		return ret;

	io_space = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	tkhdlr->iomem = devm_ioremap_resource(tkhdlr->dev, io_space);
	if (IS_ERR(tkhdlr->iomem))
		return PTR_ERR(tkhdlr->iomem);
	tkhdlr->iosize = resource_size(io_space);

	tkhdlr->clk = devm_clk_get(tkhdlr->dev, NULL);
	if (IS_ERR(tkhdlr->clk))
		return PTR_ERR(tkhdlr->clk);

	clk_prepare_enable(tkhdlr->clk);

	/* Reset registers to a known configuration */
	write_reg(tkhdlr, REG_CONTROL, BIT_CLEAR);
	write_reg(tkhdlr, REG_CONFIG, 0);

	/* Initialize V4L2 subdevice and media entity */
	subdev = &tkhdlr->subdev;
	v4l2_subdev_init(subdev, &ops);
	/* It may not be the right function, but at least it's pixel in/pixel out */
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV;
	subdev->dev = &pdev->dev;
	strscpy(subdev->name, dev_name(&pdev->dev), sizeof(subdev->name));
	v4l2_set_subdevdata(subdev, tkhdlr);
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	tkhdlr->pads[PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	tkhdlr->pads[PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	subdev->entity.ops = &media_ops;
	ret = media_entity_pads_init(&subdev->entity, 2, tkhdlr->pads);
	if (ret < 0)
		goto error;

	platform_set_drvdata(pdev, tkhdlr);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register subdev\n");
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&subdev->entity);
	clk_disable_unprepare(tkhdlr->clk);
	return ret;
}

static int remove(struct platform_device *pdev)
{
	struct psee_tkhdlr *tkhdlr = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &tkhdlr->subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);

	clk_disable_unprepare(tkhdlr->clk);

	return 0;
}

static const struct of_device_id of_id_table[] = {
	{ .compatible = "psee,axis-tkeep-handler" },
	{ }
};
MODULE_DEVICE_TABLE(of, of_id_table);

static struct platform_driver tkeep_driver = {
	.driver			= {
		.name		= "psee-tkeep-hdlr",
		.of_match_table	= of_id_table,
	},
	.probe			= probe,
	.remove			= remove,
};

module_platform_driver(tkeep_driver);

MODULE_DESCRIPTION("Prophesee Tkeep Handler Driver");
MODULE_LICENSE("GPL");

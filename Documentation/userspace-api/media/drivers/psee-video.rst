.. SPDX-License-Identifier: GPL-2.0

.. |PseeVideo| replace:: Prophesee Video Subsystem
.. |PseeProduct| replace:: Prophesee StarterKit for KV260

================================
|PseeVideo| driver
================================

Pipeline configuration
----------------------

The pipeline configuration depends on the actual hardware design, which resides
in a FPGA fabric, and is much more likely to change than on others
system-on-chips.

The |PseeProduct| has a straight pipeline, with hardware IPs meant to work on
EVT 2.1 format. After loading the FPGA design, the media-ctl looks as follows:

.. code-block:: none

	root@xilinx-kv260-starterkit-20222:~# media-ctl -p
	Media controller API version 5.15.36

	Media device information
	------------------------
	driver          psee-video
	model           Prophesee Video Pipeline
	serial
	bus info
	hw revision     0x0
	driver version  5.15.36

	Device topology
	- entity 1: ps_host_if output 0 (1 pad, 1 link)
		    type Node subtype V4L flags 0
		    device node name /dev/video0
		pad0: Sink
			<- "a0050000.event_stream_smart_tra":1 [ENABLED]

	- entity 5: a0010000.mipi_csi2_rx_subsystem (2 pads, 2 links)
		    type V4L2 subdev subtype Unknown flags 0
		    device node name /dev/v4l-subdev0
		pad0: Sink
			[fmt:SRGGB8_1X8/1920x1080 field:none colorspace:srgb]
			<- "imx636 6-003c":0 [ENABLED]
		pad1: Source
			[fmt:SRGGB8_1X8/1920x1080 field:none colorspace:srgb]
			-> "a0040000.axis_tkeep_handler":0 [ENABLED]

	- entity 8: a0050000.event_stream_smart_tra (2 pads, 2 links)
		    type V4L2 subdev subtype Unknown flags 0
		    device node name /dev/v4l-subdev1
		pad0: Sink
			[fmt:unknown/0x0]
			<- "a0040000.axis_tkeep_handler":1 [ENABLED]
		pad1: Source
			[fmt:unknown/0x0]
			-> "ps_host_if output 0":0 [ENABLED]

	- entity 11: a0040000.axis_tkeep_handler (2 pads, 2 links)
		     type V4L2 subdev subtype Unknown flags 0
		     device node name /dev/v4l-subdev2
		pad0: Sink
			[fmt:unknown/0x0]
			<- "a0010000.mipi_csi2_rx_subsystem":1 [ENABLED]
		pad1: Source
			[fmt:unknown/0x0]
			-> "a0050000.event_stream_smart_tra":0 [ENABLED]

	- entity 14: imx636 6-003c (1 pad, 1 link)
		     type V4L2 subdev subtype Sensor flags 0
		     device node name /dev/v4l-subdev3
		pad0: Source
			[fmt:PSEE_EVT3/1280x720 field:none colorspace:raw xfer:none]
			-> "a0010000.mipi_csi2_rx_subsystem":0 [ENABLED]

The easiest way to setup the pipeline is to start from the sensor and propagate
the format information down to the DMA.

.. code-block:: none

	media-ctl -V "'imx636 6-003c':0[fmt:PSEE_EVT21/1280x720]"
	media-ctl -V "'a0010000.mipi_csi2_rx_subsystem':1[fmt:PSEE_EVT21/1280x720]"
	media-ctl -V "'a0040000.axis_tkeep_handler':1[fmt:PSEE_EVT21/1280x720]"
	media-ctl -V "'a0050000.event_stream_smart_tra':1[fmt:PSEE_EVT21/1280x720]"

The pipeline will then look as follow:

.. code-block:: none

	root@xilinx-kv260-starterkit-20222:~# media-ctl -p
	Media controller API version 5.15.36

	Media device information
	------------------------
	driver          psee-video
	model           Prophesee Video Pipeline
	serial
	bus info
	hw revision     0x0
	driver version  5.15.36

	Device topology
	- entity 1: ps_host_if output 0 (1 pad, 1 link)
		    type Node subtype V4L flags 0
		    device node name /dev/video0
		pad0: Sink
			<- "a0050000.event_stream_smart_tra":1 [ENABLED]

	- entity 5: a0010000.mipi_csi2_rx_subsystem (2 pads, 2 links)
		    type V4L2 subdev subtype Unknown flags 0
		    device node name /dev/v4l-subdev0
		pad0: Sink
			[fmt:PSEE_EVT21ME/1280x720 field:none colorspace:raw xfer:none]
			<- "imx636 6-003c":0 [ENABLED]
		pad1: Source
			[fmt:PSEE_EVT21ME/1280x720 field:none colorspace:raw xfer:none]
			-> "a0040000.axis_tkeep_handler":0 [ENABLED]

	- entity 8: a0050000.event_stream_smart_tra (2 pads, 2 links)
		    type V4L2 subdev subtype Unknown flags 0
		    device node name /dev/v4l-subdev1
		pad0: Sink
			[fmt:PSEE_EVT21/1280x720 field:none colorspace:raw xfer:none]
			<- "a0040000.axis_tkeep_handler":1 [ENABLED]
		pad1: Source
			[fmt:PSEE_EVT21/1280x720 field:none colorspace:raw xfer:none]
			-> "ps_host_if output 0":0 [ENABLED]

	- entity 11: a0040000.axis_tkeep_handler (2 pads, 2 links)
		     type V4L2 subdev subtype Unknown flags 0
		     device node name /dev/v4l-subdev2
		pad0: Sink
			[fmt:PSEE_EVT21ME/1280x720 field:none colorspace:raw xfer:none]
			<- "a0010000.mipi_csi2_rx_subsystem":1 [ENABLED]
		pad1: Source
			[fmt:PSEE_EVT21/1280x720 field:none colorspace:raw xfer:none]
			-> "a0050000.event_stream_smart_tra":0 [ENABLED]

	- entity 14: imx636 6-003c (1 pad, 1 link)
		     type V4L2 subdev subtype Sensor flags 0
		     device node name /dev/v4l-subdev3
		pad0: Source
			[fmt:PSEE_EVT21ME/1280x720 field:none colorspace:raw xfer:none]
			-> "a0010000.mipi_csi2_rx_subsystem":0 [ENABLED]

This exemple also shows that an IMX636 sensor can't output fully little-endian
EVT 2.1; when requested to do so, it will output middle-endian EVT 2.1, that the
``axis_tkeep_handler`` will reorder.

For this configuration to work, ``media-ctl`` must be built with Prophesee types
declared in its header (its build uses a copy of the kernel headers, and the
mainline kernel headers don't declare Prophesee formats as of today.

Controls
--------

The |PseeVideo| driver implements the following controls:

``V4L2_CID_XFER_TIMEOUT_ENABLE``
''''''''''''''''''''''''''''''''

This control is held by the V4L2 device (not a sub-device), and allows to
disable or enable DMA transfer completion based on time (based on packetizer
clock, not on timestamps in the data stream). Default is enabled, and without
it, DMA transfers are completed when the buffer in memory is full.

This control is boolean, where true means that the timeout is enabled.

It is defined as

.. code-block:: C

   #define V4L2_CID_XFER_TIMEOUT_ENABLE    (V4L2_CID_USER_BASE | 0x1001)

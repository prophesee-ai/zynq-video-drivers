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

The effect can be shown when there is limited activity in front of the camera.
By default, the timeout is enabled, the buffers will be transferred at a fixed
rate (hence a known latency), with a variable amount of data.

.. code-block:: none

	root@xilinx-kv260-starterkit-20222:~# yavta --capture=10 /dev/video0
	Device /dev/video0 opened.
	Device `ps_host_if output 0' on `platform:ps_host_if:0' is a video output (without mplanes) device.
	Video format: PSE2 (32455350) 1280x720 (stride 0) field none buffer size 1048576
	8 buffers requested.
	length: 1048576 offset: 0 timestamp type/source: mono/EoF
	Buffer 0/0 mapped at address 0xffff98fee000.
	length: 1048576 offset: 1048576 timestamp type/source: mono/EoF
	Buffer 1/0 mapped at address 0xffff98eee000.
	length: 1048576 offset: 2097152 timestamp type/source: mono/EoF
	Buffer 2/0 mapped at address 0xffff98dee000.
	length: 1048576 offset: 3145728 timestamp type/source: mono/EoF
	Buffer 3/0 mapped at address 0xffff98cee000.
	length: 1048576 offset: 4194304 timestamp type/source: mono/EoF
	Buffer 4/0 mapped at address 0xffff98bee000.
	length: 1048576 offset: 5242880 timestamp type/source: mono/EoF
	Buffer 5/0 mapped at address 0xffff98aee000.
	length: 1048576 offset: 6291456 timestamp type/source: mono/EoF
	Buffer 6/0 mapped at address 0xffff989ee000.
	length: 1048576 offset: 7340032 timestamp type/source: mono/EoF
	Buffer 7/0 mapped at address 0xffff988ee000.
	0 (0) [-] none 0 342960 B 781.817092 781.817105 57.727 fps ts mono/EoF
	1 (1) [-] none 1 307224 B 781.834889 781.834898 56.189 fps ts mono/EoF
	2 (2) [-] none 2 259016 B 781.852691 781.852701 56.173 fps ts mono/EoF
	3 (3) [-] none 3 227920 B 781.870489 781.870499 56.186 fps ts mono/EoF
	4 (4) [-] none 4 241544 B 781.888292 781.888303 56.170 fps ts mono/EoF
	5 (5) [-] none 5 223152 B 781.906091 781.906101 56.183 fps ts mono/EoF
	6 (6) [-] none 6 196880 B 781.923889 781.923899 56.186 fps ts mono/EoF
	7 (7) [-] none 7 202640 B 781.941691 781.941700 56.173 fps ts mono/EoF
	8 (0) [-] none 8 189096 B 781.959492 781.959501 56.177 fps ts mono/EoF
	9 (1) [-] none 9 174608 B 781.977292 781.977302 56.180 fps ts mono/EoF
	Captured 10 frames in 0.177533 seconds (56.327469 fps, 13321671.819795 B/s).
	8 buffers released.

Disabling this feature, the transfers will only happen once the buffers are
full, after a time depending on the activity in front of the sensor:

.. code-block:: none

	root@xilinx-kv260-starterkit-20222:~# v4l2-ctl --set-ctrl transfer_timeout_enable=0
	root@xilinx-kv260-starterkit-20222:~# yavta --capture=10 /dev/video0
	Device /dev/video0 opened.
	Device `ps_host_if output 0' on `platform:ps_host_if:0' is a video output (without mplanes) device.
	Video format: PSE2 (32455350) 1280x720 (stride 0) field none buffer size 1048576
	8 buffers requested.
	length: 1048576 offset: 0 timestamp type/source: mono/EoF
	Buffer 0/0 mapped at address 0xffffab633000.
	length: 1048576 offset: 1048576 timestamp type/source: mono/EoF
	Buffer 1/0 mapped at address 0xffffab533000.
	length: 1048576 offset: 2097152 timestamp type/source: mono/EoF
	Buffer 2/0 mapped at address 0xffffab433000.
	length: 1048576 offset: 3145728 timestamp type/source: mono/EoF
	Buffer 3/0 mapped at address 0xffffab333000.
	length: 1048576 offset: 4194304 timestamp type/source: mono/EoF
	Buffer 4/0 mapped at address 0xffffab233000.
	length: 1048576 offset: 5242880 timestamp type/source: mono/EoF
	Buffer 5/0 mapped at address 0xffffab133000.
	length: 1048576 offset: 6291456 timestamp type/source: mono/EoF
	Buffer 6/0 mapped at address 0xffffab033000.
	length: 1048576 offset: 7340032 timestamp type/source: mono/EoF
	Buffer 7/0 mapped at address 0xffffaaf33000.
	0 (0) [-] none 0 1048576 B 1817.555355 1817.555371 22.932 fps ts mono/EoF
	1 (1) [-] none 1 1048576 B 1817.629338 1817.629349 13.517 fps ts mono/EoF
	2 (2) [-] none 2 1048576 B 1817.734335 1817.734348 9.524 fps ts mono/EoF
	3 (3) [-] none 3 1048576 B 1817.854941 1817.854953 8.291 fps ts mono/EoF
	4 (4) [-] none 4 1048576 B 1817.963346 1817.963357 9.225 fps ts mono/EoF
	5 (5) [-] none 5 1048576 B 1818.067741 1818.067752 9.579 fps ts mono/EoF
	6 (6) [-] none 6 1048576 B 1818.179532 1818.179544 8.945 fps ts mono/EoF
	7 (7) [-] none 7 1048576 B 1818.295532 1818.295543 8.621 fps ts mono/EoF
	8 (0) [-] none 8 1048576 B 1818.428342 1818.428353 7.530 fps ts mono/EoF
	9 (1) [-] none 9 1048576 B 1818.596937 1818.596948 5.931 fps ts mono/EoF
	Captured 10 frames in 1.085200 seconds (9.214890 fps, 9662512.468944 B/s).
	May 16 11:38:48 xilinx-kv260-starterkit-20222 last message buffered 1 times
	8 buffers released.

With higher activity, the buffers are released as soon as filled, without
exceeding the timeout value

.. code-block:: none

	root@xilinx-kv260-starterkit-20222:~# v4l2-ctl --set-ctrl transfer_timeout_enable=1
	root@xilinx-kv260-starterkit-20222:~# yavta --capture=10 /dev/video0
	Device /dev/video0 opened.
	Device `ps_host_if output 0' on `platform:ps_host_if:0' is a video output (without mplanes) device.
	Video format: PSE2 (32455350) 1280x720 (stride 0) field none buffer size 1048576
	8 buffers requested.
	length: 1048576 offset: 0 timestamp type/source: mono/EoF
	Buffer 0/0 mapped at address 0xffff830d0000.
	length: 1048576 offset: 1048576 timestamp type/source: mono/EoF
	Buffer 1/0 mapped at address 0xffff82fd0000.
	length: 1048576 offset: 2097152 timestamp type/source: mono/EoF
	Buffer 2/0 mapped at address 0xffff82ed0000.
	length: 1048576 offset: 3145728 timestamp type/source: mono/EoF
	Buffer 3/0 mapped at address 0xffff82dd0000.
	length: 1048576 offset: 4194304 timestamp type/source: mono/EoF
	Buffer 4/0 mapped at address 0xffff82cd0000.
	length: 1048576 offset: 5242880 timestamp type/source: mono/EoF
	Buffer 5/0 mapped at address 0xffff82bd0000.
	length: 1048576 offset: 6291456 timestamp type/source: mono/EoF
	Buffer 6/0 mapped at address 0xffff82ad0000.
	length: 1048576 offset: 7340032 timestamp type/source: mono/EoF
	Buffer 7/0 mapped at address 0xffff829d0000.
	0 (0) [-] none 0 130880 B 873.899345 874.503716 -1.655 fps ts mono/EoF
	1 (1) [-] none 1 1048576 B 874.519127 874.519138 1.613 fps ts mono/EoF
	2 (2) [-] none 2 1048576 B 874.527180 874.527190 124.177 fps ts mono/EoF
	3 (3) [-] none 3 1048576 B 874.534345 874.534354 139.567 fps ts mono/EoF
	4 (4) [-] none 4 1048576 B 874.541505 874.541514 139.665 fps ts mono/EoF
	5 (5) [-] none 5 1048576 B 874.549914 874.549924 118.920 fps ts mono/EoF
	6 (6) [-] none 6 1048576 B 874.557854 874.557864 125.945 fps ts mono/EoF
	7 (7) [-] none 7 1048576 B 874.566743 874.566752 112.499 fps ts mono/EoF
	8 (0) [-] none 8 1048576 B 874.582116 874.582125 65.049 fps ts mono/EoF
	9 (1) [-] none 9 946464 B 874.599847 874.599857 56.398 fps ts mono/EoF
	Captured 10 frames in 0.096148 seconds (104.005368 fps, 98450982.524675 B/s).
	8 buffers released.

This behavior only makes sense with asynchronous data. A frame-based system,
even with compression and variable data rate, would dimension the buffer for
the worst case, and ensure one transfer per frame.

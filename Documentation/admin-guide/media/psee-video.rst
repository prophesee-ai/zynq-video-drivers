.. SPDX-License-Identifier: GPL-2.0

.. include:: <isonum.txt>

.. |PseeVideo| replace:: Prophesee Video Subsystem

================================
|PseeVideo| driver
================================

Supported hardware
------------------

The |PseeVideo| is a design on FPGA (programmable logic), used on
AMD FPGA from the Zynq UltraScale+ MPSoC family.
It is inherently meant to be modified, complexified, to handle new use cases,
such as hardware acceleration of data processing.

The baseline of the design is a straight 64-bit `AXI4-Stream
<https://developer.arm.com/documentation/ihi0051>`_ bus, from a MIPI CSI-2
receiver (designed by `AMD <https://docs.amd.com/r/en-US/pg232-mipi-csi2-rx>`_)
to a DMA packetizer (see `psee-dma`_).

The design is tailored to handle Event-based camera data, but the base
hardware is data agnostic, and should be able to transfer conventional
frame-based data, or abstract data passed over MIPI CSI-2, however the design
does not include any Image Signal Processor to perform the operations usually
needed on colored frames (de-bayer, white-balance, ...).

Driver development and maintenance
----------------------------------

The |PseeVideo| driver suite is currently maintained out-of-tree in
`this repository <https://github.com/prophesee-ai/zynq-video-drivers>`_.

The driver implements V4L2, Media controller and v4l2_subdev interfaces. Camera
sensor using V4L2 subdev interface in the kernel is supported.

The driver has been successfully used with ``v4l2-ctl``, ``yavta``, and a modified
version of `OpenEB <https://github.com/prophesee-ai/openeb>`_, the open-source
subset of `Prophesee <https://www.prophesee.ai/>`_ solution for event-based
vision.

Media device topology
---------------------

The media controller pipeline depends on the bitstream loaded in the FPGA, but
a real-world use case, a Sony IMX636 sensor connected to an AMD Kria KV260
board, looks as follow.

.. graphviz::
   :alt: The output of media-ctl --print-dot

   digraph board {
   	rankdir=TB
   	n00000001 [label="ps_host_if output 0\n/dev/video0", shape=box, style=filled, fillcolor=yellow]
   	n00000005 [label="{{<port0> 0} | a0010000.mipi_csi2_rx_subsystem\n/dev/v4l-subdev0 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
   	n00000005:port1 -> n0000000b:port0
   	n00000008 [label="{{<port0> 0} | a0050000.event_stream_smart_tra\n/dev/v4l-subdev1 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
   	n00000008:port1 -> n00000001
   	n0000000b [label="{{<port0> 0} | a0040000.axis_tkeep_handler\n/dev/v4l-subdev2 | {<port1> 1}}", shape=Mrecord, style=filled, fillcolor=green]
   	n0000000b:port1 -> n00000008:port0
   	n0000000e [label="{{} | imx636 6-003c\n/dev/v4l-subdev3 | {<port0> 0}}", shape=Mrecord, style=filled, fillcolor=green]
   	n0000000e:port0 -> n00000005:port0
   }

The V4L2 subdevices are described hereafter, except the ``imx636`` sensor, which
is mostly independent of this design, even through their drivers were designed
at the same time.

The media controller is handled by the driver named ``psee-video``, whose code
resides in ``psee-composite.c``, and is probed on the platform device compatible
with ``psee,axi4s-packetizer``.

It also creates a V4L2 capture device, with a driver named ``psee-dma`` (in
``psee-dma.c``).

Media formats and V4L2 pixel formats
------------------------------------

Event-Based video does not use raw pixel intensity measurement, but a flow of
events usually indicating the coordinates of the pixel that signaled a change,
the polarity of the change, and the timestamp of the change, often called xyt,
as x and y are the Address Event Representation (AER), and the time is added to
preserve the time resolution when passing on a bus that packetize information,
such as MIPI CSI-2.

To propagate the format information to the userland, event encoding formats were
added as V4L_PIX_FMT.

.. code-block:: C

   #define V4L2_PIX_FMT_PSEE_EVT2 v4l2_fourcc('P', 'S', 'E', 'E')
   #define V4L2_PIX_FMT_PSEE_EVT21ME v4l2_fourcc('P', 'S', 'E', '1')
   #define V4L2_PIX_FMT_PSEE_EVT21 v4l2_fourcc('P', 'S', 'E', '2')
   #define V4L2_PIX_FMT_PSEE_EVT3 v4l2_fourcc('P', 'S', 'E', '3')

Those formats are `documented by Prophesee
<https://docs.prophesee.ai/stable/data/encoding_formats/index.html>`_,
``EVT21ME`` being EVT 2.1 with a middle endian encoding, which happens if an
IMX636 sends EVT 2.1 to a little-endian receiver. Those formats are
single-planar (linear, actually).

Media bus formats were also added. The codes are not declared on upstream
Linux either, they have been chosen to reduce the risk of conflict with new
types that may appear upstream, but are likely to be moved if they are
themself integrated upstream, requiring a rebuild of the userspace with the new
definitions if this happen and a system switch to an upstream kernel.

.. code-block:: C

   #define MEDIA_BUS_FMT_PSEE_EVT2 0x5300
   #define MEDIA_BUS_FMT_PSEE_EVT21ME 0x5301
   #define MEDIA_BUS_FMT_PSEE_EVT21 0x5303
   #define MEDIA_BUS_FMT_PSEE_EVT3 0x5302

There is no padding information in the media bus format because it has only
been used on busses with no padding (MIPI CSI-2 as user-defined data type) or
on busses handling this information (AXI4S with TKEEP signal).

psee-dma
--------

From the userland, the capture is done with a V4L2 capture device, managed by
the ``psee-dma`` driver. Buffers are managed using videobuf2, transfers from
device to memory use a DMA
(`Xilinx AXI DMA <https://docs.amd.com/r/en-US/pg021_axi_dma>`_)
with the dmaengine API.

The way the Prophesee AXI4S packetizer generates transaction is uncommon with
regards to traditional video handling: on a frame-based system, an output
buffer is expected to contain a frame, possibly over several planes, and buffer
completions are expected to follow the framerate.

An event-based system has no other granularity than the event, but managing an
AXI frame per event would be costly in scheduling. Instead, the packetizer
makes frames arbitrarily large (currently hard-coded to 1MB in the driver,
but this should be configurable). This however, comes with a drawback: with no
activity in front of the imager, the output data rate can in theory fall down
to 0 (around 100kB/s with an actual sensor), which can introduce a significant
delay from an event to the completion of the buffer which contains it.

To avoid this behavior (which is an issue for real-time applications), the
packetizer can also trigger the end of a frame based on a timeout, allowing
to bound the system latency. Currently, the driver uses the default value of
the timeout, but creates a V4L2 control allowing to disable this mechanism.

This repacketization is not mandatory: the MIPI CSI-2 bus is also packetized,
and the sensor already made an arbitraty packetization. The packetizer can use
this packetization (which is also what is done on capture device that were not
designed for event-based data), but this is not implemented in the driver, and
requires the packetization information from the sensor to allocate large enough
buffers, and this information is not propagated (on frame-based system, it is
inferred from the pixel array size and the pixel encoding).

psee-csi2rxss
-------------

This driver is a copy of xlnx-csi2rxss driver, barely modified. The Xilinx
flavor of the driver checks at probe that the IP was instantiated with a Video
Format Bridge, that takes the CSI-2 data and outputs one pixel per cycle,
according to the requested pixel layout. The Prophesee flavor does the opposite,
allowing both drivers to coexist in the same system. Without the VFB, the IP
outputs data packed on the full bus width (or uses the bus TKEEP signal to mark
unused bytes), as the Prophesee IPs.

psee-tkeep-hdlr
---------------

This is the driver of the ``axis-tkeep-handler`` IP. The AXI-Stream bus used in
this design carries a TKEEP information to signal partial transactions on the
bus, but, in some tests, the DMA seemed to stop with sparse streams, which
may happen with EVT 2.0 and EVT 3.0, which are narrower than the AXI bus,
allowing packets that are not multiple of the bus width at sensor output, and
introducting partial transfers at csi2rxss output between CSI-2 packets.

With IMX636, EVT 2.1 events can be split in two different CSI-2 packets, and
on little-endian systems, the less significant and the most significant 32 bits
are inverted.

This IP repacks the output of the csi2rxss, and can also swap the 2 halves of
the bus, effectively converting EVT 2.1 ME into regular EVT 2.1, allowing to
run a pipeline in regular EVT 2.1 with an IMX636 and re-packetize other event
formats without passing sparse stream to the DMA.

event_stream_smart_tracker
--------------------------

The stream tracker is a block that generates statistics on the stream, and is
able to drop data, e.g. to prevent overflow of the CSI-2 Rx FIFO, but it is not
used currently, and is kept in bypass mode.

psee-streamer
-------------

This driver is a basic driver compatible with most Prophesee IPs. Its sole
purpose is to put a block in bypass mode (which means that its output is the
same as its input, possibly delayed by a few clock cycles). The driver also
discards skid buffer content, if any, between streams.

From the media controller point of view, an entity driven with ``psee-streamer``
will always output the same media type it has on input.

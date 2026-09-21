:orphan:

.. zephyr:board:: bk7258_evb

Overview
********

The BK7258 is a 2.4 GHz Wi-Fi 6 (IEEE 802.11b/g/n/ax 1x1) and Bluetooth Low
Energy 5.4 combo SoC, built around an Armv8-M STAR-MC1 core running at up to
480 MHz.

The ``bk7258_evb`` board is the Beken evaluation board for this SoC.

Hardware
********

- Beken BK7258 SoC

  - Armv8-M STAR-MC1 at up to 480 MHz, with TrustZone and a double-precision FPU
  - 640 KB of shared SRAM
  - System-in-package flash, 8 MB on this board
  - IEEE 802.11b/g/n/ax 1x1, 2.4 GHz, 20 MHz and 40 MHz channels
  - Bluetooth Low Energy 5.4, 1 Mbps, 2 Mbps and long range

- CH340 USB-UART bridge, connected to UART0 (TX on GPIO11, RX on GPIO10)
- Reset button and boot-mode button

Devicetree
==========

SRAM
----

The SoC has 640 KB of shared SRAM. Zephyr claims the 256 KB region at
``0x28010000``, which is where the bootloader loads the image's data.

Flash
-----

``flash0``
   The code region, at ``0x02150000``, 1156 KB. This is the window from which the
   core fetches instructions, and Zephyr loads the image there. Reads through the
   window are handled by hardware. The size is the application slot that the
   bootloader reserves, not the capacity of the flash device.

``flash_ap``
   The device that backs read, write and erase, at ``0x44030000``, 8 MB. It is
   not driven from this core: the firmware the device ships with owns the
   controller, and the driver asks it to perform each operation and waits for the
   result. Offsets here are physical, and what comes back is the stream as
   stored -- 32 bytes of data followed by 2 bytes of checksum -- because the
   driver neither inserts nor strips those checksum bytes.

   This is the node named by ``zephyr,flash-controller``. It is not another name
   for ``flash0``: ``flash0`` is a window computed from the physical stream, and
   it is valid only where the stream was written in that form.

System Clock
============

The bootloader leaves the core clock at 120 MHz, which is what
``CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`` defaults to. SysTick runs from it.

Serial Port
===========

UART0 is the console, at 115200 baud, 8 data bits, no parity, one stop bit, TX on
GPIO11 and RX on GPIO10, brought out through a CH340 USB-UART bridge.

The port is configured by the firmware the device ships with, which also uses it
as its own console and is the only reader of it. This image only borrows the port
to print, which is why the board enables ``CONFIG_UART_BK7258_NO_HW_INIT``: the
UART driver then registers the device without writing a single register. Running
the normal initialisation (soft reset, pin muxing, divider) would break that
console mid-line and the board would go silent.

For the same reason the shell does not sit on UART0. ``zephyr,shell-uart`` points
at a console transport over the mailbox instead: the firmware forwards a line
typed at its own prompt, and prints what the shell writes back. A shell on UART0
would mean two readers on one port, and the second one would receive nothing.

Requirements
************

Building an image needs nothing beyond Zephyr. Running one needs two things that
are neither built from this tree nor redistributed with it, and that come with
the evaluation board:

- The vendor bootloader. It starts the image, and it owns the partition table and
  the checksums that the vendor packaging tool inserts.
- A prebuilt firmware image that drives the radio, initialises the flash and owns
  UART0. This patch does not replace it, and it is not rebuildable from this tree.

Only the application slot may be erased when programming. Erasing the whole
device removes both of the above together with the radio calibration data.

Programming and Debugging
*************************

Building
========

Build the board bring-up sample:

.. zephyr-app-commands::
   :zephyr-app: samples/boards/beken/bk7258_evb
   :board: bk7258_evb
   :goals: build
   :compact:

For a board that needs no board-specific configuration, any application that
builds for the architecture will do, for example
:zephyr:code-sample:`hello_world`.

Flashing
========

There is no ``west flash`` runner for this board. Programming goes through the
SoC's BootROM UART download protocol, which needs the vendor loader tool on the
host, and the built image has to pass through the vendor packaging tool first:

#. Connect the board's USB-UART bridge to the host.
#. Download the built ``zephyr.bin`` into the application slot with the vendor
   loader, after packaging it with the vendor's packaging tool. The packaging
   tool interleaves a checksum after every 32 bytes and adds the partition table
   the bootloader expects.
#. Open the serial port at 115200 8N1 to see the console output.

See the Requirements section above for what the device has to contain before this
image is programmed into it.

Debugging
=========

The SoC supports SWD, but no runner is configured. See ``board.cmake`` for the
OpenOCD lines to enable if a probe is attached.

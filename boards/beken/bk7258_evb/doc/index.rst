:orphan:

.. zephyr:board:: bk7258_evb

Overview
********

The BK7258 is a Beken SoC with a dual-core Arm Cortex-M33 application
subsystem, Wi-Fi 802.11 b/g/n and Bluetooth LE, plus audio and multimedia
interfaces. The application subsystem is what Zephyr runs on; the SoC's other
core (CPU0) executes vendor firmware that drives the radio and the flash
controller.

The ``bk7258_evb`` board is the Beken evaluation board for this SoC.

Hardware
********

- Beken BK7258 SoC

  - Application subsystem: 2x Arm Cortex-M33
  - 640 KB SRAM in total, shared between the cores
  - 8 MB flash

- CH340 USB-UART bridge, connected to UART0 (TX on GPIO11, RX on GPIO10)
- Reset button and boot-mode button

Devicetree
==========

SRAM
----

The application core may use the SRAM region at ``0x28010000``. 256 KB of it is
claimed by default; the rest of the 640 KB belongs to, or is shared with, the
other core.

Flash
-----

Two flash regions appear in devicetree:

``flash0``
   The code region, at ``0x02150000``. This is the window from which the CPU
   fetches instructions, and Zephyr loads the image there. Reads through the
   window are handled by hardware.

There is no flash controller driver yet, so no device node for the controller
exists, and ``zephyr,flash-controller`` is not set.

System Clock
============

The bootloader leaves the CPU clock at 120 MHz, which is what
``CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`` defaults to. SysTick runs from the CPU
clock.

Serial Port
===========

UART0 is the console, at 115200 baud, 8 data bits, no parity, one stop bit.

Note that on this board UART0 is owned by the SoC's other core, which has
already configured it as its own console. The application core only borrows the
port to print, which is why the board enables
``CONFIG_UART_BK7258_NO_HW_INIT``: the UART driver then registers the device
without writing a single register. If it ran its normal initialisation (soft
reset, pin muxing, divider) it would break the other core's console mid-line
and the board would go silent.

Programming and Debugging
*************************

Building
========

Build the :zephyr:code-sample:`hello_world` sample application:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: bk7258_evb
   :goals: build
   :compact:

Flashing
========

There is no ``west flash`` runner for this board. Programming goes through the
SoC's BootROM UART download protocol, which needs the vendor loader tool on the
host, and the built image has to pass through the vendor packaging tool first:

#. Connect the board's USB-UART bridge to the host.
#. Download the built ``zephyr.bin`` into the application slot with the vendor
   loader, after packaging it with the vendor's packaging tool. The packaging
   tool interleaves a checksum after every 32 bytes and adds the partition
   table the bootloader expects.
#. Open the serial port at 115200 8N1 to see the console output.

Because the image has to be programmed into a slot that still contains the
vendor bootloader and the other core's firmware, only the application slot may
be erased; erasing the whole device also removes the radio calibration data and
the board will no longer be able to use the radio.

Debugging
=========

The SoC supports SWD, but no runner is configured. See ``board.cmake`` for the
OpenOCD lines to enable if a probe is attached.

.. zephyr:code-sample:: am62l_lpm
   :name: AM62L S2RAM Low Power Mode
   :relevant-api: pm_cpu_ops counter_interface

   Demonstrate RTC-woken Suspend-to-RAM (S2RAM) on the TI AM62L EVM.

Overview
********

This sample exercises the Suspend-to-RAM path on the TI AM62L SoC and
validates that the I2C controller (OMAP I2C) is correctly re-initialized
after resume via its ``PM_DEVICE_ACTION_TURN_ON`` handler.

Each cycle:

1. Reads 3 bytes from the AT24 EEPROM on ``main_i2c0`` and caches them in
   a DDR buffer (pre-suspend I2C check).  The AT24 WP pin is hardwired HIGH
   on the AM62L EVM so the existing board-ID bytes are used as the test
   pattern.
2. Arms a wakeup alarm on the always-on RTC (``wkup_rtc0``).
3. Forces ``PM_STATE_SUSPEND_TO_RAM`` via :c:func:`pm_state_force`.
4. The idle thread calls ``pm_state_set()`` which saves CPU context via
   :c:func:`arch_pm_s2ram_suspend` and issues ``PSCI_CPU_SUSPEND``.
   CPU1 is powered off via ``PSCI_CPU_OFF`` first.
   ``CONFIG_PM_DEVICE_SYSTEM_MANAGED`` issues ``PM_DEVICE_ACTION_TURN_OFF``
   on all devices before entry.  TF-A selects the system LPM mode.
5. TF-A gates clocks, puts DDR into self-refresh, and powers down both A53
   cores.
6. After the RTC alarm fires, TF-A resumes the A53 at
   ``arch_pm_s2ram_resume``, which restores CPU context and returns to the
   ``pm_state_set()`` call site.  ``PM_DEVICE_ACTION_TURN_ON`` is issued to
   all devices — including ``i2c_omap``, which re-programs the I2C
   controller registers that were lost when the MAIN power domain was cut.
   CPU1 is then restarted via :c:func:`k_smp_cpu_resume`.
7. Reads back the EEPROM and verifies the 3-byte pattern matches the
   pre-suspend DDR snapshot (post-resume I2C check).  A failure here means
   ``i2c_omap`` ``TURN_ON`` is broken.

A counter in DDR (``cycles``) is incremented before each suspend.  Its
value surviving the suspend/resume cycle proves that DDR contents are
retained across self-refresh.

Requirements
************

* TI AM62L EVM (``am62l_evm/am62l3/a53``)
* SD card loaded with TI's WIC image (provides TF-A + U-Boot)

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/ti/am62l_lpm
   :board: am62l_evm/am62l3/a53
   :goals: build

Flashing
========

1. Flash TI's official WIC image to an SD card using an etching tool
   (e.g., balenaEtcher).
2. Copy ``build/zephyr/zephyr.bin`` to the first FAT partition of the SD card.
3. Plug the SD card into the EVM.  Connect the USB-UART cable and open a
   terminal at 115200 8N1.
4. Power on the board and stop U-Boot at the prompt.
5. At the U-Boot prompt, load and start Zephyr::

      fatload mmc 1:1 0x82000000 zephyr.bin
      dcache flush; icache flush; dcache off; icache off
      go 0x82000000

Expected Output
***************

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   [00:00:00.000] <inf> am62l_lpm: AM62L S2RAM demo — RTC wakeup, I2C verify
   [00:00:00.000] <inf> am62l_lpm: DDR-retained cycle counter at boot: 0
   [00:00:00.000] <inf> am62l_lpm: --- Cycle 1 ---
   [00:00:00.001] <inf> am62l_lpm: I2C pre-suspend: read  {xx xx xx} — cached in DDR
   [00:00:00.001] <inf> am62l_lpm: RTC alarm set for 5 s, entering S2RAM...
   <console goes silent — both CPUs off, DDR in self-refresh>
   <~5 seconds later RTC fires, TF-A resumes A53, TURN_ON re-inits I2C>
   [00:00:05.xxx] <inf> am62l_lpm: Resumed from S2RAM (DDR-retained counter: 1)
   [00:00:05.xxx] <inf> am62l_lpm: I2C post-resume: read  {xx xx xx} — OK (TURN_ON verified)
   [00:00:05.xxx] <inf> am62l_lpm: --- Cycle 2 ---
   ...

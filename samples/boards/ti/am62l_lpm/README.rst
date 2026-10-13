.. zephyr:code-sample:: am62l_lpm
   :name: AM62L S2RAM Low Power Mode
   :relevant-api: pm_cpu_ops subsys_pm_sys

   Demonstrate RTC-woken Suspend-to-RAM (S2RAM) with both A53 cores active.

Overview
********

This sample exercises the Suspend-to-RAM path on the TI AM62L SoC with
both A53 cores participating: CPU1 runs an independent demo task and is
powered off via PSCI CPU_OFF before CPU0 forces its own PSCI
SYSTEM_SUSPEND.

CPU1 runs ``cpu1_task``, which just prints "I am alive" every 1 s, and
is pinned to CPU1 at boot via :c:func:`k_thread_cpu_mask_enable`. On each
suspend cycle, CPU0 creates a second CPU1-pinned, highest-priority
thread that calls :c:func:`pm_state_set` directly to power CPU1 off via
PSCI CPU_OFF. This is an ordinary priority preemption, so ``cpu1_task``'s
context is saved through the normal thread-switch path before CPU1 goes
down, and it resumes from there once CPU1 is restarted.

Each cycle:

1. Arms a wakeup alarm on the always-on RTC (``wkup_rtc0``).
2. CPU0 creates a highest-priority, CPU1-pinned control thread that
   calls :c:func:`pm_state_set` directly, which calls :c:func:`pm_cpu_off`
   for CPU1 (PSCI CPU_OFF; never returns on success).
3. CPU0 forces ``PM_STATE_SUSPEND_TO_RAM`` for itself. Its idle thread
   consumes the forced state and calls ``pm_state_set()``, which first
   polls :c:func:`psci_affinity_info` to confirm CPU1 reached AFF_STATE_OFF
   (on timeout the cycle is skipped), then saves CPU context via
   :c:func:`arch_pm_s2ram_suspend` and issues PSCI SYSTEM_SUSPEND.
   Devices are not suspended by the PM core in this flow: CPU1 is powered
   off directly, so it never leaves the active-CPU count and the
   system-managed device suspend does not run.
4. TF-A gates clocks, puts DDR into self-refresh, and powers down both
   A53 cores.
5. After the RTC alarm fires, TF-A resumes CPU0 at
   ``arch_pm_s2ram_resume``, which restores CPU context and returns to
   the ``pm_state_set()`` call site.
6. ``pm_state_set()`` on CPU0 calls :c:func:`k_smp_cpu_start` for CPU1,
   which restarts it; ``cpu1_task`` then continues where it was preempted.

A counter in DDR (``cycles``) is incremented before each suspend. Its
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
3. Plug the SD card into the EVM. Connect the USB-UART cable and open a
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
   [00:00:00.000] <inf> am62l_lpm: AM62L S2RAM demo - RTC wakeup
   [00:00:00.000] <inf> am62l_lpm: DDR-retained cycle counter at boot: 0
   [00:00:00.000] <inf> am62l_lpm: --- Cycle 1 ---
   CPU1: I am alive (0)
   [00:00:00.001] <inf> am62l_lpm: RTC alarm set for 10 s, entering S2RAM...
   CPU1: I am alive (1)
   [00:00:00.010] <inf> am62l_lpm: Going into S2RAM...
   <console goes silent -- both CPUs off, DDR in self-refresh>
   <~10 seconds later RTC fires, TF-A resumes A53>
   [00:00:30.xxx] <inf> am62l_lpm: Resumed from S2RAM (DDR-retained counter: 1)
   CPU1: I am alive (2)
   [00:00:30.xxx] <inf> am62l_lpm: --- Cycle 2 ---
   ...
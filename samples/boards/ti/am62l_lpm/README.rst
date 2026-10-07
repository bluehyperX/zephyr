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

CPU1 runs ``cpu1_task``, which just prints "I am alive" every 500 ms, and
is pinned to CPU1 at boot via :c:func:`k_thread_cpu_mask_enable`. A second
CPU1-pinned, highest-priority thread (``cpu1_pm_ctrl``, in
:zephyr_file:`soc/ti/k3/am62lx/power.c`) blocks on a semaphore and, when
signaled, calls :c:func:`pm_state_set` directly to power CPU1 off via
PSCI CPU_OFF — this is a normal cooperative-scheduler preemption, so
``cpu1_task``'s context is saved through the ordinary thread-switch path
before CPU1 goes down, and resumes correctly afterward.

Each cycle:

1. Arms a wakeup alarm on the always-on RTC (``wkup_rtc0``).
2. CPU0 forces ``PM_STATE_SUSPEND_TO_RAM`` for CPU1 and gives
   ``cpu1_pm_sem``, waking ``cpu1_pm_ctrl``.
3. ``cpu1_pm_ctrl`` calls :c:func:`pm_state_set` directly, which calls
   :c:func:`pm_cpu_off` for CPU1 (PSCI CPU_OFF; never returns on success).
4. CPU0 calls ``am62l_wait_cpu1_off()`` (a bounded, sleepable poll of
   :c:func:`psci_affinity_info`) to confirm CPU1 actually reached
   AFF_STATE_OFF before risking its own suspend. On timeout the cycle is
   skipped and retried later — see the comment above
   ``am62l_wait_cpu1_off()`` in power.c for why this check cannot be
   skipped or folded into TF-A's own internal poll.
5. CPU0 forces ``PM_STATE_SUSPEND_TO_RAM`` for itself. Its idle thread
   consumes the forced state and calls ``pm_state_set()``, which saves
   CPU context via :c:func:`arch_pm_s2ram_suspend` and issues PSCI
   SYSTEM_SUSPEND. ``CONFIG_PM_DEVICE_SYSTEM_MANAGED`` issues
   ``PM_DEVICE_ACTION_TURN_OFF`` on all devices before entry.
6. TF-A gates clocks, puts DDR into self-refresh, and powers down both
   A53 cores.
7. After the RTC alarm fires, TF-A resumes CPU0 at
   ``arch_pm_s2ram_resume``, which restores CPU context and returns to
   the ``pm_state_set()`` call site. ``PM_DEVICE_ACTION_TURN_ON`` is
   issued to all devices.
8. CPU0 calls :c:func:`k_smp_cpu_start` for CPU1, which recreates
   ``cpu1_pm_ctrl`` (via ``cpu1_pm_ctrl_restart()``) ready for the next
   cycle, and resumes ``cpu1_task`` from where it was preempted.

A counter in DDR (``cycles``) is incremented before each suspend. Its
value surviving the suspend/resume cycle proves that DDR contents are
retained across self-refresh.

Requirements
************

* TI AM62L EVM (``am62l_evm/am62l3/a53``)
* SD card loaded with TI's WIC image (provides TF-A + U-Boot)

Building and Running
*********************

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
****************

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   [00:00:00.000] <inf> am62l_lpm: AM62L S2RAM demo — RTC wakeup
   [00:00:00.000] <inf> am62l_lpm: DDR-retained cycle counter at boot: 0
   [00:00:00.000] <inf> am62l_lpm: --- Cycle 1 ---
   CPU1: I am alive
   [00:00:00.001] <inf> am62l_lpm: RTC alarm set for 10 s, entering S2RAM...
   CPU1: I am alive
   [00:00:00.010] <inf> am62l_lpm: Going into S2RAM...
   <console goes silent — both CPUs off, DDR in self-refresh>
   <~10 seconds later RTC fires, TF-A resumes A53, TURN_ON re-inits devices>
   [00:00:30.xxx] <inf> am62l_lpm: Resumed from S2RAM (DDR-retained counter: 1)
   CPU1: I am alive
   [00:00:30.xxx] <inf> am62l_lpm: --- Cycle 2 ---
   ...

# Kernel module ASSERT test cases

This directory holds one patch per kernel module. Each patch adds a negative
test case which drives the module into the state that its own `ASSERT()` /
`DEBUGASSERT()` is there to catch, so that running the test case hits the
assertion and the board takes the crash dump path.

| Patch | Module | Assertion hit | Kconfig |
| ----- | ------ | ------------- | ------- |
| 0001 | `os/kernel/group` | `group_foreachchild.c` `DEBUGASSERT(group)` | `TC_KERNEL_ASSERT_GROUP` |
| 0002 | `os/kernel/irq` | `irq_spinlock.c` `DEBUGASSERT(0 < g_irq_spin_count[me])`, or `irq_unexpectedisr.c` `PANIC()` on a single core build | `TC_KERNEL_ASSERT_IRQ` |
| 0003 | `os/kernel/environ` | `env_release.c` `DEBUGASSERT(group)` | `TC_KERNEL_ASSERT_ENVIRON` |
| 0004 | `os/kernel/paging` | `pg_miss.c` `DEBUGASSERT(g_pgworker != ftcb->pid)` | `TC_KERNEL_ASSERT_PAGING` |
| 0005 | `os/kernel/binary_manager` | `binary_manager_recovery.c` `ASSERT(sem != NULL && sem->semcount < 0)` | `TC_KERNEL_ASSERT_BINARY_MANAGER` |
| 0006 | `os/kernel/init` | `os_bringup.c` `ASSERT(pid > 0)` | `TC_KERNEL_ASSERT_INIT` |
| 0007 | `os/kernel/debug` | `mem_leak_checker.c` `ASSERT(ctcb != NULL)` | `TC_KERNEL_ASSERT_DEBUG` |
| 0008 | `os/kernel/clock` | `clock_systimespec.c` `DEBUGASSERT(ts->tv_sec >= g_basetime.tv_sec)` | `TC_KERNEL_ASSERT_CLOCK` |
| 0009 | `os/kernel/log_dump` | `log_dump.c` `ASSERT(log_dump_tail)` | `TC_KERNEL_ASSERT_LOG_DUMP` |

## How the test cases are built

Seven of the nine assertions are reached from a running system. The trigger
lives on the kernel side in `os/drivers/os_api_test/kernel/test_assert_*.c`
and is reached through a new ioctl of the existing `/dev/os_api_test` driver,
the same path the kernel test cases already use for kernel only APIs. The
user side test case is a normal `le_tc/kernel` test case which issues that
ioctl and reports a failure if the ioctl ever returns.

Two assertions cannot be reached that way and are handled differently.

* `os/kernel/binary_manager` and `os/kernel/log_dump` guard state which only
  a `static` function touches. Each patch adds a test only entry point next
  to that function, built in only when the test case is enabled.
* Every function of `os/kernel/init` is `static inline` and runs only from
  `os_start()`, so its assertion can only be hit while the board is coming
  up. The patch injects the failure at the call site: the application main
  task is asked for a stack no target can allocate, `task_create()` fails and
  `ASSERT(pid > 0)` is executed before user space ever starts.

## How to run

Apply the series, or a single patch, on top of the branch under test:

    git am 0001-os-kernel-group-*.patch

Enable **exactly one** test case, because each of them ends the boot:

    Application Configuration
      -> Examples
        -> Kernel TestCase Example
          -> [*] Group module ASSERT test

`CONFIG_DEBUG` is required for the `DEBUGASSERT()` based test cases (0001,
0002, 0003, 0004, 0008); the others use `ASSERT()`, which is always built in.
The remaining dependencies are declared in the Kconfig entries:
`PAGING` for 0004, `BINMGR_RECOVERY` for 0005, a flat build for 0006,
`MEM_LEAK_CHECKER` for 0007, `RTC_HIRES` for 0008 and `LOG_DUMP` for 0009.

Build, flash and run `kernel_tc`. The expected result is an assertion with
the file and line of the target assertion in the crash dump. The user side
test case only reports `FAIL`, which means the assertion was **not** hit.

## Review of the assertions in these modules

The assertions were reviewed before the test cases were written. Most of
them are correct preconditions on kernel internal state and are reachable
only through a caller contract violation, which is what the test cases
reproduce. Four findings are worth reporting separately, none of which is
addressed by these patches.

1. `os/kernel/clock/clock_initialize.c`

        DEBUGASSERT(clock_getinittime(&ts) == OK);
        (void)up_rtc_settime(&ts);

   `DEBUGASSERT()` expands to nothing when `CONFIG_DEBUG` is disabled, so on
   a release build `clock_getinittime()` is never called and `up_rtc_settime()`
   sets the RTC from an uninitialized stack variable. The call has to be made
   outside the assertion and only its result asserted on.

2. `os/kernel/clock/clock_time2ticks.c`

        #if 0     // overkill
        DEBUGASSERT(reltime->tv_sec < 2147487 || ...);
        #endif

   The assertion is disabled, so the range check it describes never runs.
   The 32 bit path it guards really does overflow `relusec` above 2147487
   seconds, so it should either be restored or the overflow handled.

3. `os/kernel/init/os_bringup.c`

   `int pid;` in `os_do_appstart()` is only assigned inside optional
   `#ifdef` blocks. In a configuration where `CONFIG_APP_BINARY_SEPARATION`
   is set and `CONFIG_BINARY_MANAGER` is not, the closing `ASSERT(pid > 0)`
   reads an uninitialized variable. Initializing `pid` at its declaration
   makes the assertion mean what it says.

4. `os/kernel/binary_manager/binary_manager_recovery.c`

        ret = mq_send(binary_manager_get_mqfd(), ...);
        ASSERT(ret == OK);

   `mq_send()` can fail at runtime, for instance on a full queue, so this
   turns a recoverable condition into a board reset. The failure should be
   reported and retried instead. The `leave_critical_section(flags)` after
   the enclosing `while (1)` in the same function is unreachable.

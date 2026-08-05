# NuttX and TizenRT schedulers — comparative analysis

## What TizenRT is missing, and what is worth taking

| Field | Value |
| --- | --- |
| Subject | Feature and architecture comparison of the two schedulers |
| TizenRT scope | `os/kernel/sched/`, `os/kernel/irq/`, `os/kernel/Kconfig`, tree-wide `Kconfig*` |
| NuttX scope | `apache/nuttx@master`: `sched/Kconfig`, `sched/sched/Make.defs`, `sched.h`, `sched_roundrobin.c`, `sched_lock.c` |
| Reference platform | rtl8730e / loadable_ext_ddr — `CONFIG_SMP_NCPUS=2`, `CONFIG_RR_INTERVAL=10` |
| Related documents | `PR7465_SMP_RR_REVIEW.md`, `PR7465_REVIEW_COMMENTS.md`, `TizenRT_Scheduling_Scenario_Test_Catalogue.docx` |
| Status | Analysis for review |

---

## Table of contents

1. [Headline finding](#1-headline-finding-nuttx-already-fixed-the-defect-pr-7465-addresses)
2. [The deepest divergence: the global scheduler lock](#2-the-deepest-divergence-the-global-scheduler-lock)
3. [Cross-CPU strategy: pause versus call](#3-cross-cpu-strategy-pause-versus-call)
4. [Feature inventory: present in NuttX, absent from TizenRT](#4-feature-inventory-present-in-nuttx-absent-from-tizenrt)
5. [Source file comparison](#5-source-file-comparison)
6. [Mapping to the scenario test suite](#6-mapping-to-the-scenario-test-suite)
7. [What the scenarios reveal about observability](#7-what-the-scenarios-reveal-about-observability)
8. [Recommendation](#8-recommendation)
9. [Method and caveats](#9-method-and-caveats)

---

## 1. Headline finding: NuttX already fixed the defect PR 7465 addresses

NuttX factored round-robin timeslice handling out of the timer path into
`sched/sched/sched_roundrobin.c`. The SMP branch of that file reads:

```c
#ifdef CONFIG_SMP
  /* In SMP mode, the running task is in g_assignedtasks[cpu], not
   * in the ready-to-run list.  Therefore, tcb->flink is NULL and
   * we cannot check it to find the next task.  If the task is
   * running on a different CPU, send an SMP call to that CPU.
   * Otherwise, directly call nxsched_switch_running() to find the
   * next eligible task from the ready-to-run list and switch to it.
   */

  DEBUGASSERT(tcb->task_state == TSTATE_TASK_RUNNING);
  if (tcb->cpu != this_cpu())
    {
      nxsched_smp_call_init(&g_call_data, nxsched_roundrobin_handler,
                            (FAR void *)(uintptr_t)tcb->pid);
      nxsched_smp_call_single_async(tcb->cpu, &g_call_data);
    }
  else if (nxsched_switch_running(tcb->cpu, true))
    {
      up_switch_context(this_task(), rtcb);
    }
#else
  /* Non-SMP only: the classic flink check */

  if (tcb->flink &&
      tcb->flink->sched_priority >= tcb->sched_priority)
    {
      if (nxsched_reprioritize_rtr(tcb, tcb->sched_priority))
        {
          up_switch_context(this_task(), rtcb);
        }
    }
#endif
```

The comment states the defect explicitly. `tcb->flink` cannot be used on SMP,
because the running task is in `g_assignedtasks[cpu]` rather than in the
ready-to-run list. NuttX's response is to remove the `flink` test from the SMP
path entirely rather than to supplement it.

### How this lands against the open review

| Review finding | NuttX's answer |
| --- | --- |
| The defect: `rtcb->flink` is always the IDLE task on SMP, so round-robin never yields | `flink` is not consulted on SMP at all; `nxsched_switch_running()` selects from the ready-to-run list with affinity applied |
| **B1** — the lock predicate is asked about the wrong CPU during cross-CPU rotation | No cross-CPU rotation is attempted. `tcb->cpu != this_cpu()` sends an inter-processor call and the owning CPU rotates itself, so there is no predicate to get wrong |
| **B2** — the timeslice is refilled before the lock is tested | The guard is `if (tcb->timeslice <= 0 && !nxsched_islocked_tcb(tcb))`, evaluated **before** the reset. This is precisely the correction proposed in the review |

TizenRT's `sched_processtimer.c` and `sched_timerexpiration.c` correspond to a
pre-refactor NuttX vintage. PR 7465 is re-deriving, inline and in part, a fix
that exists upstream in factored form.

A second, subtler difference in the same file: NuttX returns `CLOCK_MAX` when a
task has no timeslicing and `0` under `noswitches`, where TizenRT returns `0`
and `1` respectively. Any port has to reconcile those conventions with
TizenRT's `sched_timer_reassess()` callers.

---

## 2. The deepest divergence: the global scheduler lock

`sched/sched/sched.h`, unconditionally and not behind `CONFIG_SMP`:

```c
#define nxsched_islocked_tcb(tcb)   ((tcb)->lockcount > 0)
```

`sched_lock.c` in NuttX touches only `rtcb->lockcount`. There is no
`g_cpu_lockset`, no `g_cpu_schedlock`, and no `spin_setbit` anywhere in it.

TizenRT `os/kernel/sched/sched.h:404`:

```c
#  define sched_islocked_global() spin_islocked(&g_cpu_schedlock)
#  define sched_islocked_tcb(tcb) sched_islocked_global()
```

On SMP, TizenRT answers *"is this task's pre-emption disabled"* with *"is any
CPU in the system holding the global scheduler lock"*.

| | TizenRT | NuttX |
| --- | --- | --- |
| Scope of `sched_lock()` | Global; suppresses scheduling decisions system-wide | Per task |
| Coupling between CPUs | One CPU's lock forces tasks onto the pending list on every CPU | None |
| Consequence | The pending-list priority inversion: a high priority task is displaced by the IDLE task until the lock clears | The mechanism does not exist |

This is the largest architectural gap between the two, and it sits upstream of
the pull request under review: a substantial part of what PR 7465's lock guard
defends against is an artefact of a design NuttX has since abandoned.

It is also the one item in this document that is a **design decision rather
than a port**. Removing the global lock changes the meaning of `sched_lock()`
for every existing caller that relies, knowingly or not, on system-wide
pre-emption suppression.

---

## 3. Cross-CPU strategy: pause versus call

The two projects reach a task running on another CPU in fundamentally
different ways.

**TizenRT** manipulates the remote CPU's lists from the CPU that noticed the
need. `sched_removereadytorun.c` brackets the surgery with `up_cpu_pause(cpu)`
and `up_cpu_resume(cpu)`: the acting CPU halts the remote one, edits its
assigned list, and releases it.

**NuttX** delegates. `nxsched_smp_call_single_async(cpu, &call_data)` posts a
function to be executed *by the target CPU itself*, which then performs its own
rescheduling locally.

Consequences worth weighing:

- Under delegation there is no window during which one CPU is stopped while
  another edits its state, so the class of defect that B1 belongs to — asking a
  CPU-relative question about the wrong CPU — largely disappears.
- Delegation is asynchronous, so the rotation happens slightly later than the
  tick that triggered it. That is a latency the pause approach does not have.
- `nxsched_smp_call_*` is general infrastructure, not round-robin specific. It
  is reusable for any cross-CPU operation, which is why it is worth porting on
  its own merits rather than only as a dependency.

---

## 4. Feature inventory: present in NuttX, absent from TizenRT

Every symbol below was verified absent from TizenRT tree-wide with
`grep -rl "config <SYMBOL>" --include=Kconfig*`. The single false positive in a
raw symbol diff was `SCHED_WORKQUEUE`, which TizenRT does have, in
`os/wqueue/Kconfig`; it is excluded from this list.

### 4.1 Scheduling policies

| Feature | Notes |
| --- | --- |
| `SCHED_SPORADIC`, `SCHED_SPORADIC_MAXREPL`, `SPORADIC_INSTRUMENTATION`, `sched_sporadic.c` | The POSIX sporadic server policy. TizenRT implements FIFO and round-robin only |

### 4.2 Locking and priority protocols

| Feature | Notes |
| --- | --- |
| `PRIORITY_PROTECT` | Priority ceiling, `PTHREAD_PRIO_PROTECT`. TizenRT has inheritance only |
| `PTHREAD_MUTEX_DEFAULT_PRIO_INHERIT`, `PTHREAD_MUTEX_DEFAULT_PRIO_NONE` | Selectable default mutex protocol |
| `TICKET_SPINLOCK` | First-in first-out fair spinlocks; bounds acquisition latency under contention |
| `RW_SPINLOCK` | Reader and writer spinlocks |

### 4.3 Observability

The largest cluster, and the one with the most direct bearing on testability.

| Feature | Notes |
| --- | --- |
| `SCHED_INSTRUMENTATION` with `_SWITCH`, `_PREEMPTION`, `_CSECTION`, `_SPINLOCKS`, `_IRQHANDLER`, `_SYSCALL`, `_HEAP`, `_WDOG`, `_DUMP`, `_FILTER`, `_FILTER_DEFAULT_MODE`, `_CPUSET`, `_FUNCTION` | Scheduler note and trace framework |
| `SCHED_CRITMONITOR` with `_MAXTIME_CSECTION`, `_MAXTIME_IRQ`, `_MAXTIME_PREEMPTION`, `_MAXTIME_THREAD`, `_MAXTIME_WDOG`, `_MAXTIME_WQUEUE`, `_MAXTIME_BUSYWAIT`, `_MAXTIME_PANIC`, `sched_critmonitor.c` | Measures and bounds time spent in critical sections, in interrupt handlers, and with pre-emption disabled |
| `SCHED_IRQMONITOR` | Per interrupt timing |
| `SCHED_BACKTRACE`, `sched_backtrace.c` | Per task backtrace capture |
| `SCHED_STACK_RECORD` | Stack high-water recording |
| `SCHED_DUMP_LEAK`, `SCHED_DUMP_ON_EXIT`, `sched_dumponexit.c` | Resource leak reporting at task exit |
| `sched_get_stateinfo.c`, `sched_get_stackinfo.c`, `sched_sysinfo.c` | Structured scheduler introspection |

**Note on partial inheritance.** TizenRT contains 93 `#ifdef
CONFIG_SCHED_INSTRUMENTATION` call sites across `os/`, and ships
`os/include/tinyara/sched_note.h`, but has no Kconfig symbol and no
implementation file. The instrumentation points were inherited; the framework
behind them was not. Porting is therefore cheaper than the feature list
suggests, because the call sites already exist and are already in the right
places.

### 4.4 SMP

| Feature | Notes |
| --- | --- |
| `sched_smp.c`, `nxsched_smp_call_*` | Generic cross-CPU function call framework |
| `nxsched_switch_running(cpu, switch_equal)` in `sched_addreadytorun.c` | Factored selection of the next eligible task for a CPU |
| `sched_process_delivered.c` | Deferred cross-CPU work processing |
| `SMP_DEFAULT_CPUSET` | Default affinity mask applied to new tasks |
| `ASSERT_PAUSE_CPU_TIMEOUT` | Detects a CPU that never answers a pause request |

### 4.5 CPU load and timing

`CPULOAD_ONESHOT`, `CPULOAD_PERIOD`, `CPULOAD_ENTROPY`,
`SCHED_CPULOAD_SYSCLK`, `SCHED_CPULOAD_CRITMONITOR`, `SCHED_CPULOAD_NONE`,
`SCHED_PROFILE_TICKSPERSEC`, `HRTIMER` with `_LIST` and `_TREE`,
`CLOCK_ADJTIME` with `_PERIOD_MS` and `_SLEWLIMIT_PPM`, `CLOCK_TIMEKEEPING`,
`PERF_OVERFLOW_CORRECTION`, `SYSTEMTICK_HOOK`, `TIMER_ADJUST_USEC`

### 4.6 Other

`SCHED_EVENTS`, `SCHED_THREAD_LOCAL`, `SCHED_USER_IDENTITY`,
`DEFAULT_TASK_STACKSIZE`, `PTHREAD_GUARDSIZE_DEFAULT`, `PID_INITIAL_COUNT`,
`GROUP_KILL_CHILDREN_TIMEOUT_MS`, `COREDUMP`, `IRQCHAIN` with
`PREALLOC_IRQCHAIN`, `sched_idletask.c`, `sched_suspend.c`, `sched_profil.c`

---

## 5. Source file comparison

NuttX `sched/sched/` builds 51 sources; TizenRT `os/kernel/sched/` builds 47.
Files present in NuttX and absent from TizenRT:

| File | Purpose |
| --- | --- |
| `sched_roundrobin.c` | Round-robin timeslice handling, factored out of the timer path |
| `sched_smp.c` | Cross-CPU function call framework |
| `sched_process_delivered.c` | Deferred cross-CPU work |
| `sched_critmonitor.c` | Critical section and latency monitoring |
| `sched_sporadic.c` | Sporadic server policy |
| `sched_backtrace.c` | Per task backtrace |
| `sched_dumponexit.c` | Exit-time resource dump |
| `sched_cpuload_oneshot.c`, `sched_cpuload_period.c` | Alternative CPU load sources |
| `sched_get_stackinfo.c`, `sched_get_stateinfo.c`, `sched_sysinfo.c` | Introspection |
| `sched_get_tls.c` | Thread local storage access |
| `sched_idletask.c` | Idle task identification helper |
| `sched_suspend.c` | Task suspend and resume |
| `sched_switchcontext.c` | Context switch helper |
| `sched_profil.c` | Profiling support |
| `sched_reprioritizertr.c` | Reprioritise in the scheduler core rather than in architecture code |

The last entry is a structural difference worth noting on its own. TizenRT
implements `up_reprioritize_rtr()` per architecture, for example
`os/arch/arm/src/armv7-a/arm_reprioritizertr.c`, duplicating the same context
switch logic across every architecture port. NuttX moved the common part into
`sched/sched/sched_reprioritizertr.c`, leaving only `up_switch_context()` to
the architecture.

Files present in TizenRT and absent from NuttX — mostly TizenRT additions or
renamed equivalents — include `sched_cpuoff.c`, `sched_cpuon.c`,
`sched_migrate_tasks.c`, `sched_getsockets.c`, `sched_getstreams.c`,
`sched_garbage.c`, `sched_free.c`, `sched_thistask.c`, `sched_processtimer.c`
and `sched_timerexpiration.c`. The last two correspond to NuttX's
`sched_processtick.c` and `sched_processtickless.c`, which delegate to
`sched_roundrobin.c` rather than open-coding the policy.

---

## 6. Mapping to the scenario test suite

Ranked by value to the scenario suite described in the catalogue.

| Rank | Port from NuttX | Scenarios served | Why |
| --- | --- | --- | --- |
| 1 | `sched_roundrobin.c` and `nxsched_switch_running()` | RR-01 to RR-06, SMP-01 | Supersedes PR 7465 with an upstream implementation. Resolves the live defect, B1 and B2 together |
| 2 | `SCHED_INSTRUMENTATION_SWITCH` | RR-06, SMP-04, BLK-01 | Turns inference into measurement; see section 7 |
| 3 | `sched_smp.c` | RR-03, SMP-02, SMP-03, BLK-02, LIFE-02 | Removes the cross-CPU pause pattern and the defect class B1 belongs to |
| 4 | `SCHED_CRITMONITOR` | LOCK-01 to LOCK-03, INV-01 to INV-03, BLK-02, BLK-03 | Measures pre-emption disabled and critical section duration directly, instead of proving a negative by busy waiting |
| 5 | Per task `sched_lock`, dropping `g_cpu_schedlock` | INV-04, RR-05 | Eliminates the pending-list inversion rather than testing for it |
| 6 | `SCHED_CPULOAD_*` variants | SMP-04 | The reference configuration has `SCHED_CPULOAD` disabled, which is why SMP-04 samples occupancy by hand |
| 7 | `PRIORITY_PROTECT` | INV-01 to INV-03, plus new ceiling variants | Would justify three or four additional inversion scenarios |
| 8 | `SCHED_SPORADIC` | A new family | Completes POSIX policy coverage, and unlocks the sporadic conformance tests that NuttX's own LTP integration currently excludes |
| 9 | `SCHED_DUMP_LEAK`, `SCHED_DUMP_ON_EXIT` | LIFE-01, LIFE-03 | Reports leaks directly, where the scenarios currently infer them from task control block counts and `mallinfo()` |
| 10 | `TICKET_SPINLOCK` | AFF-03, SMP-04 | Bounds contended lock acquisition, which makes fairness claims defensible rather than merely observed |

---

## 7. What the scenarios reveal about observability

Several scenarios in the suite are elaborate not because the property is hard
to state, but because TizenRT offers no way to observe it. This is worth
recording, because it is an argument for porting instrumentation that is
independent of any individual defect.

| Scenario | Present implementation | With NuttX instrumentation |
| --- | --- | --- |
| RR-06, rotation rate | Workers watch a shared last-runner variable and infer transfers of control | Read the context switch note count |
| SMP-04, CPU utilisation | Walks every task control block every ten milliseconds through an ioctl to sample occupancy | Read per CPU load |
| LOCK-01 to LOCK-03 | Busy waits two hundred milliseconds to demonstrate that nothing ran | Read the pre-emption disabled duration |
| LIFE-01, task churn | One thousand create and exit cycles, comparing control block counts and heap usage | Read the leak dump |
| INV-01 to INV-03 | Measures the wait for a mutex and infers that a boost occurred | Read the priority change notes |

Porting `SCHED_INSTRUMENTATION_SWITCH` alone would allow a measurable amount of
test machinery to be deleted and replaced with direct measurement. That is a
better outcome than a larger suite: inference is what makes a test fragile.

---

## 8. Recommendation

**Immediate, and it changes the review in progress.** Raise on PR 7465 that
NuttX solved this in `sched_roundrobin.c`, with the `flink` test removed from
the SMP path, the lock test correctly placed ahead of the timeslice reset, and
cross-CPU rotation delegated by inter-processor call. Porting that file is a
smaller and better tested change than the inline patch under review, and it
makes B1 and B2 moot rather than merely fixed.

**Short term.** `SCHED_INSTRUMENTATION_SWITCH`. The 93 orphaned call sites mean
most of the placement work is already done.

**Medium term.** `sched_smp.c`, then `SCHED_CRITMONITOR`.

**Strategic, and a decision rather than a patch.** Dropping the global
scheduler lock in favour of per task lockcount. It removes an entire class of
defect, but it changes the meaning of `sched_lock()` for every caller that
relies on system-wide pre-emption suppression, so it needs agreement before it
needs code.

---

## 9. Method and caveats

**What was read in full:** NuttX `sched/Kconfig` (186 configuration symbols),
`sched/sched/Make.defs` (51 sources), `sched/sched/sched.h`,
`sched/sched/sched_roundrobin.c`, `sched/sched/sched_lock.c`. TizenRT
`os/kernel/sched/` in its entirety across the preceding review work.

**What was inferred rather than read:** the bodies of
`nxsched_switch_running()`, `nxsched_smp_call_single_async()` and
`up_switch_context()`. Statements about their behaviour come from their
declarations, their call sites and the surrounding comments.

**Version skew.** The comparison is against NuttX master, which has diverged
from whatever revision TizenRT forked. Some differences are TizenRT being
behind; others are deliberate divergence by either project. The code alone does
not distinguish the two, and this document does not attempt to.

**Effort estimates are judgement, not measurement.** No port was attempted.
`sched_roundrobin.c` alone depends on `nxsched_switch_running`,
`nxsched_smp_call_*`, `up_switch_context` and the `CLOCK_MAX` return
convention, none of which TizenRT has. "Port one file" is in practice "port one
file and its dependencies", and the return value convention has to be
reconciled with TizenRT's existing timer callers.

**Nothing was built or run.** Every claim is from source inspection.

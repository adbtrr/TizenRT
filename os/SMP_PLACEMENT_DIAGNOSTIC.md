# SMP Placement Diagnostic — Phase 8

Companion document for `os/0008-apps-testcase-smp-placement-diagnostic.patch`.

Target board: **rtl8730e / loadable_ext_ddr** (`CONFIG_SMP=y`, `CONFIG_SMP_NCPUS=2`,
`CONFIG_RR_INTERVAL=10`, `CONFIG_USEC_PER_TICK=1000`, `CONFIG_AMP` not set).

---

## 1. Why this patch exists

### 1.1 Where the scenario suite stands

The scheduling scenario suite (phases 1–3, 13 scenarios) was run on hardware in two
configurations.

| Configuration | Pass | Fail |
|---|---|---|
| Phases 1–3, no PR 7465 | 4 | 9 |
| Phases 1–3 + PR 7465 (head `7346e015`) | 10 | 3 |

PR 7465 in its updated form fixes both blocking findings raised in review:

* **B2** — the pre-emption/lock test now sits *before* `rtcb->timeslice` is reset, so
  the timeslice counter is no longer re-armed and then discarded on a blocked tick.
* **B1** — `irq_cpu_locked(cpu)` became `irq_cpu_locked(this_cpu())`. The tick CPU
  always holds its own bit in `g_cpu_irqset` (set by the IRQ path of
  `enter_critical_section()`), so the guard is now false for the whole
  `sched_process_scheduler()` loop and `sched_process_timeslice(1)` actually executes
  its body instead of returning at the top.

The six scenarios that flipped to pass — RR-01, RR-02, RR-03, PRI-04, AFF-01, AFF-03 —
are precisely the round-robin scenarios, including two that pin their tasks to the
**non-tick CPU**. That is direct hardware confirmation that B1 was real and is fixed.

### 1.2 The three that remain

| Scenario | Assertion | Source | Reported value |
|---|---|---|---|
| SCN-SMP-01 | `smp01_no_starvation` | `stc_sched_smp.c:110` | `stc_monitor_violations()` non-zero |
| SCN-PRI-02 | `pri02_top_ran` | `stc_sched_priority.c:281` | `stc_count(i) == 0x0` |
| SCN-AFF-04 | `aff04_all_cpus_used` | `stc_sched_affinity.c:438` | `union_mask == 0x1`, expected `0x3` |

**AFF-04 names the defect.** It failed at line 438, not at line 432, so the preceding
`aff04_all_ran` loop passed — each of the two workers logged more than
`STC_AFF_MIN_SAMPLES` (1000) samples. Both workers ran, and ran a lot. They simply
never ran anywhere but CPU0.

That is a physical-core observation, not a bookkeeping one. `sched_getcpu()` is

```c
int sched_getcpu(void)
{
	return up_cpu_index();  /* Does not fail */
}
```

(`kernel/sched/sched_getcpu.c:62`), so `union_mask` is built from the core each worker
was actually executing on.

**PRI-02 is the same fact with a sharper edge.** Four unpinned tasks at priorities
130 / 120 / 110 / 100, two cores available, and one of the top two recorded exactly
zero work over a full second. Only one core takes unpinned work, so the 130 task owns
it and the 120 task waits in `g_readytorun` for the entire window while a core is
available. In effect a runnable priority-120 task receives no CPU at all.

**SMP-01 follows from the same cause.** Its monitor sampled successfully (line 108
passed) and then flagged at least one starvation violation, which is the expected
outcome once four tasks are crowded onto a single core.

All three are one defect: **unpinned tasks are not being distributed to the second
core.** Round-robin is not involved, and PR 7465 does not touch this — that patch
governs rotation *within* one core's queue, and it is doing that job correctly.

---

## 2. What was ruled out before writing the patch

Each of these was checked against the source rather than assumed.

**Affinity inheritance.** `CONFIG_AMP` is not set in the board defconfig, so the IDLE
tasks are given `SCHED_ALL_CPUS` (`kernel/init/os_start.c:514`) and every task inherits
that mask through `task_inherit_affinity()` (`kernel/task/task_setup.c:223`). The
workers are genuinely eligible for both cores. The comment at `os_start.c:508` states
the intent explicitly: *"all tasks inherit the affinity mask from their parent and,
ultimately, the parent of all tasks is the IDLE task."*

**A wrong CPU count.** `sched_getcpucount()` returns `CONFIG_SMP_NCPUS`, so
`stc_ncpus()` is 2 and AFF-04's `all_cpus` is correctly `0x3`.

**A dead second core.** AFF-01, AFF-02 and AFF-03 all pass with tasks pinned to CPU1,
each logging more than 1000 samples and reporting `seen_mask == 0x2`. CPU1 executes
tasks correctly whenever affinity names it.

So the second core works, the workers are allowed on it, and they still never go there.

---

## 3. The three candidate causes

Placement of an unpinned task is decided by `sched_select_cpu()`
(`kernel/sched/sched_cpuselect.c:64`), called from `sched_addreadytorun()`
(`sched_addreadytorun.c:191`):

```c
	CPU_AND(&eligible_cpus, &affinity, &g_active_cpus_mask);      /* :72 */

	if (eligible_cpus == 0) {                                     /* :75 */
		eligible_cpus = g_active_cpus_mask;
	}
	...
			if (rtcb->flink == NULL) {                            /* :91 */
				return i;        /* CPU running only IDLE */
			} else if (rtcb->sched_priority < minprio) {          /* :97 */
				minprio = rtcb->sched_priority;
				cpu = i;         /* Best non-IDLE CPU so far */
			}
```

and its result is then filtered by the state decision in `sched_addreadytorun()`:

```c
	if (rtcb->sched_priority < btcb->sched_priority) {            /* :204 */
		task_state = TSTATE_TASK_RUNNING;
	} else if ((btcb->flags & TCB_FLAG_CPU_LOCKED) != 0) {
		task_state = TSTATE_TASK_ASSIGNED;
	} else {
		task_state = TSTATE_TASK_READYTORUN;
		cpu = 0;  /* CPU does not matter */                       /* :216 */
	}
```

From that code there are exactly three ways CPU1 can be declined, and **each calls for
a different fix**:

| # | Cause | Consequence |
|---|---|---|
| 1 | CPU1 is absent from `g_active_cpus_mask` | It is filtered out at line 72, before load is considered at all |
| 2 | CPU1 is not idle — a task is resident on it that the newcomer cannot displace | Line 91 does not fire; line 204's strict `<` prevents an equal-priority task from preempting; line 97 breaks ties toward the lowest-numbered core |
| 3 | CPU1 is idle and still not chosen | The defect is in the placement path itself |

### 3.1 What can already be narrowed

**Cause 3 is impossible by inspection.** If CPU1 were idle, `g_assignedtasks[1].head`
is the IDLE task with `flink == NULL`, so line 91 returns `1` immediately. Then
`rtcb->sched_priority (0) < btcb->sched_priority (100)` at line 204 is true and the task
goes `TSTATE_TASK_RUNNING` on CPU1. There is no route by which an idle CPU1 is skipped.

**Cause 1 is nearly excluded.** If CPU1 were inactive, the `eligible_cpus == 0`
fallback at line 75 would send even CPU1-*pinned* tasks to CPU0 — and AFF-01 passes
with `seen_mask == 0x2`.

**Cause 2 is therefore the leading candidate**, and PRI-02 constrains the priority:
its worker at 120 was also excluded, so a resident task on CPU1 would have to be at
**priority ≥ 120** and permanently runnable.

### 3.2 Why this matters for the conclusion

If cause 2 is what the board shows, **this is not a TizenRT scheduler defect**. It
would mean some platform thread legitimately occupies core 1, the board has roughly one
core available for application work, and the three scenarios are asserting something
the platform cannot provide. The correct response would then be to fix the tests, not
the kernel — the opposite of a scheduler patch.

That is the reason this phase ships a diagnostic and not a fix. The three causes lead
to three mutually exclusive changes, one of which is not a kernel change at all, and a
speculative patch on the SMP path risks faulting the board while masking the real
answer.

### 3.3 A related gap worth recording

Only two paths can ever place work on an idle core:

1. `sched_addreadytorun()` → `sched_select_cpu()` — the *push* path examined above.
2. `sched_removereadytorun()` on that core, which scans `g_readytorun`
   (`sched_removereadytorun.c:221`) — the *pull* path.

On this board the pull path is inert. CPU1 runs IDLE, IDLE never blocks, and the timer
is enabled on CPU0 only — `up_timer_initialize()` gates `arm_arch_timer_enable(1)`
behind `if (up_cpu_index() == 0)` (`arch/arm/src/amebasmart/amebasmart_timerisr.c:98`),
and that helper writes `CNTV_CTL` via `mcr p15, 0, %0, c14, c3, 1`
(`arch/arm/src/amebasmart/arch_timer.h:62`), which is banked per core. CPU1 therefore
has essentially no scheduling events of its own.

**Consequence: core 1 can only ever be given work by being pushed from core 0. There is
no pull.** Everything rests on the push path, which is what makes cause 2 so decisive.

The same "use whatever is next in the assigned list" fallback appears in
`sched_removereadytorun.c:221` and `sched_setpriority.c:119`, both carrying the authors'
own note — *"REVISIT: What if it is not the IDLE thread?"*. Those are pre-existing and
independent of PR 7465.

---

## 4. What the patch adds

### 4.1 Kernel side — `TESTIOC_SCHED_CPUSTATE`

A new read-only ioctl on the existing OS API test driver. It fills
`struct test_cpustate_s` from inside a single `enter_critical_section()` so that every
field describes the same instant; sampling the lists separately would let them move
underneath the reader and produce a picture that never actually existed.

Captured fields:

| Field | Meaning |
|---|---|
| `ncpus` | `CONFIG_SMP_NCPUS` |
| `caller_cpu` | Core the sampling task is running on |
| `active_mask` | `g_active_cpus_mask` — settles cause 1 |
| `sched_locked` | `sched_islocked_global()` |
| `select_all` | What `sched_select_cpu()` returns right now for an unpinned task |
| `cpu[i].pid` / `.name` / `.priority` / `.state` | Head of `g_assignedtasks[i]`, i.e. what core `i` is executing |
| `cpu[i].nassigned` | Depth of `g_assignedtasks[i]` |
| `cpu[i].idle_only` | `head->flink == NULL` — the exact test at `sched_cpuselect.c:91`, recorded so the decision can be checked against its input |
| `rtr[n].pid` / `.name` / `.priority` / `.affinity` | Unassigned runnable tasks in `g_readytorun`, in scheduler order |
| `nrtr` / `nrtr_reported` | True depth of `g_readytorun`, and how many entries fit in the array |

Files touched:

* `os/include/tinyara/os_api_test_drv.h` — command and structures
* `os/drivers/os_api_test/kernel/test_sched.c` — implementation
* `os/drivers/os_api_test/os_api_test_drv.c` — command routing

The implementation reads scheduler state and nothing else. No list is modified and no
task is moved.

### 4.2 Application side — `SCN-DIAG-01` … `SCN-DIAG-04`

New file `apps/examples/testcase/le_tc/kernel/stc_sched_diag.c`. Each scenario takes
five snapshots at 120 ms intervals, so that "always on one core" can be distinguished
from "moves, but rarely".

| Scenario | Shape | Question answered |
|---|---|---|
| DIAG-01 | No load | Reference picture. Are both cores active, and is anything resident on core 1 before any test load exists? |
| DIAG-02 | Two unpinned equal-priority spinners — the AFF-04 shape | **The decisive one.** Where is each worker assigned, is the other core idle-only, and is a worker sitting in `g_readytorun`? |
| DIAG-03 | `ncpus + 2` unpinned tasks on a descending priority ladder — the PRI-02 shape | Is the priority-120 task in `g_readytorun` while a core reports `idle_only=1`? |
| DIAG-04 | Same pair, then worker 1 explicitly pinned to the highest-numbered core mid-load | Control. Separates "the core cannot run tasks" from "the core is never chosen" |

Every scenario reports success. A diagnostic run must not mask a real regression
elsewhere in the suite; the value is in the log, not in the verdict.

Runtime is roughly 15 seconds. The block appears at the end of the log, after the three
existing failures, since the run order is RR → SMP → PRI → AFF → DIAG.

---

## 5. Applying and building

The patch is generated against master plus phases 1–3, which is the configuration
currently under test:

```
git apply os/0001-apps-testcase-scheduling-scenario-harness-and-rr-scenarios.patch
git apply os/0002-apps-testcase-starvation-invariant-monitor-and-smp-family.patch
git apply os/0003-apps-testcase-priority-and-affinity-scenario-families.patch
git apply os/0008-apps-testcase-smp-placement-diagnostic.patch
```

All four apply clean with no fuzz.

If phases 0004–0007 are also applied, the three wiring files
(`Make.defs`, `tc_internal.h`, `kernel_tc_main.c`) have shifted and plain `git apply`
will reject. Use `patch -p1 --fuzz=5` instead, which has been verified against the full
series.

**No Kconfig change is required.** `stc_sched_diag_main()` sits under the same
`CONFIG_STC_KERNEL_SCHED` guard as the other scenario families, so if the scenario
tests already build, this builds with them.

Both new source files were compile-checked against the real TizenRT headers with a
configuration generated from the board defconfig, under `-Wall -Wextra`, with no
warnings. The whole body of `stc_sched_diag.c` is inside `#ifdef CONFIG_SMP` and the
entry point reports a skip on a single-CPU build.

---

## 6. Reading the output

The format is fixed and greppable. One snapshot looks like this:

```
[SCN-DIAG pair #0] caller_cpu=0 active_mask=0x3 sched_locked=0 select_cpu(all)=0 rtr_depth=1
[SCN-DIAG pair #0]   cpu0: pid=12 prio=100 state=3 nassigned=2 idle_only=0 name=stcwrk
[SCN-DIAG pair #0]   cpu1: pid=?? prio=??  state=?? nassigned=? idle_only=? name=????
[SCN-DIAG pair #0]   rtr[0]: pid=13 prio=100 affinity=0x3 name=stcwrk
```

The decisive line is `cpu1:` in the `pair` snapshots.

| Observation | Cause | Follow-up |
|---|---|---|
| `active_mask=0x1` | 1 | Fix the CPU activation / hotplug path that maintains `g_active_cpus_mask` |
| `cpu1: idle_only=0` with a named task at priority ≥ 120 | 2 | Not a scheduler defect. Correct the three scenarios, and report core 1 occupancy as a platform finding |
| `cpu1: idle_only=1` **and** `select_cpu(all)=0` | 3 | Kernel fix in `sched_select_cpu()` / `sched_addreadytorun()` |

Two supporting reads:

* **The `rtr[]` lines.** An entry with `affinity=0x3` present while `cpu1` reports
  `idle_only=1` is a runnable task sitting next to an idle core. That is a hard defect
  regardless of which cause is in play.
* **DIAG-04 is the control.** If `forced-after` shows worker 1 on core 1 while
  `forced-before` and every `pair` snapshot do not, then the core works and only the
  *selection* is at fault.

One caveat when reading any snapshot: the sampling task has to be running somewhere,
and `caller_cpu` names that core. That core is occupied by the diagnostic rather than by
the workload at the instant of the sample, and the log should be read with that
subtracted. This is why `select_all` and the full assigned lists are captured rather
than just a summary.

---

## 7. Status and next step

* PR 7465 is not blocked by any of this. B1 and B2 are fixed and confirmed on
  hardware — 4/13 → 10/13, with the six flips being exactly the round-robin scenarios,
  two of them pinned to the non-tick core. It should merge.
* The three residual failures are pre-existing behaviour that the patch neither causes
  nor claims to address, and they belong in a separate report.
* This diagnostic determines which of the three causes applies. The fix — a kernel
  patch or corrected test expectations — follows from its output, and is deliberately
  not guessed at in advance.

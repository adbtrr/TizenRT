# Semaphore scenario test tier - phase 4 to 6 patches

These are `git format-patch` files for the remaining phases of the
Priority Inheritance and Semaphore Recovery Scenario Test Catalogue.
Phases 1 to 3 are already committed on this branch; these three are
supplied as patches rather than as applied changes.

| Patch | Phase | Catalogue content |
| ----- | ----- | ----------------- |
| `0004-stc-sem-scenario-phase4-recovery.patch` | 4 | Families E and F (recovery), plus `TESTIOC_SEM_SNAPSHOT` |
| `0005-stc-sem-scenario-phase5-protocol-destroy.patch` | 5 | Families D and G, testable part of H, `TESTIOC_SEM_RESET_TEST`, SCN-REC-04 |
| `0006-stc-sem-scenario-phase6-smp-soak.patch` | 6 | Families I and J (SMP and soak) |

## Applying

They are sequential and must be applied in order, on top of the phase 3
commit:

```
git am 0004-*.patch 0005-*.patch 0006-*.patch
```

or, to apply without committing:

```
git apply 0004-*.patch && git apply 0005-*.patch && git apply 0006-*.patch
```

Each was verified to apply cleanly, in this order, against the phase 3
tree.

## What these patches touch outside apps/

Phases 4 and 5 add read-only test support to the OS API test driver,
because three things the scenarios need are unreachable from user space:

| Added | Why |
| ----- | --- |
| `TESTIOC_SEM_SNAPSHOT` | The holder list, per-holder counts and base priorities are kernel structures. Under `CONFIG_APP_BINARY_SEPARATION` a user-space test cannot read them, and `CONFIG_SEM_PHDEBUG` is not enabled on the reference platform. |
| `TESTIOC_SEM_RESET_TEST` | `sem_reset()` is declared in `tinyara/semaphore.h` and is not exported through the syscall table, so SCN-DES-03 and SCN-DES-04 have to run kernel-side. |

Both are read-only with respect to kernel state, allocate nothing, and
are gated behind the existing `CONFIG_DRIVERS_OS_API_TEST`.

## Coverage that these patches do not provide

Stated here so the gaps are visible without reading the catalogue:

| Scenario | Why not covered | What it would take |
| -------- | --------------- | ------------------ |
| SCN-PIC-03 | `sem_tickwait()` is not declared in the application-facing `semaphore.h` and is not a syscall | a driver-side handler, like the existing tick-wait test |
| SCN-BIN-01 to SCN-BIN-03 | need a loadable user binary that can be faulted on demand | binary-manager fault fixture; the manual procedure is in the header of `stc_sem_destroy.c` |
| SCN-SMP-03 | needs a semaphore posted from a real interrupt handler to reach `sem_restorebaseprio_irq()` | a driver-side post from a timer or watchdog callback |
| SCN-REC-04 | needs `CONFIG_PTHREAD_MUTEX_ROBUST`; the reference platform sets `CONFIG_PTHREAD_MUTEX_UNSAFE=y`, which leaves the robust mutex implementation out of the build | a dedicated build configuration; the scenario is written and reports a skip until then |

## Verification status

Not build-verified: no target toolchain was available when these were
written. The application sources were syntax-checked with
`gcc -Wall -Wextra` against stub headers in four configurations
(inheritance on, on with SMP, on with `PTHREAD_MUTEX_UNSAFE`, and off for
the skip paths) and are clean in all four. The kernel-side driver changes
are not covered by that check. No scenario has been executed on hardware.

/****************************************************************************
 *
 * Copyright 2026 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

/// @file stc_sem_inherit.c
/// @brief Family A of the Priority Inheritance and Semaphore Recovery
///        Scenario Test Catalogue - boost and restore.
///
/// SCN-PI-01  boost on block, restore on post
/// SCN-PI-02  no boost when the waiter does not outrank the holder
/// SCN-PI-03  the holder tracks the maximum waiter priority
/// SCN-PI-04  a classic inversion is bounded by the critical section
/// SCN-PI-05  every holder of a counting semaphore is boosted
/// SCN-PI-06  every holder is restored, not only the one that posted
///
/// These are the base cases.  Nothing in the tree asserts today that a boost
/// happens at all: the only priority inheritance test that exists,
/// tc_semaphore_sem_setprotocol(), checks a return code and never reads a
/// priority, so a kernel with sem_boostpriority() stubbed out passes it
/// unchanged.  Every oracle below is an exact integer equality on a priority
/// or on a count; none of them carries a timing tolerance.
///
/// Note on style: every value asserted is first stored in a local.  The
/// TC_ASSERT macros evaluate their argument a second time when building the
/// failure message, and several of the expressions here either spawn a task or
/// wait up to STC_TIMEOUT_MS.

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/types.h>

#include "tc_common.h"
#include "tc_internal.h"
#include "stc_sem_common.h"

#ifdef CONFIG_PRIORITY_INHERITANCE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Actor stages.  Monotonic per actor, so the harness never has to guess which
 * of two runnable actors reported first.
 */

#define STAGE_STARTED           1
#define STAGE_HELD              2	/* holder acquired a count             */
#define STAGE_POSTED            3	/* holder released it                  */

#define STAGE_ACQUIRED          2	/* waiter obtained the count           */

/* Slot assignment, uniform across the scenarios in this file. */

#define SLOT_HOLDER             0
#define SLOT_HOLDER2            1
#define SLOT_HOLDER3            2
#define SLOT_WAITER             3
#define SLOT_WAITER2            4
#define SLOT_SPINNER            5

/* Length of the SCN-PI-04 critical section, in loop iterations.  Work, not a
 * sleep: a sleeping holder would release the CPU, and the scenario would then
 * pass even with inheritance removed.
 */

#define CRITICAL_WORK           200000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The semaphore under test.  Re-initialised by every scenario. */

static sem_t g_target;

/* Sink for the critical-section work loop, so it cannot be optimised away. */

static volatile uint32_t g_work_sink;

/****************************************************************************
 * Private Functions - actors
 ****************************************************************************/

/****************************************************************************
 * Name: holder_actor
 *
 * Description:
 *   Take one count, announce it, wait for the harness, release it.  While
 *   waiting for the harness this actor is blocked on its own signalling
 *   semaphore, which records no holder and so cannot itself boost anybody:
 *   the only boost it can receive is the one under test.
 *
 ****************************************************************************/

static int holder_actor(int argc, char *argv[])
{
	int slot = atoi(argv[1]);

	(void)argc;

	if (sem_wait(&g_target) != OK) {
		stc_actor_done(slot);
		return ERROR;
	}

	stc_trace('L');
	stc_stage(slot, STAGE_HELD);

	stc_wait_go(slot);

	(void)sem_post(&g_target);
	stc_trace('l');
	stc_stage(slot, STAGE_POSTED);

	stc_actor_done(slot);
	return OK;
}

/****************************************************************************
 * Name: holder_work_actor
 *
 * Description:
 *   As holder_actor(), but with a bounded work loop inside the critical
 *   section.  Used by SCN-PI-04, where the point is that the holder needs CPU
 *   time it can only obtain by inheriting the waiter's priority.
 *
 ****************************************************************************/

static int holder_work_actor(int argc, char *argv[])
{
	int slot = atoi(argv[1]);
	uint32_t i;

	(void)argc;

	if (sem_wait(&g_target) != OK) {
		stc_actor_done(slot);
		return ERROR;
	}

	stc_trace('L');
	stc_stage(slot, STAGE_HELD);

	stc_wait_go(slot);

	for (i = 0; i < CRITICAL_WORK; i++) {
		g_work_sink++;
	}

	(void)sem_post(&g_target);
	stc_trace('l');
	stc_stage(slot, STAGE_POSTED);

	stc_actor_done(slot);
	return OK;
}

/****************************************************************************
 * Name: waiter_actor
 *
 * Description:
 *   Block on the semaphore, announce the acquisition, wait for the harness,
 *   release it.  The actor cannot announce "I am now blocked", so the harness
 *   observes that transition through the semaphore count instead.
 *
 ****************************************************************************/

static int waiter_actor(int argc, char *argv[])
{
	int slot = atoi(argv[1]);

	(void)argc;

	stc_stage(slot, STAGE_STARTED);
	stc_trace('h');

	if (sem_wait(&g_target) != OK) {
		stc_actor_done(slot);
		return ERROR;
	}

	stc_trace('H');
	stc_stage(slot, STAGE_ACQUIRED);

	stc_wait_go(slot);

	(void)sem_post(&g_target);

	stc_actor_done(slot);
	return OK;
}

/****************************************************************************
 * Name: spinner_actor
 *
 * Description:
 *   The medium-priority inverter of SCN-PI-04.  It never touches the
 *   semaphore; its only job is to occupy the CPU the holder needs.
 *
 ****************************************************************************/

static int spinner_actor(int argc, char *argv[])
{
	int slot = atoi(argv[1]);

	(void)argc;

	stc_stage(slot, STAGE_STARTED);
	stc_trace('M');

	while (!g_stc_spin_stop) {
		g_stc_spin_count++;
	}

	stc_actor_done(slot);
	return OK;
}

/****************************************************************************
 * Private Functions - helpers
 *
 * These return int rather than using TC_ASSERT, because the TC_ASSERT macros
 * expand to a bare return and may only be used from the void scenario
 * functions themselves.
 ****************************************************************************/

/****************************************************************************
 * Name: scenario_begin
 ****************************************************************************/

static int scenario_begin(int initial_count)
{
	if (stc_reset() != OK) {
		return ERROR;
	}

	if (sem_init(&g_target, 0, initial_count) != OK) {
		return ERROR;
	}

	return OK;
}

/****************************************************************************
 * Name: scenario_end
 *
 * Description:
 *   Rule 7: deterministic teardown.  Returns the number of actors that had to
 *   be force-deleted, so the scenario can assert that it is zero.
 *
 ****************************************************************************/

static int scenario_end(void)
{
	int leaked = stc_teardown();

	(void)sem_destroy(&g_target);

	return leaked;
}

/****************************************************************************
 * Name: hold_and_block
 *
 * Description:
 *   Common opening: one holder at priority 100 takes the semaphore, then one
 *   waiter of the given priority blocks on it.  On return the holder is parked
 *   on its go semaphore and the waiter is in TSTATE_WAIT_SEM, so nothing can
 *   move until the harness releases someone and the sample that follows is
 *   unambiguous.
 *
 ****************************************************************************/

static int hold_and_block(int waiter_prio, int waiter_slot)
{
	if (stc_spawn("stc_hold", STC_PRIO_LOW, holder_actor, SLOT_HOLDER) == (pid_t)ERROR) {
		return ERROR;
	}

	if (stc_wait_stage(SLOT_HOLDER, STAGE_HELD) != OK) {
		return ERROR;
	}

	if (stc_spawn("stc_wait", waiter_prio, waiter_actor, waiter_slot) == (pid_t)ERROR) {
		return ERROR;
	}

	return stc_wait_count(&g_target, -1);
}

/****************************************************************************
 * Name: release_waiter
 *
 * Description:
 *   Wait for the waiter in the given slot to acquire the count, then release
 *   it and wait for the semaphore to reach its final value.
 *
 ****************************************************************************/

static int release_waiter(int slot, int final_count)
{
	if (stc_wait_stage(slot, STAGE_ACQUIRED) != OK) {
		return ERROR;
	}

	stc_go(slot);

	return stc_wait_count(&g_target, final_count);
}

/****************************************************************************
 * Private Functions - scenarios
 ****************************************************************************/

/****************************************************************************
 * Name: stc_sem_pi01_boost_and_restore
 *
 * Scenario: SCN-PI-01
 *   A priority 100 task holds a binary semaphore; a priority 140 task blocks
 *   on it.
 *
 * Oracle:
 *   (1) while the waiter is blocked, the holder reports 140       <- HARD
 *   (2) after the post, the holder reports 100 again              <- HARD
 *   (3) the count returns to its initial value                    <- HARD
 *
 * Defect signature:
 *   Oracle (1) fails if the boost never happens.  Oracle (2) fails if the
 *   restore never happens, which leaves a worker thread permanently elevated.
 *
 ****************************************************************************/

static void stc_sem_pi01_boost_and_restore(void)
{
	int ret;
	int prio;

	ret = scenario_begin(1);
	TC_ASSERT_EQ("pi01_begin", ret, OK);

	ret = hold_and_block(STC_PRIO_HIGH, SLOT_WAITER);
	TC_ASSERT_EQ_CLEANUP("pi01_setup", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi01_boosted", prio, STC_PRIO_HIGH, scenario_end());

	/* Release the holder.  Observing the count return to 0 proves the post
	 * completed: sem_post() increments the count, hands it to the waiter and
	 * runs the priority restore inside a single critical section, so a count
	 * observed from outside that section implies the restore has already run.
	 */

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, 0);
	TC_ASSERT_EQ_CLEANUP("pi01_post", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi01_restored", prio, STC_PRIO_LOW, scenario_end());

	ret = release_waiter(SLOT_WAITER, 1);
	TC_ASSERT_EQ_CLEANUP("pi01_drained", ret, OK, scenario_end());

	ret = stc_wait_finished(SLOT_HOLDER);
	TC_ASSERT_EQ_CLEANUP("pi01_holder_done", ret, OK, scenario_end());

	ret = stc_wait_finished(SLOT_WAITER);
	TC_ASSERT_EQ_CLEANUP("pi01_waiter_done", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi01_no_leaked_actor", ret, 0);

	TC_SUCCESS_RESULT();
}

/****************************************************************************
 * Name: stc_sem_pi02_no_boost_for_equal_or_lower
 *
 * Scenario: SCN-PI-02
 *   The same shape, with the waiter at the holder's own priority, and then
 *   below it.
 *
 * Oracle:
 *   the holder's priority is unchanged in both runs                <- HARD
 *
 * Defect signature:
 *   A non-strict comparison in sem_boostholderprio() would call
 *   sched_setpriority() with the value the holder already has, requeueing it
 *   behind its equals - a yield the holder never asked for.
 *
 ****************************************************************************/

static void stc_sem_pi02_no_boost_for_equal_or_lower(void)
{
	int ret;
	int prio;

	/* Run 1: waiter at exactly the holder's priority. */

	ret = scenario_begin(1);
	TC_ASSERT_EQ("pi02_begin1", ret, OK);

	ret = hold_and_block(STC_PRIO_LOW, SLOT_WAITER);
	TC_ASSERT_EQ_CLEANUP("pi02_setup1", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi02_equal_no_boost", prio, STC_PRIO_LOW, scenario_end());

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, 0);
	TC_ASSERT_EQ_CLEANUP("pi02_post1", ret, OK, scenario_end());

	ret = release_waiter(SLOT_WAITER, 1);
	TC_ASSERT_EQ_CLEANUP("pi02_drained1", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi02_no_leaked_actor1", ret, 0);

	/* Run 2: waiter below the holder. */

	ret = scenario_begin(1);
	TC_ASSERT_EQ("pi02_begin2", ret, OK);

	ret = hold_and_block(STC_PRIO_LOW - 10, SLOT_WAITER);
	TC_ASSERT_EQ_CLEANUP("pi02_setup2", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi02_lower_no_boost", prio, STC_PRIO_LOW, scenario_end());

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, 0);
	TC_ASSERT_EQ_CLEANUP("pi02_post2", ret, OK, scenario_end());

	ret = release_waiter(SLOT_WAITER, 1);
	TC_ASSERT_EQ_CLEANUP("pi02_drained2", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi02_no_leaked_actor2", ret, 0);

	TC_SUCCESS_RESULT();
}

/****************************************************************************
 * Name: stc_sem_pi03_tracks_highest_waiter
 *
 * Scenario: SCN-PI-03
 *   Waiters arrive at 130 and then at 160.
 *
 * Oracle:
 *   the holder reports 130, then 160                               <- HARD
 *
 ****************************************************************************/

static void stc_sem_pi03_tracks_highest_waiter(void)
{
	int ret;
	int prio;
	pid_t pid;

	ret = scenario_begin(1);
	TC_ASSERT_EQ("pi03_begin", ret, OK);

	ret = hold_and_block(STC_PRIO_MID, SLOT_WAITER);
	TC_ASSERT_EQ_CLEANUP("pi03_setup", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi03_first_boost", prio, STC_PRIO_MID, scenario_end());

	pid = stc_spawn("stc_wait2", STC_PRIO_EXTRA, waiter_actor, SLOT_WAITER2);
	TC_ASSERT_NEQ_CLEANUP("pi03_spawn_waiter2", pid, (pid_t)ERROR, scenario_end());

	ret = stc_wait_count(&g_target, -2);
	TC_ASSERT_EQ_CLEANUP("pi03_two_waiters", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi03_second_boost", prio, STC_PRIO_EXTRA, scenario_end());

	/* Drain.  The highest-priority waiter takes the count first, so the count
	 * walks -2 -> -1 -> 0 -> 1 as each participant posts in turn.
	 */

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, -1);
	TC_ASSERT_EQ_CLEANUP("pi03_post", ret, OK, scenario_end());

	ret = release_waiter(SLOT_WAITER2, 0);
	TC_ASSERT_EQ_CLEANUP("pi03_x_drained", ret, OK, scenario_end());

	ret = release_waiter(SLOT_WAITER, 1);
	TC_ASSERT_EQ_CLEANUP("pi03_h_drained", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi03_no_leaked_actor", ret, 0);

	TC_SUCCESS_RESULT();
}

/****************************************************************************
 * Name: stc_sem_pi04_inversion_is_bounded
 *
 * Scenario: SCN-PI-04
 *   A priority 100 holder, a priority 120 CPU burner that never touches the
 *   semaphore, and a priority 140 waiter.  All actors are pinned to one CPU,
 *   so the holder can only make progress by inheriting.
 *
 * Oracle:
 *   (1) the holder reports 140 while the waiter is blocked         <- HARD
 *   (2) the holder completes its critical section and posts        <- HARD
 *   (3) the holder is back at 100 afterwards                       <- HARD
 *
 * Defect signature:
 *   Without inheritance the holder sits at 100, the burner at 120 owns the CPU
 *   indefinitely, and oracle (2) times out.  That timeout is the classic
 *   unbounded priority inversion, reported as a failure rather than as a hang.
 *
 ****************************************************************************/

static void stc_sem_pi04_inversion_is_bounded(void)
{
	int ret;
	int prio;
	pid_t pid;

	ret = scenario_begin(1);
	TC_ASSERT_EQ("pi04_begin", ret, OK);

	pid = stc_spawn("stc_hwork", STC_PRIO_LOW, holder_work_actor, SLOT_HOLDER);
	TC_ASSERT_NEQ_CLEANUP("pi04_spawn_holder", pid, (pid_t)ERROR, scenario_end());

	ret = stc_wait_stage(SLOT_HOLDER, STAGE_HELD);
	TC_ASSERT_EQ_CLEANUP("pi04_held", ret, OK, scenario_end());

	pid = stc_spawn("stc_spin", STC_PRIO_INVERTER, spinner_actor, SLOT_SPINNER);
	TC_ASSERT_NEQ_CLEANUP("pi04_spawn_spinner", pid, (pid_t)ERROR, scenario_end());

	ret = stc_wait_stage(SLOT_SPINNER, STAGE_STARTED);
	TC_ASSERT_EQ_CLEANUP("pi04_spinning", ret, OK, scenario_end());

	pid = stc_spawn("stc_wait", STC_PRIO_HIGH, waiter_actor, SLOT_WAITER);
	TC_ASSERT_NEQ_CLEANUP("pi04_spawn_waiter", pid, (pid_t)ERROR, scenario_end());

	ret = stc_wait_count(&g_target, -1);
	TC_ASSERT_EQ_CLEANUP("pi04_blocked", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi04_boosted", prio, STC_PRIO_HIGH, scenario_end());

	/* The holder now needs CPU time that only the boost can give it: the
	 * burner outranks its base priority and never blocks.
	 */

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, 0);
	TC_ASSERT_EQ_CLEANUP("pi04_progress", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi04_restored", prio, STC_PRIO_LOW, scenario_end());

	g_stc_spin_stop = true;

	ret = release_waiter(SLOT_WAITER, 1);
	TC_ASSERT_EQ_CLEANUP("pi04_drained", ret, OK, scenario_end());

	/* The burner must be confirmed gone before teardown, otherwise it would be
	 * force-deleted and reported as a leaked actor.
	 */

	ret = stc_wait_finished(SLOT_SPINNER);
	TC_ASSERT_EQ_CLEANUP("pi04_spinner_stopped", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi04_no_leaked_actor", ret, 0);

	TC_SUCCESS_RESULT();
}

/****************************************************************************
 * Name: three_holders_and_waiter
 *
 * Description:
 *   Shared opening for SCN-PI-05 and SCN-PI-06: a counting semaphore with
 *   three counts, three holders at 100, 105 and 110, and one waiter at 160.
 *
 ****************************************************************************/

static int three_holders_and_waiter(void)
{
	static const int prio[3] = { STC_PRIO_LOW, STC_PRIO_LOW2, STC_PRIO_LOW3 };
	static const int slot[3] = { SLOT_HOLDER, SLOT_HOLDER2, SLOT_HOLDER3 };
	int i;

	for (i = 0; i < 3; i++) {
		if (stc_spawn("stc_hold", prio[i], holder_actor, slot[i]) == (pid_t)ERROR) {
			return ERROR;
		}

		if (stc_wait_stage(slot[i], STAGE_HELD) != OK) {
			return ERROR;
		}
	}

	if (stc_getcount(&g_target) != 0) {
		return ERROR;
	}

	if (stc_spawn("stc_wait", STC_PRIO_EXTRA, waiter_actor, SLOT_WAITER) == (pid_t)ERROR) {
		return ERROR;
	}

	return stc_wait_count(&g_target, -1);
}

/****************************************************************************
 * Name: stc_sem_pi05_all_holders_boosted
 *
 * Scenario: SCN-PI-05
 *   Three holders of a counting semaphore, one waiter at 160.
 *
 * Oracle:
 *   all three holders report 160                                   <- HARD
 *
 * Defect signature:
 *   A boost pass that stops at the first holder leaves the other two below the
 *   waiter, and the inversion persists for as long as they hold counts.
 *
 ****************************************************************************/

static void stc_sem_pi05_all_holders_boosted(void)
{
	int ret;
	int prio;

	ret = scenario_begin(3);
	TC_ASSERT_EQ("pi05_begin", ret, OK);

	ret = three_holders_and_waiter();
	TC_ASSERT_EQ_CLEANUP("pi05_setup", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi05_holder1_boosted", prio, STC_PRIO_EXTRA, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER2].pid);
	TC_ASSERT_EQ_CLEANUP("pi05_holder2_boosted", prio, STC_PRIO_EXTRA, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER3].pid);
	TC_ASSERT_EQ_CLEANUP("pi05_holder3_boosted", prio, STC_PRIO_EXTRA, scenario_end());

	/* Drain: -1 -> 0 -> 1 -> 2 -> 3 as each participant posts. */

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, 0);
	TC_ASSERT_EQ_CLEANUP("pi05_post1", ret, OK, scenario_end());

	stc_go(SLOT_HOLDER2);
	ret = stc_wait_count(&g_target, 1);
	TC_ASSERT_EQ_CLEANUP("pi05_post2", ret, OK, scenario_end());

	stc_go(SLOT_HOLDER3);
	ret = stc_wait_count(&g_target, 2);
	TC_ASSERT_EQ_CLEANUP("pi05_post3", ret, OK, scenario_end());

	ret = release_waiter(SLOT_WAITER, 3);
	TC_ASSERT_EQ_CLEANUP("pi05_drained", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi05_no_leaked_actor", ret, 0);

	TC_SUCCESS_RESULT();
}

/****************************************************************************
 * Name: stc_sem_pi06_all_holders_restored
 *
 * Scenario: SCN-PI-06
 *   The same set-up, observed after the first holder posts.  The waiter has
 *   been granted the count, so no waiter remains that could justify a boost.
 *
 * Oracle:
 *   every holder is back at its own base priority, not only the one that
 *   posted                                                         <- HARD
 *
 * Defect signature:
 *   A restore pass that only reprioritises the posting task leaves the other
 *   two holders at 160 until they happen to post something of their own.  It
 *   is invisible to every existing test, because no existing test reads a
 *   priority.
 *
 ****************************************************************************/

static void stc_sem_pi06_all_holders_restored(void)
{
	int ret;
	int prio;

	ret = scenario_begin(3);
	TC_ASSERT_EQ("pi06_begin", ret, OK);

	ret = three_holders_and_waiter();
	TC_ASSERT_EQ_CLEANUP("pi06_setup", ret, OK, scenario_end());

	stc_go(SLOT_HOLDER);
	ret = stc_wait_count(&g_target, 0);
	TC_ASSERT_EQ_CLEANUP("pi06_post1", ret, OK, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER].pid);
	TC_ASSERT_EQ_CLEANUP("pi06_poster_restored", prio, STC_PRIO_LOW, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER2].pid);
	TC_ASSERT_EQ_CLEANUP("pi06_peer2_restored", prio, STC_PRIO_LOW2, scenario_end());

	prio = stc_getprio(g_stc_actor[SLOT_HOLDER3].pid);
	TC_ASSERT_EQ_CLEANUP("pi06_peer3_restored", prio, STC_PRIO_LOW3, scenario_end());

	stc_go(SLOT_HOLDER2);
	ret = stc_wait_count(&g_target, 1);
	TC_ASSERT_EQ_CLEANUP("pi06_post2", ret, OK, scenario_end());

	stc_go(SLOT_HOLDER3);
	ret = stc_wait_count(&g_target, 2);
	TC_ASSERT_EQ_CLEANUP("pi06_post3", ret, OK, scenario_end());

	ret = release_waiter(SLOT_WAITER, 3);
	TC_ASSERT_EQ_CLEANUP("pi06_drained", ret, OK, scenario_end());

	ret = scenario_end();
	TC_ASSERT_EQ("pi06_no_leaked_actor", ret, 0);

	TC_SUCCESS_RESULT();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stc_sem_inherit_main
 ****************************************************************************/

int stc_sem_inherit_main(void)
{
	if (stc_harness_begin() != OK) {
		printf("\n[stc_sem_inherit] FAIL : cannot raise the harness priority\n");
		total_fail++;
		return ERROR;
	}

	stc_sem_pi01_boost_and_restore();
	stc_sem_pi02_no_boost_for_equal_or_lower();
	stc_sem_pi03_tracks_highest_waiter();
	stc_sem_pi04_inversion_is_bounded();
	stc_sem_pi05_all_holders_boosted();
	stc_sem_pi06_all_holders_restored();

	stc_harness_end();

	return OK;
}

#else							/* CONFIG_PRIORITY_INHERITANCE */

int stc_sem_inherit_main(void)
{
	printf("\n[stc_sem_inherit] SKIP : CONFIG_PRIORITY_INHERITANCE is not enabled\n");
	return OK;
}

#endif							/* CONFIG_PRIORITY_INHERITANCE */

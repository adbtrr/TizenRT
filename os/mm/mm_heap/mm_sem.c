/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
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
/****************************************************************************
 * mm/mm_heap/mm_sem.c
 *
 *   Copyright (C) 2007-2009, 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>
#include <debug.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sched.h>

#include <tinyara/mm/mm.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mm_seminitialize
 *
 * Description:
 *   Initialize the MM mutex
 *
 ****************************************************************************/

void mm_seminitialize(FAR struct mm_heap_s *heap)
{
	/* Initialize the MM semaphore to one (to support one-at-a-time access to
	 * private data sets.
	 */

	(void)sem_init(&heap->mm_semaphore, 0, 1);

	heap->mm_holder      = -1;
	heap->mm_counts_held = 0;
}

/****************************************************************************
 * Name: mm_trysemaphore
 *
 * Description:
 *   Try to take the MM mutex.  This is called only from the OS in certain
 *   conditions when it is necessary to have exclusive access to the memory
 *   manager but it is impossible to wait on a semaphore (e.g., the idle
 *   process when it performs its background memory cleanup).
 *
 ****************************************************************************/

int mm_trysemaphore(FAR struct mm_heap_s *heap)
{
	pid_t my_pid = getpid();

	/* Do I already have the semaphore? */

	if (heap->mm_holder == my_pid) {
		/* Yes, just increment the number of references that I have */

		heap->mm_counts_held++;
		return OK;
	} else {
		/* Try to take the semaphore (perhaps waiting) */

		if (sem_trywait(&heap->mm_semaphore) != 0) {
			return -get_errno();
		}

		/* As in mm_takesemaphore(), and for the same reason. This path
		 * acquires the heap too and is released through
		 * mm_givesemaphore(), so without this the unlock there would have
		 * no matching lock and would drive lockcount negative. It is
		 * reached from sched_ufree() and sched_kfree(), which take the heap
		 * here and give it back after the free.
		 */

		sched_lock();

		/* We have it.  Claim the stak and return */

		heap->mm_holder      = my_pid;
		heap->mm_counts_held = 1;
		return OK;
	}
}

/****************************************************************************
 * Name: mm_takesemaphore
 *
 * Description:
 *   Take the MM mutex.  This is the normal action before all memory
 *   management actions.
 *
 ****************************************************************************/

bool mm_takesemaphore(FAR struct mm_heap_s *heap)
{
#if defined(CONFIG_BUILD_FLAT) || defined(__KERNEL__)
	if (up_interrupt_context())
	{
#if defined(CONFIG_SMP)
		/* Can't take semaphore in SMP interrupt handler */
		return false;
#endif
	}
#endif
	pid_t my_pid = getpid();

	/* Do I already have the semaphore? */

	if (heap->mm_holder == my_pid) {
		/* Yes, just increment the number of references that I have */

		heap->mm_counts_held++;
	} else {
		/* Take the semaphore (perhaps waiting) */

		mvdbg("PID=%d taking\n", my_pid);

#ifdef CONFIG_MM_HANG_DETECT
		/* Say who we are about to wait for, before we wait. A heap
		 * semaphore whose holder never releases it blocks every later
		 * allocation here, and from the outside that is an allocator call
		 * that simply never returns, with nothing on the console.
		 *
		 * Printed only when the semaphore is actually contended, so an
		 * uncontended heap stays silent, and printed over lldbg() so it
		 * survives interrupts being disabled. If this is the last line the
		 * board prints, the holder it names is the task that never gave the
		 * heap back.
		 */

		if (sem_trywait(&heap->mm_semaphore) != 0) {
			lldbg("mm: pid %d waiting on heap %p, holder %d, count %d\n", my_pid, heap, heap->mm_holder, heap->mm_counts_held);

			while (sem_wait(&heap->mm_semaphore) != 0) {
				/* The only case that an error should occur here is
				 * if the wait was awakened by a signal.
				 */

				ASSERT(errno == EINTR);
			}
		}
#else
		while (sem_wait(&heap->mm_semaphore) != 0) {
			/* The only case that an error should occur here is if
			 * the wait was awakened by a signal.
			 */

			ASSERT(errno == EINTR);
		}
#endif

		/* We have it. Hold off rescheduling for as long as we do.
		 *
		 * This is a mutex with no priority inheritance: mm_seminitialize()
		 * creates it with sem_init() and never calls sem_setprotocol(), and
		 * CONFIG_PRIORITY_INHERITANCE is not set on every target. Nothing
		 * else keeps the holder on the CPU either, because the heap is
		 * protected by a semaphore rather than by disabling interrupts, so
		 * the whole of mm_malloc() and mm_free() has always been
		 * preemptible.
		 *
		 * That is an unbounded priority inversion waiting to happen. A low
		 * priority task takes the heap and is preempted; a high priority
		 * task blocks on the heap below; and any task of middling priority
		 * that is ready to run keeps the holder off the CPU indefinitely.
		 * The holder never releases, every later allocation piles up in the
		 * wait above, and the system stops with no fault and nothing on the
		 * console.
		 *
		 * The window is small while heap operations are short, which is why
		 * this went unnoticed. Anything that lengthens them opens it: with
		 * KASan the allocator writes the shadow map inside this very
		 * section, and the instrumented code around it runs two to three
		 * times slower.
		 *
		 * Locking the scheduler closes it by construction rather than by
		 * timing. Interrupts stay enabled throughout, so drivers and the
		 * watchdog are unaffected; only a context switch is deferred, and
		 * only for the length of one heap operation.
		 *
		 * Taken after the wait, never before, or the wait could block with
		 * rescheduling disabled and nothing could ever release it. Taken on
		 * the outermost acquire only: the recursive path above already runs
		 * with it held. sched_lock() is a no-op before there is a task and
		 * in interrupt context, and sched_unlock() makes the same two
		 * checks, so the pair stays balanced wherever it is reached.
		 */

		sched_lock();

		/* We have it.  Claim the stake and return */

		heap->mm_holder      = my_pid;
		heap->mm_counts_held = 1;
	}

	mvdbg("Holder=%d count=%d\n", heap->mm_holder, heap->mm_counts_held);
	return true;
}

/****************************************************************************
 * Name: mm_givesemaphore
 *
 * Description:
 *   Release the MM mutex when it is not longer needed.
 *
 ****************************************************************************/

void mm_givesemaphore(FAR struct mm_heap_s *heap)
{
#ifdef CONFIG_DEBUG
	pid_t my_pid = getpid();
#endif

	/* I better be holding at least one reference to the semaphore */

	DEBUGASSERT(heap->mm_holder == my_pid);

	/* Do I hold multiple references to the semphore */

	if (heap->mm_counts_held > 1) {
		/* Yes, just release one count and return */

		heap->mm_counts_held--;
		mvdbg("Holder=%d count=%d\n", heap->mm_holder, heap->mm_counts_held);
	} else {
		/* Nope, this is the last reference I have */

#ifdef CONFIG_DEBUG
		mvdbg("PID=%d giving\n", my_pid);
#endif

		heap->mm_holder      = -1;
		heap->mm_counts_held = 0;
		ASSERT(sem_post(&heap->mm_semaphore) == 0);

		/* Paired with the sched_lock() taken on the outermost acquire in
		 * mm_takesemaphore(). After the post, so that waking a waiter of
		 * higher priority costs one context switch here rather than one on
		 * the post and another on the unlock.
		 */

		sched_unlock();
	}
}

/****************************************************************************
 * Name: mm_is_sem_available
 *
 * Description:
 *   Check availability of mm semaphore 
 *
 ****************************************************************************/
void mm_is_sem_available(void *address)
{
	struct mm_heap_s *heap;

	heap = mm_get_heap(address);
	if (heap == NULL) {
		mdbg("Invalid Heap address given, Fail to check sem availability.\n");
		return;
	}
	DEBUGASSERT(mm_takesemaphore(heap));
	mm_givesemaphore(heap);
}

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

#ifndef __MM_MM_HEAP_MM_HANGDETECT_H
#define __MM_MM_HEAP_MM_HANGDETECT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <debug.h>
#include <assert.h>

#include <tinyara/mm/mm.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The heap walks the allocator performs are all over singly linked lists
 * threaded through heap metadata, and none of them is bounded: a corrupted
 * flink that points back into the list spins forever with the console
 * silent, inside an allocator call that simply never returns.
 *
 * No chunk is smaller than MM_MIN_CHUNK, so no honest list can be longer
 * than the heap holds chunks. A walk that passes that count has therefore
 * found a cycle, not a long list. That is a proof rather than a threshold,
 * so the bound never needs tuning and never fires on a healthy heap.
 *
 * The report goes out with lldbg(), which is lowsyslog() where the target
 * has CONFIG_ARCH_LOWPUTC. That path is polled and touches neither the
 * serial driver nor a semaphore, so it still reaches the console from
 * inside the heap critical section and with interrupts disabled, which is
 * exactly where these walks run.
 */

#ifdef CONFIG_MM_HANG_DETECT

/* The heap can hold mm_heapsize / MM_MIN_CHUNK chunks, so that many nodes
 * is the longest honest list; one more is allowed before calling it a cycle
 * so that a list which is legitimately at full length never trips.
 *
 * A heap whose size is not set yet has no meaningful bound, so the walk is
 * left unchecked rather than guessing one. No current caller walks before
 * mm_addregion() has accumulated the size, this only keeps a future one
 * from seeing a false report.
 */

#define MM_WALK_DECL(h)							\
	size_t _mm_walk_left = ((h)->mm_heapsize != 0 ?			\
				(h)->mm_heapsize / MM_MIN_CHUNK + 1 :	\
				(size_t)-1)

#define MM_WALK_STEP(what, p)							\
	do {									\
		if (_mm_walk_left-- == 0) {					\
			lldbg("mm: %s list cycles at %p, heap is corrupt\n",	\
			      (what), (p));					\
			PANIC();						\
		}								\
	} while (0)

#else

#define MM_WALK_DECL(h)		int _mm_walk_left __attribute__((unused)) = ((void)(h), 0)
#define MM_WALK_STEP(what, p)	do { } while (0)

#endif							/* CONFIG_MM_HANG_DETECT */
#endif							/* __MM_MM_HEAP_MM_HANGDETECT_H */

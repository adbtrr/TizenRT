/****************************************************************************
 *
 * Copyright 2023 Samsung Electronics All Rights Reserved.
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
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>
#include <stdlib.h>
#include <debug.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <queue.h>
#include <sys/types.h>
#include <tinyara/mm/mm.h>
#include <tinyara/mm/heap_regioninfo.h>
#include <arch/chip/memory_region.h>
#include <tinyara/binfmt/elf.h>

#include "binary_manager/binary_manager_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
#define CMN_BIN_IDX 0

#define MEM_ACCESS_UNIT    0x04
#define MAX_ALLOC_COUNT    CONFIG_MEM_LEAK_CHECKER_MAX_ALLOC_COUNT
#define HASH_SIZE          CONFIG_MEM_LEAK_CHECKER_HASH_TABLE_SIZE
#define MEM_DUMP_MAX_BYTES 32

#define MM_PREV_NODE_SIZE(x)            ((x)->preceding & ~MM_ALLOC_BIT)

/* The kernel heap plus the heap of every application */

#ifdef CONFIG_APP_BINARY_SEPARATION
#define MAX_CHECK_TARGETS  (CONFIG_NUM_APPS + 1)
#else
#define MAX_CHECK_TARGETS  1
#endif

struct alloc_node_info_s {
	volatile struct mm_allocnode_s *node;
	struct alloc_node_info_s *next;
};

/* A heap which is checked for leaks, and the name it is reported under */

struct check_target_s {
	struct mm_heap_s *heap;
	char *name;
};

static struct alloc_node_info_s **g_hash_table;
static struct alloc_node_info_s *g_node_info;

/* Number of chunks registered in g_node_info[]. The chunks of every checked
 * heap share one table, so this runs across the heaps instead of restarting
 * for each of them.
 */
static int g_node_total;

static struct check_target_s g_target[MAX_CHECK_TARGETS];
static int g_target_total;

static int hash_init(void)
{
	int index;

	g_hash_table = (struct alloc_node_info_s **)malloc(sizeof(struct alloc_node_info_s *) * HASH_SIZE);
	if (!g_hash_table) {
		return ERROR;
	}

	g_node_info = (struct alloc_node_info_s*)malloc(sizeof(struct alloc_node_info_s) * MAX_ALLOC_COUNT);
	if (!g_node_info) {
		free(g_hash_table);
		return ERROR;
	}

	for (index = 0; index < HASH_SIZE; ++index) {
		g_hash_table[index] = NULL;
	}

	return OK;
}

static void hash_deinit(void)
{
	free(g_hash_table);
	free(g_node_info);
	g_hash_table = NULL;
	g_node_info = NULL;
}

static void add_hash(int index)
{
	long key;
	struct alloc_node_info_s *cur;

	key = (long)g_node_info[index].node % HASH_SIZE;
	if (g_hash_table[key] == NULL) {
		g_hash_table[key] = &g_node_info[index];
		return;
	}

	cur = g_hash_table[key];
	while (cur->next) {
		cur = cur->next;
	}
	cur->next = &g_node_info[index];
}

static bool search_hash(unsigned long value)
{
	long key = value % HASH_SIZE;
	struct alloc_node_info_s *cur = g_hash_table[key];

	while (cur != NULL) {
		if ((unsigned long)cur->node == value) {
			if (cur->node->memory_state == MM_MEMORY_STATE_USED) {
				return false;
			}
			cur->node->memory_state = MM_MEMORY_STATE_USED;
			return true;
		}
		if (cur->next == NULL) {
			return false;
		}
		cur = cur->next;
	}
	return false;
}

static int get_node_cnt(struct mm_heap_s *heap)
{
	volatile struct mm_allocnode_s *node;
	mmsize_t node_size;
	node_size = SIZEOF_MM_ALLOCNODE;

	int ret = 0;
	
#if CONFIG_KMM_REGIONS > 1
	int region;
#else
#define region 0
#endif

	/* Visit each region */

#if CONFIG_KMM_REGIONS > 1
	for (region = 0; region < heap->mm_nregions; region++)
#endif
	{
		node_size = SIZEOF_MM_ALLOCNODE;
		for (node = heap->mm_heapstart[region]; node < heap->mm_heapend[region]; node = (struct mm_allocnode_s *)((char *)node + node->size)) {
			ASSERT(node->size);
			/* Ignore the heap start checking, because there is a guard node in heap start */
			if (node == heap->mm_heapstart[region]) {
				continue;
			}
			/* Check broken link */
			if (node_size != MM_PREV_NODE_SIZE(node)) {
				continue;
			}
			node_size = node->size;
			/* Check if the node corresponds to an allocated memory chunk */
			if ((node->preceding & MM_ALLOC_BIT) != 0) {
				ret++;
			}
		}
	}

	return ret;
}

static void fill_hash_table(struct mm_heap_s *heap)
{
	volatile struct mm_allocnode_s *node;
	mmsize_t node_size;
	node_size = SIZEOF_MM_ALLOCNODE;

	mm_takesemaphore(heap);

#if CONFIG_KMM_REGIONS > 1
	int region;
#else
#define region 0
#endif

	/* Visit each region */

#if CONFIG_KMM_REGIONS > 1
	for (region = 0; region < heap->mm_nregions; region++)
#endif
	{
		node_size = SIZEOF_MM_ALLOCNODE;
		for (node = heap->mm_heapstart[region]; node < heap->mm_heapend[region]; node = (struct mm_allocnode_s *)((char *)node + node->size)) {
			ASSERT(node->size);
			/* Ignore the heap start checking, because there is a guard node in heap start */
			if (node == heap->mm_heapstart[region]) {
				continue;
			}

			/* Check broken link */
			if (node_size != MM_PREV_NODE_SIZE(node)) {
				node->memory_state = MM_MEMORY_STATE_BROKEN;
				continue;
			}
			node_size = node->size;
			if ((unsigned long)node + (unsigned long)SIZEOF_MM_ALLOCNODE == (unsigned long)g_node_info ||
					(unsigned long)node + (unsigned long)SIZEOF_MM_ALLOCNODE == (unsigned long)g_hash_table) {
				continue;
			}
			/* Check if the node corresponds to an allocated memory chunk */
			if ((node->preceding & MM_ALLOC_BIT) != 0) {
				if (g_node_total >= MAX_ALLOC_COUNT) {
					/* The heaps grew after they were counted. Stop instead
					 * of writing past the end of g_node_info[].
					 */
					break;
				}
				g_node_info[g_node_total].node = node;
				g_node_info[g_node_total].next = NULL;
				node->memory_state = MM_MEMORY_STATE_LEAK;
				add_hash(g_node_total);
				g_node_total++;
			}
		}
	}
	mm_givesemaphore(heap);
}

static void search_addr(void *start_addr, void *end_addr)
{
	/* This function traverse the memory from start_addr to end_addr for comparing the address based on hash table. */
	void *leak_chk;

	/* Not to access over its region, subtract 0x04 from the end of the address. */
	for (leak_chk = start_addr; leak_chk < end_addr - MEM_ACCESS_UNIT; leak_chk++) {
		search_hash(*(unsigned long volatile *)leak_chk - (unsigned long)SIZEOF_MM_ALLOCNODE);
	}
}

static void heap_check(struct mm_heap_s *heap, int checker_pid)
{
	void *leak_chk;
	struct mm_allocnode_s *visit_node;
	void *exclude_top;
	void *exclude_bottom;

	struct tcb_s *ctcb = sched_gettcb(checker_pid);
	ASSERT(ctcb != NULL);
	exclude_top = ctcb->adj_stack_ptr;
	exclude_bottom = ctcb->adj_stack_ptr - ctcb->adj_stack_size;

#if CONFIG_KMM_REGIONS > 1
	int region;
#else
#define region 0
#endif

	/* Visit each region */

#if CONFIG_KMM_REGIONS > 1
	for (region = 0; region < heap->mm_nregions; region++)
#endif
	{
		for (visit_node = heap->mm_heapstart[region]; visit_node < heap->mm_heapend[region]; visit_node = (struct mm_allocnode_s *)((char *)visit_node + visit_node->size)) {
			if ((visit_node->preceding & MM_ALLOC_BIT) != 0) {
				if ((void *)((char *)visit_node + SIZEOF_MM_ALLOCNODE) == (void *)g_node_info) {
					continue;
				}
				for (leak_chk = (void *)visit_node; leak_chk < (void *)(((char *)visit_node) + visit_node->size); leak_chk++) {
					if ((leak_chk >= exclude_bottom && leak_chk <= exclude_top)) {
						continue;
					}
					search_hash(*(unsigned long volatile *)leak_chk - (unsigned long)SIZEOF_MM_ALLOCNODE);
				}
			}
		}
	}
}

static struct mm_heap_s * init_mem_leak_checker(int checker_pid, char *bin_name);

/****************************************************************************
 * Name: collect_targets
 *
 * Description:
 *   Build the list of heaps which exist right now: the kernel heap and the
 *   heap of every loaded application.
 *
 *   The list is used for two things. It is the set of heaps to visit while
 *   searching for references, and, when the whole system is checked, it is
 *   also the set of heaps to report on.
 *
 *   Returns the number of heaps found.
 *
 ****************************************************************************/

static int collect_targets(void)
{
#ifdef CONFIG_APP_BINARY_SEPARATION
	bin_addr_info_t *info;
	struct mm_heap_s *app_heap;
	int bin_idx;
#endif

	g_target_total = 0;

	g_target[g_target_total].heap = kmm_get_baseheap();
	g_target[g_target_total].name = "Kernel";
	g_target_total++;

#ifdef CONFIG_APP_BINARY_SEPARATION
	/* Index CMN_BIN_IDX is the common binary, which has no heap of its own,
	 * so the applications start at the next index.
	 */

	info = (bin_addr_info_t *)get_bin_addr_list();
	for (bin_idx = CMN_BIN_IDX + 1; bin_idx <= CONFIG_NUM_APPS && g_target_total < MAX_CHECK_TARGETS; bin_idx++) {
		if (info[bin_idx].text_addr == 0) {
			/* This binary is not loaded */
			continue;
		}
		app_heap = mm_get_app_heap_with_name(BIN_NAME(bin_idx));
		if (app_heap == NULL) {
			continue;
		}
		g_target[g_target_total].heap = app_heap;
		g_target[g_target_total].name = BIN_NAME(bin_idx);
		g_target_total++;
	}
#endif

	return g_target_total;
}

/****************************************************************************
 * Name: ram_check
 *
 * Description:
 *   Visit every place a reference to a checked chunk can be kept, and mark
 *   the referenced chunks.
 *
 *   The set of places to visit does not depend on which heap is checked.
 *   A chunk of the kernel heap can be referenced by an application just as
 *   an application chunk can be referenced by the kernel, so all data
 *   regions and all heaps have to be visited in either case.
 *
 *   Checking the kernel used to visit the kernel data regions and the
 *   kernel heap only. Every kernel chunk which was referenced from an
 *   application was reported as a leak, because the reference itself was
 *   never visited. That is not limited to a corner case: with XIP the data,
 *   bss and heap of an application are placed by the user space linker
 *   script instead of being carved out of the kernel heap, so no part of
 *   the application memory was visited by the kernel check at all.
 *
 *   This is by far the most expensive phase, because every position of
 *   every visited region is read. Since what it visits does not depend on
 *   the heap being reported, it runs once for all of them.
 *
 ****************************************************************************/

static void ram_check(int checker_pid)
{
	int mem_region_idx;
	int idx;
#ifdef CONFIG_APP_BINARY_SEPARATION
	bin_addr_info_t *info;
	int bin_idx;
#endif

	/* Visit all the data regions of the kernel */

	for (mem_region_idx = 0; mem_region_idx < MEM_VAR_REGION_COUNT; mem_region_idx++) {
		search_addr(variable_region_start_addr[mem_region_idx], variable_region_end_addr[mem_region_idx]);
	}

#ifdef CONFIG_APP_BINARY_SEPARATION
	/* Visit the data and the bss region of every loaded binary. Index
	 * CMN_BIN_IDX is the common binary and is covered by the same loop.
	 */

	info = (bin_addr_info_t *)get_bin_addr_list();
	for (bin_idx = CMN_BIN_IDX; bin_idx <= CONFIG_NUM_APPS; bin_idx++) {
		if (info[bin_idx].text_addr == 0) {
			continue;
		}
		search_addr((void *)info[bin_idx].data_addr, (void *)(info[bin_idx].data_addr + info[bin_idx].data_size));
		search_addr((void *)info[bin_idx].bss_addr, (void *)(info[bin_idx].bss_addr + info[bin_idx].bss_size));
	}
#endif

	/* Visit the kernel heap and the heap of every loaded application */

	for (idx = 0; idx < g_target_total; idx++) {
		heap_check(g_target[idx].heap, checker_pid);
	}
}

static void print_mem_hex_dump(void *addr, size_t alloc_size)
{
	unsigned char *ptr = (unsigned char *)addr;
	size_t dump_size = (alloc_size < MEM_DUMP_MAX_BYTES) ? alloc_size : MEM_DUMP_MAX_BYTES;
	size_t i;

	printf("[DATA] ");
	for (i = 0; i < dump_size; i++) {
		printf("%02x ", ptr[i]);
		if ((i + 1) % 16 == 0 && (i + 1) < dump_size) {
			printf("\n       ");
		}
	}
	printf("\n");
}

/****************************************************************************
 * Name: print_info
 *
 * Description:
 *   Report the chunks of one heap which are still marked as a leak after
 *   the search for references has run.
 *
 *   The counters are taken from the same walk which prints the chunks
 *   instead of being carried along the search, so that the totals always
 *   describe exactly what was printed.
 *
 ****************************************************************************/

static void print_info(struct mm_heap_s *heap)
{
	volatile struct mm_allocnode_s *node;
	uint32_t owner_addr;
	int leak_cnt = 0;
	int broken_cnt = 0;

#if CONFIG_KMM_REGIONS > 1
	int region;
#else
#define region 0
#endif

	mm_takesemaphore(heap);

	/* Count first, so that the totals and the printed chunks come from the
	 * same state of the heap.
	 */

#if CONFIG_KMM_REGIONS > 1
	for (region = 0; region < heap->mm_nregions; region++)
#endif
	{
		for (node = heap->mm_heapstart[region]; node < heap->mm_heapend[region]; node = (struct mm_allocnode_s *)((char *)node + node->size)) {
			ASSERT(node->size);
			if (node->memory_state == MM_MEMORY_STATE_LEAK) {
				leak_cnt++;
			} else if (node->memory_state == MM_MEMORY_STATE_BROKEN) {
				broken_cnt++;
			}
		}
	}

	if (leak_cnt > 0 || broken_cnt > 0) {
		printf("Type   |    Addr    | Size(byte) |    Owner   | PID \n");
		printf("---------------------------------------------------\n");

		/* Visit each region */

#if CONFIG_KMM_REGIONS > 1
		for (region = 0; region < heap->mm_nregions; region++)
#endif
		{
			for (node = heap->mm_heapstart[region]; node <  heap->mm_heapend[region]; node = (struct mm_allocnode_s *)((char *)node + node->size)) {
				ASSERT(node->size);
				if (node->memory_state == MM_MEMORY_STATE_LEAK) {
					/* alloc_call_addr can be from kernel, app or common binary.
					* based on the text addresses printed, user needs to check the
					* corresponding binaries accordingly
					*/
					owner_addr = (uint32_t)node->alloc_call_addr;
					pid_t pid = node->pid;
					if (pid < 0) {
						/* For stack allocated node, pid is negative value.
						* To use the pid, change it to original positive value.
						*/
						pid = (-1) * pid;
					}
					printf("LEAK   | %10p |  %8d  | %10p | %d\n", (void *)((char *)node + SIZEOF_MM_ALLOCNODE), node->size - SIZEOF_MM_ALLOCNODE, owner_addr, pid);
					print_mem_hex_dump((void *)((char *)node + SIZEOF_MM_ALLOCNODE), node->size - SIZEOF_MM_ALLOCNODE);
				} else if (node->memory_state == MM_MEMORY_STATE_BROKEN) {
					printf("BROKEN | %p\n", node);
				}
			}
		}

		printf("*** %d LEAKS, %d BROKENS.\n", leak_cnt, broken_cnt);
	} else {
		printf("*** NO MEMORY LEAK.\n");
	}

	mm_givesemaphore(heap);
}

/****************************************************************************
 * Name: check_reported_targets
 *
 * Description:
 *   Register the chunks of the heaps in report[0..nreport-1], run the
 *   search for references once, and report each of those heaps.
 *
 *   All of the heaps share one registration table and one search, so the
 *   expensive phase runs once no matter how many heaps are reported. It
 *   used to run once per reported heap, and each of those runs visited
 *   every region again.
 *
 ****************************************************************************/

static int check_reported_targets(int checker_pid, struct check_target_s *report, int nreport)
{
	int node_cnt = 0;
	int idx;

	for (idx = 0; idx < nreport; idx++) {
		node_cnt += get_node_cnt(report[idx].heap);
	}

	if (MAX_ALLOC_COUNT < node_cnt) {
		printf("Available buffer size (%d) is small.\nPlease increase CONFIG_MEM_LEAK_CHECKER_MAX_ALLOC_COUNT value more than %d.\n", MAX_ALLOC_COUNT, node_cnt);
		return ERROR;
	}

	if (g_hash_table || g_node_info) {
		printf("mem_leak_checker is already running.\n");
		return ERROR;
	}

	if (hash_init() != OK) {
		printf("hash table memory alloc is failed.\n");
		return ERROR;
	}

	/* Register the chunks of every reported heap in the one table */

	g_node_total = 0;
	for (idx = 0; idx < nreport; idx++) {
		fill_hash_table(report[idx].heap);
	}

	/* Visit RAM region. Shared by every reported heap. */

	ram_check(checker_pid);

	/* A reported heap is normally one of the heaps ram_check() visited.
	 * Visit it here if it was not, so that a reference kept by the reported
	 * heap itself can never be missed.
	 */

	for (idx = 0; idx < nreport; idx++) {
		int visited = 0;
		int target_idx;

		for (target_idx = 0; target_idx < g_target_total; target_idx++) {
			if (g_target[target_idx].heap == report[idx].heap) {
				visited = 1;
				break;
			}
		}

		if (!visited) {
			heap_check(report[idx].heap, checker_pid);
		}
	}

	for (idx = 0; idx < nreport; idx++) {
		printf("\n%s :\n", report[idx].name);
		print_info(report[idx].heap);
	}

	hash_deinit();
	return OK;
}

int run_mem_leak_checker(int checker_pid, char *bin_name)
{
	struct check_target_s report;

	if (collect_targets() <= 0) {
		printf("Can't found any heap to check.\n");
		return ERROR;
	}

	if (strncmp(bin_name, "kernel", strlen("kernel") + 1) == 0) {
		report.heap = kmm_get_baseheap();
	}
#ifdef CONFIG_APP_BINARY_SEPARATION
	else {
		report.heap = mm_get_app_heap_with_name(bin_name);
	}
#endif

	if (!report.heap) {
		printf("Can't found heap, bin name: %s", bin_name);
		return ERROR;
	}
	report.name = bin_name;

	/* Only the requested heap is reported, but the search still visits
	 * every heap, because any of them can hold the reference.
	 */

	return check_reported_targets(checker_pid, &report, 1);
}

int run_all_mem_leak_checker(int checker_pid)
{
	if (collect_targets() <= 0) {
		printf("Can't found any heap to check.\n");
		return ERROR;
	}

#ifdef CONFIG_APP_BINARY_SEPARATION
	printf("\nBelow are text addresses of loadable apps (and common binary if enabled) :\n");
	printf("The pc value of the allocation can be obtained by subtracting the text start address of the appropriate binary\n\n");
	bin_addr_info_t *bin_addr_info = (bin_addr_info_t *)get_bin_addr_list();
	int bin_idx;
	for (bin_idx = 0; bin_idx <= CONFIG_NUM_APPS; bin_idx++) {
		if (bin_addr_info[bin_idx].text_addr != 0) {
			printf("[%s] Text Addr : %p, Text Size : %u\n", BIN_NAME(bin_idx), bin_addr_info[bin_idx].text_addr, bin_addr_info[bin_idx].text_size);
		}
	}
#endif

	/* Report the kernel heap and the heap of every loaded application from
	 * a single search.
	 */

	return check_reported_targets(checker_pid, g_target, g_target_total);
}

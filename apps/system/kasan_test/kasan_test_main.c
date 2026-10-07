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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <tinyara/mm/kasan.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kasan_case_s {
	FAR const char *name;
	int value;
	bool reports;
	FAR const char *what;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct kasan_case_s g_cases[] = {
	{"inbounds",  KASAN_SELFTEST_INBOUNDS,       false, "write inside the block"},
	{"overflow",  KASAN_SELFTEST_OVERFLOW_WRITE, true,  "write one byte past the block"},
	{"read",      KASAN_SELFTEST_OVERFLOW_READ,  true,  "read one byte past the block"},
	{"underflow", KASAN_SELFTEST_UNDERFLOW,      true,  "write one byte before the block"},
	{"uaf",       KASAN_SELFTEST_USE_AFTER_FREE, true,  "write to a freed block"},
	{"straddle",  KASAN_SELFTEST_STRADDLE,       true,  "word write across the block end"},
};

#define KASAN_NCASES	(int)(sizeof(g_cases) / sizeof(g_cases[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void kasan_test_usage(void)
{
	int i;

	printf("Usage: kasan_test <case>\n\n");
	printf("  %-10s  %-34s  %s\n", "case", "what it does", "expected");
	printf("  %-10s  %-34s  %s\n", "----", "------------", "--------");

	for (i = 0; i < KASAN_NCASES; i++) {
		printf("  %-10s  %-34s  %s\n", g_cases[i].name, g_cases[i].what, g_cases[i].reports ? "report, then assert" : "nothing");
	}

	printf("\nRun 'inbounds' first. It touches the last accessible byte of a\n");
	printf("block and must stay silent; if it reports, the allocator hook is\n");
	printf("unpoisoning less than it hands out and no other result here means\n");
	printf("anything.\n\n");
	printf("Every other case is meant to be caught. A KASAN report followed by\n");
	printf("an assert is the pass. Silence is the failure, and means the code\n");
	printf("under test was never instrumented.\n\n");
	printf("The board will reset on a caught case if it is configured to reset\n");
	printf("on assert, so run one case per boot.\n\n");
	printf("Control:\n");
	printf("  %-10s  %s\n", "arm", "start reporting");
	printf("  %-10s  %s\n", "disarm", "stop reporting");
	printf("  %-10s  %s\n", "status", "say whether reporting is on");
	printf("\nWith CONFIG_MM_KASAN_START_MANUAL the board boots disarmed, so a\n");
	printf("report can only come from what you run after 'arm'. The shadow map\n");
	printf("is maintained either way, so blocks allocated before arming are\n");
	printf("still tracked.\n");
}

/* Reporting control. Called straight through: this is a flat build, so the
 * application and the kernel are one image and one link.
 */

static int kasan_test_control(FAR const char *cmd)
{
	if (strcmp(cmd, "arm") == 0) {
		kasan_start();
		printf("kasan_test: armed - reporting is on\n");
		return EXIT_SUCCESS;
	}

	if (strcmp(cmd, "disarm") == 0) {
		kasan_stop();
		printf("kasan_test: disarmed - reporting is off\n");
		return EXIT_SUCCESS;
	}

	if (strcmp(cmd, "status") == 0) {
		printf("kasan_test: reporting is %s\n", kasan_is_armed() ? "ON" : "off");

		if (!kasan_is_armed()) {
			printf("            the shadow map is still being maintained;\n");
			printf("            run 'kasan_test arm' to begin reporting.\n");
		}

		return EXIT_SUCCESS;
	}

	return -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kasan_test_main(int argc, FAR char *argv[])
{
	FAR const struct kasan_case_s *sel = NULL;
	int i;
	int ret;

	if (argc != 2) {
		kasan_test_usage();
		return EXIT_FAILURE;
	}

	ret = kasan_test_control(argv[1]);
	if (ret >= 0) {
		return ret;
	}

	for (i = 0; i < KASAN_NCASES; i++) {
		if (strcmp(argv[1], g_cases[i].name) == 0) {
			sel = &g_cases[i];
			break;
		}
	}

	if (sel == NULL) {
		printf("kasan_test: no case named '%s'\n\n", argv[1]);
		kasan_test_usage();
		return EXIT_FAILURE;
	}

	printf("kasan_test: %s - %s\n", sel->name, sel->what);
	printf("kasan_test: expecting %s\n", sel->reports ? "a KASAN report and an assert" : "no report");

	/* Flushed before the call because a caught case asserts inside the
	 * kernel and never comes back, taking anything still buffered with it.
	 */

	fflush(stdout);

	/* Called straight through. This is a flat build: the application and
	 * the kernel are one image and one link, so no syscall or ioctl bridge
	 * is needed to reach a kernel function. The faulting access still has
	 * to be made by kasan_selftest(), which lives outside os/mm and is
	 * therefore instrumented, against a block from the registered heap.
	 */

	ret = kasan_selftest(sel->value);
	if (ret < 0) {
		printf("kasan_test: the self test refused the request: %d\n", -ret);
		return EXIT_FAILURE;
	}

	/* Reached only when nothing was reported. */

	if (sel->reports) {
		printf("kasan_test: FAIL - '%s' returned with nothing reported.\n", sel->name);

		if (sel->value == KASAN_SELFTEST_OVERFLOW_READ) {
			printf("            Expected when CONFIG_MM_KASAN_DISABLE_READS_CHECK\n");
			printf("            is set, since reads are then not instrumented.\n");
		} else {
			printf("            Either the kernel was not built with\n");
			printf("            -fsanitize=kernel-address, or the heap it\n");
			printf("            allocated from was never registered with KASan.\n");
		}

		return EXIT_FAILURE;
	}

	printf("kasan_test: PASS - '%s' completed with no report.\n", sel->name);
	return EXIT_SUCCESS;
}

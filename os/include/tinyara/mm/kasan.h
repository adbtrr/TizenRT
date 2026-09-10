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
 * os/include/tinyara/mm/kasan.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Derived from Apache NuttX include/nuttx/mm/kasan.h at commit b4aa94d.
 * The software tag, hardware tag and global instrumentation interfaces of
 * the original are not present; TizenRT supports generic mode only.
 *
 ****************************************************************************/

#ifndef __INCLUDE_MM_KASAN_H
#define __INCLUDE_MM_KASAN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <stdbool.h>
#include <stddef.h>

#include <tinyara/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_MM_KASAN

/* With KASan disabled every entry point collapses to nothing, so the call
 * sites in the memory manager need no conditional compilation of their own.
 */

#define kasan_poison(addr, size)
#define kasan_unpoison(addr, size)	((FAR void *)(addr))
#define kasan_register(addr, size)
#define kasan_unregister(addr)
#define kasan_start()
#define kasan_stop()
#define kasan_init_early()

#else

/* Called from the earliest point of the boot sequence. Checking must stay
 * disabled until the first region is registered, because instrumented code
 * runs long before any heap exists, and on some platforms before .bss has
 * been cleared.
 */

#define kasan_init_early()	kasan_stop()

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: kasan_poison
 *
 * Description:
 *   Mark the memory range as inaccessible. Any instrumented access to a
 *   poisoned address is reported.
 *
 * Input Parameters:
 *   addr - range start address
 *   size - range size in bytes
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void kasan_poison(FAR const void *addr, size_t size);

/****************************************************************************
 * Name: kasan_unpoison
 *
 * Description:
 *   Mark the memory range as accessible.
 *
 * Input Parameters:
 *   addr - range start address
 *   size - range size in bytes
 *
 * Returned Value:
 *   The address that was passed in, so the call can wrap an allocator
 *   return value directly.
 *
 ****************************************************************************/

FAR void *kasan_unpoison(FAR const void *addr, size_t size);

/****************************************************************************
 * Name: kasan_register
 *
 * Description:
 *   Monitor the memory range for invalid access checking.
 *
 *   The shadow map for the region is carved out of the tail of the region
 *   itself, so no linker script support is needed. On return *size has been
 *   reduced by the number of bytes taken for the shadow, and the caller must
 *   build its heap over the reduced range only.
 *
 *   The whole region is left poisoned, so an allocator must call
 *   kasan_unpoison() for each block it hands out.
 *
 *   Checking becomes active from the first successful registration.
 *
 * Input Parameters:
 *   addr - range start address
 *   size - on entry the region size, on return the size left for the caller
 *
 * Returned Value:
 *   None. If the region cannot be registered *size is left unchanged and
 *   the region is simply not covered.
 *
 ****************************************************************************/

void kasan_register(FAR void *addr, FAR size_t *size);

/****************************************************************************
 * Name: kasan_unregister
 *
 * Description:
 *   Stop monitoring the memory range beginning at addr.
 *
 * Input Parameters:
 *   addr - range start address, as passed to kasan_register()
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void kasan_unregister(FAR void *addr);

/****************************************************************************
 * Name: kasan_start
 *
 * Description:
 *   Allow KASan to report. Called automatically by kasan_register().
 *
 ****************************************************************************/

/****************************************************************************
 * Name: kasan_stop
 *
 * Description:
 *   Stop KASan reporting. Used before the boot sequence has a heap, and by
 *   the assertion path so that the crash dump can walk the heap without
 *   tripping a second report.
 *
 ****************************************************************************/

#ifdef CONFIG_MM_KASAN_INSTRUMENT
void kasan_start(void);
void kasan_stop(void);
#else
#define kasan_start()
#define kasan_stop()
#endif

#ifdef __cplusplus
}
#endif

#endif							/* CONFIG_MM_KASAN */
#endif							/* __INCLUDE_MM_KASAN_H */

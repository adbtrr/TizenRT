Patch series for upstream pull request 7475
===========================================

Upstream PR 7475, "os/mm: Add use-after-free detection to the heap allocator",
adds a poison-and-verify use-after-free detector to the TinyAra heap.

These patches are review fixes for that change, followed by the work needed to
cover the case the change cannot see. They apply on top of the pull request's
own commit, in numbered order.

There are 14 of them.

  base:  20fb5f3c  os/mm: Add use-after-free detection to the heap allocator

Applying
--------

  git checkout -b <branch> 20fb5f3c
  git am /path/to/os/pr7475_patches/*.patch

Part 1 - review fixes (0001 to 0010)
------------------------------------

None of these change what the detector catches. They fix the reporting path,
the configuration surface and the coverage the design gives away for nothing.

  0001  Bound the hex dump.
        A corrupted top of heap chunk dumped the whole remaining heap, one
        line per 32 bytes, with the memory manager semaphore held. Adds
        CONFIG_DEBUG_MM_UAF_DUMP_SIZE, default 256. Small chunks are still
        dumped in full, so a typical report is unchanged.

  0002  Add "range 4 4096" to CONFIG_DEBUG_MM_UAF_POISON_SIZE.
        The value is compared against a size_t, so a negative setting became
        a huge unsigned number, defeated the clamp and made every free and
        every malloc linear in the chunk size. No effect at the default.

  0003  Repair the pattern after reporting.
        A neighbour verified by free or realloc usually stays on the free
        list, so a single defect was re-reported forever. Also adds
        CONFIG_DEBUG_MM_UAF_MAX_REPORTS as an optional console budget.

  0004  Verify after the chunk leaves the free list.
        All seven checks ran while the chunk was still linked, and the memory
        manager semaphore is recursive per PID, so a nested allocation could
        hand out the chunk the caller was about to use. Detection is
        unchanged: removal only rewrites the neighbours' links.

  0005  Stop over-aligning the poisoned window.
        The window started at MM_ALIGN_UP(SIZEOF_MM_FREENODE), rounding to the
        16 byte granule when 4 byte alignment is all that is needed. Recovers
        8 bytes of coverage on the common HEAPINFO configuration and gives a
        32 byte chunk a window where it previously had none.

  0006  Enable the option in build/configs/qemu/build_test.
        Nothing in the tree set CONFIG_DEBUG_MM_UAF, so continuous
        integration compiled none of the feature.

  0007  Make mm_dump_heap_region() take uintptr_t, and stop it reading up to
        28 bytes past the end of a region.

  0008  Say in the Kconfig help that a reallocated chunk is not covered.

  0009  Split a range based poison and verify primitive out of the node based
        functions. No behavioural change; this is what 0011 and 0012 need in
        order to cover a whole payload without reopening every call site.

  0010  Cleanups: empty macros for the disabled case so the call sites lose
        their ifdefs, pass the heap so the report can print the free node
        list, include tinyara/arch.h, drop an unused include, FAR
        consistency, an unsigned pattern constant.

Part 2 - the reallocation case (0011 to 0014)
---------------------------------------------

The detector only watches a chunk while it is on the free list. Once the
chunk has been handed out again the address legitimately belongs to the new
owner, so no check of the contents can tell a stale write apart from a valid
one. Delaying the reuse is the only way to separate them without hardware
support for pointer identity.

  0011  Add a bounded quarantine. A freed chunk is held out of the free list
        with its allocated bit still set, so neither malloc nor the merge
        logic can reach it. malloc flushes the quarantine before reporting
        failure, so this never turns into an allocation failure that would
        not happen with the option off. A held chunk still looks allocated,
        which the existing double free test cannot see, so the ring is
        scanned explicitly; that also reports a double free with both
        callers.

  0012  Poison the whole payload of a held chunk. A held chunk is in no list,
        so there are no links to preserve, and the first bytes of the payload
        become checkable. That is where a write to the first member of a
        freed object lands.

  0013  Add apps/examples/mm_uaf_test, which trips the detector in each shape
        it should catch and prints what was expected of each one, including a
        case of well behaved traffic which must stay silent.

  0014  Add apps/examples/mm_quarantine_test, which measures the quarantine
        rather than reading its log: a freed address is not handed straight
        back, the delay is bounded so memory is still recycled, and held
        memory is released rather than failing an allocation. Each check
        prints pass or fail and the exit status reflects the result. The last
        check is a regression test for the flush in mm_malloc(): modelling the
        allocator without that hook makes it fall short by exactly the ring
        depth.

Configuration
-------------

  CONFIG_DEBUG_MM_UAF                     the detector
  CONFIG_DEBUG_MM_UAF_POISON_SIZE         bytes checked per free chunk (64)
  CONFIG_DEBUG_MM_UAF_DUMP_SIZE           cap on the hex dump (256)
  CONFIG_DEBUG_MM_UAF_MAX_REPORTS         console budget, 0 for no limit
  CONFIG_DEBUG_MM_UAF_PANIC               assert instead of reporting

  CONFIG_DEBUG_MM_QUARANTINE              delay the reuse of freed chunks
  CONFIG_DEBUG_MM_QUARANTINE_CHUNKS       ring size (32)
  CONFIG_DEBUG_MM_QUARANTINE_BYTES        bytes held at once (4096)
  CONFIG_DEBUG_MM_QUARANTINE_MAX_SIZE     largest chunk held (1024)

CONFIG_DEBUG_MM_HEAPINFO is worth enabling alongside: without it the report
has no owning task and no allocating address to print.

Known limits
------------

Quarantine widens the window in which a stale write is still detectable; it
does not close it. A write which happens after the chunk has aged out and
been reallocated is still missed, and the depth is a tuning knob rather than
a guarantee. Reads through a stale pointer are not detected at all, only
writes. Detection is deferred to the moment the chunk is released, so the
call stack in the report belongs to the releasing allocation, not to the
offending write; the node header is what identifies the culprit.

Quarantine support for upstream pull request 7475
=================================================

Upstream PR 7475, "os/mm: Add use-after-free detection to the heap allocator",
fills a freed chunk with a poison pattern and checks that pattern at the
moment the chunk is handed out again.

That check only works in the gap between the free and the next handout, and
the allocator normally hands a just-freed chunk straight back, because it is a
perfect fit for the next request of the same size. So the gap is often a
single malloc call wide. If one thread frees a block, a second thread
allocates it, and the first thread only then writes through its stale pointer,
the write falls outside the gap and nothing reports it.

These patches close that case, and fix the two things in the existing code
which get in its way. They apply on top of the pull request's own commit, in
numbered order.

  base:  20fb5f3c  os/mm: Add use-after-free detection to the heap allocator

Applying
--------

  git checkout -b <branch> 20fb5f3c
  git am /path/to/os/pr7475_quarantine_patches/*.patch

The patches
-----------

  0001  Bound the hex dump and repair the pattern after reporting.

        The dump covered the whole chunk, so a corrupted top of heap chunk
        printed one line per 32 bytes of the remaining heap with the memory
        manager semaphore held - long enough on a serial console to trip the
        watchdog. The pattern was also left corrupted after a report, so a
        neighbour that mm_realloc() inspects but does not consume was
        re-reported on every later realloc, forever, from one defect.

  0002  Stop rounding the poisoned window up to the allocator granule.

        The window began at MM_ALIGN_UP(SIZEOF_MM_FREENODE) when word
        alignment is all it needs. On the common HEAPINFO configuration that
        left eight bytes of user data unchecked, and gave a 32 byte chunk no
        coverage at all - every freed allocation of 16 bytes or less. This is
        pure detection coverage the old code was walking straight past.

  0003  Split a range based poison and verify primitive out of the node based
        functions. No behavioural change; 0004 and 0005 need it in order to
        cover a whole payload while the free list path keeps its window.

  0004  Hold freed chunks in a bounded ring instead of returning them to the
        free list, so the address is not handed to anybody else while the
        original owner may still write through it. A held chunk keeps its
        allocated bit, which is what makes it unreachable by malloc and
        unmergeable by its neighbours. malloc flushes the ring before
        reporting failure, so this never becomes an allocation failure that
        would not have happened anyway. Because a held chunk still looks
        allocated, the ring is also scanned on every free, which both keeps a
        double free from corrupting the ring and turns it into a report
        naming both callers.

  0005  Poison the whole payload of a held chunk. A held chunk is in no list,
        so the first bytes of the payload - where a write to the first member
        of a freed object lands - can carry the pattern too.

  0006  Enable both options in build/configs/qemu/build_test, so continuous
        integration actually compiles the feature. It is a build test
        configuration, not a product one.

  0007  apps/examples/mm_uaf_test - trips the detector in each shape it
        should catch and prints what was expected of each, including
        well-behaved traffic which must stay silent.

  0008  apps/examples/mm_quarantine_test - measures the quarantine rather
        than reading its log. Each check prints pass or fail and the exit
        status reflects the result.

Configuration
-------------

  CONFIG_DEBUG_MM_UAF                     the detector (from PR 7475)
  CONFIG_DEBUG_MM_UAF_POISON_SIZE         bytes checked per free chunk (64)
  CONFIG_DEBUG_MM_UAF_PANIC               assert instead of reporting

  CONFIG_DEBUG_MM_QUARANTINE              delay the reuse of freed chunks
  CONFIG_DEBUG_MM_QUARANTINE_CHUNKS       ring size (32)
  CONFIG_DEBUG_MM_QUARANTINE_BYTES        bytes held at once (4096)
  CONFIG_DEBUG_MM_QUARANTINE_MAX_SIZE     largest chunk held (1024)

  CONFIG_EXAMPLES_MM_UAF_TEST             detector test application
  CONFIG_EXAMPLES_MM_QUARANTINE_TEST      quarantine test application

Enable CONFIG_DEBUG_MM_HEAPINFO alongside. Without it the report has no
owning task and no allocating address to print, which is most of its value.

What this does and does not do
------------------------------

It is a detection feature. Holding a chunk back also makes a stale write
harmless while the chunk is held, but that window is small and is flushed
under memory pressure, so it is not protection to rely on or to advertise.

It widens the window in which a stale write is detectable; it does not close
it. A write which happens after the chunk has aged out and been reallocated is
still missed, and the ring depth is a probability knob rather than a
guarantee. Reads through a stale pointer are not detected at all, because a
read leaves nothing behind to check.

Holding chunks back costs contiguous free space: a held chunk keeps its
allocated bit, so it also blocks its two neighbours from merging. Simulation
of a mixed workload at 60 percent heap utilisation showed the largest
contiguous free block falling by about 25 percent on a 64 KB heap, 11 percent
on 128 KB and 3 percent on 512 KB. With CONFIG_MM_ASSERT_ON_FAIL enabled, an
allocation failure panics, so measure this on the real workload before
enabling it on a small-heap product configuration. Test and soak builds are
the intended home.

Status
------

The series applies cleanly in order onto 20fb5f3c and the algorithms have been
exercised on the host, but it has not been compiled for a target: build
qemu/build_test and run both example applications before relying on it.

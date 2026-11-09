# P3 handoff: R7, R4, R5 and quiescence

Implemented against the existing S7 shadow APIs and A8 lifecycle gate.

## Behavior and file ownership

- `kernel/spec_commit.c`: calls `spec_commit_start()` before inspecting shadows.
  Existing A8 policy rejects commit with `SPEC_ERR_BUSY` while another member
  exists or an admitted operation remains. The group stays ACTIVE on BUSY.
  Once frozen, copies P2's entire indexed count/key/data set into allocated
  pages, then publishes that immutable snapshot. Allocation/enumeration errors
  occur before any real cache mutation; every snapshot page is freed.
- `kernel/spec_conflict.c`: queries S7's write count for each snapshot key.
  Any nonzero count returns `SPEC_ERR_CONFLICT`; missing counters fail closed.
- `kernel/log.c`: reserves exclusive journal ownership, checks the entire
  snapshot, then copies all blocks through `log_write()` and synchronously
  completes `commit()` before releasing ownership. Failed checks release the
  reservation without modifying real buffers or the journal. The former
  mid-publication enumeration/partial-unpin failure path has been removed.
- Normal `log_write()` additionally calls S7's write notification. This covers
  the case where an ACTIVE speculative process runs the last `end_op()` for
  a journal containing an ordinary process's writes: S7's physical `bwrite()`
  hook identifies the flushing process and skips its own shadows. Recording
  the ordinary writer at `log_write()` preserves conflict evidence anyway.
  Repeated log entries and subsequent disk installation can increment counters
  multiple times. They are saturating overlap witnesses, not syscall totals.
- S7's existing `bwrite()` hook continues to mark other owners when a
  speculation publishes. No duplicate counter table or baseline is introduced.
- Commit failure discards shadows, buffered output, and speculation pipe state
  before completing the failed lifecycle transition. Successful output release
  remains after durable filesystem publication.

The only shared build integration is two Makefile entries: `spec_conflict.o`
and the `speccommit` user regression program. New private `spec_commit.h`
contains P3's internal snapshot interface; shared declarations and P1/P2/P4
implementation files were not changed. `spec_fs_commit()` retains its existing
public signature but now lives in `spec_commit.c`.

## Race and locking argument

A8 freezes membership and admission under its lifecycle lock. Under the current
sole-member policy no other group member can alter shadows while P2's count/get
API is enumerated, even across allocations and sleeps. No module spinlock is
held while allocating, acquiring a buffer sleep lock, or performing disk I/O.

`reserve_spec_log()` waits for ordinary outstanding operations and any earlier
commit to drain. Only afterward does `spec_conflict_check()` read the counters.
Journal admission stays closed throughout checking, cache publication, and
synchronous disk commit, so another journal writer cannot slip between a check
and publication. Read-only outsiders may still use cache buffers; this does not
add a read-set conflict policy or whole-filesystem read snapshot semantics.

## Capacity

The maximum distinct dirty-block count is

```
min(LOGBLOCKS, superblock.nlog - 1, NBUF - 1)
```

The on-disk header consumes one log block. Dirty buffers stay pinned, so leave
one cache buffer available for journal I/O. With this tree's values, the limit
is 29 total dirty blocks (including inode, bitmap, directory and indirect
metadata), rather than 30 or NSHADOW=64. Oversized sets return
`SPEC_ERR_NO_SPACE` before publication; commits are never split. Each snapshot
block uses one allocated page, avoiding a large kernel-stack array.

## Reproduction and observed results

From the repository root:

```
make -j4 kernel/kernel fs.img
python3 tests/run_spec_commit.py
```

The runner uses separate temporary image copies, runs QEMU with CPUS=1 and
CPUS=3, and preserves raw guest output in `spec_commit_cpus1.log` and
`spec_commit_cpus3.log`. Both runs passed:

- Six P3 cases: private multi-block snapshot and successful/empty commits;
  oversized rejection with original data intact; 29-block success/30-block
  rejection at the cache boundary; ordinary overlapping writer causes conflict
  with all real blocks and outward output preserved/discarded as appropriate;
  another speculation's publication conflicts with an older owner; a live child
  blocks freezing, then its complete shadow set commits after exit.
- All eight existing `spectest_proc` lifecycle cases, including BUSY while
  members are live, reuse, exit cleanup, and failed fork cleanup.
- Existing boot-time output and pipe checks also report PASSED in the logs.

The two outside-writer tests enforce staging within a 20-tick bound before a
scheduled writer starts; a missed bound explicitly fails the test. They are
regressions for the specified ordering, not exhaustive scheduler exploration.
The runner has a 60-second timeout per expected guest result.

Crash injection, GDB evidence, and exhaustive SMP race exploration are outside
this handoff and have not been performed. No crash-recovery test or debugger
session is claimed here.

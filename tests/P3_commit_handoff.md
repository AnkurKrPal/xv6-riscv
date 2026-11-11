# P3 handoff: R4/R5/R7, R9/R10/R11 and quiescence

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

Build integration includes `spec_conflict.o`, the `speccommit` user regression
program, and a separate optional `kernel/kernel-publication-test` target.
Private `spec_commit.h` contains P3's snapshot interface. R10 additionally adds
`log.h` and the four enter/leave hook calls in P4's `file.c`; these hooks are
necessary to cover existing descriptors that bypass `begin_op()`. P1/P2
implementation and shared declarations are unchanged. `spec_fs_commit()` keeps
its existing public signature in `spec_commit.c`.

## Race and locking argument

A8 freezes membership and admission under its lifecycle lock. Under the current
sole-member policy no other group member can alter shadows while P2's count/get
API is enumerated, even across allocations and sleeps. No module spinlock is
held while allocating, acquiring a buffer sleep lock, or performing disk I/O.

`reserve_spec_log()` closes new admission and waits for ordinary outstanding
operations, complete read/stat operations, and any earlier commit to drain. Only afterward does `spec_conflict_check()` read the counters.
Journal admission stays closed throughout checking, cache publication, and
synchronous disk commit, so another journal writer cannot slip between a check
and publication. R10 also excludes inode readers until the synchronous disk
commit has finished; see the reader-gate argument below. This does not add a
read-set conflict policy or snapshots across multiple separate syscalls.

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

## R9: rejection before real changes

The previous preflight remains in force: the coordinator rejects oversized sets
before allocation/publication; the journal validates the full device/key/count
set and checks all overlapping-write counters before changing any real buffer.
There is no partial-copy rollback path. All recoverable filesystem rejection
paths precede the first real cache copy. Capacity, ordinary-writer conflict,
other-speculation conflict, discarded output, and fresh-baseline regressions
are run on both CPU configurations.

## R10: S10 cache visibility is serialized

S10 reads inode size and block addresses from real inode buffers. A journal
reservation alone therefore failed to isolate outside `read()` and `fstat()`
from early cache copies. `log_read_enter()`/`log_read_leave()` now bracket the
whole inode read/stat operation, starting before `ilock()` and ending after
`iunlock()`. One `read()` cannot combine an old inode view with new data blocks.
This also covers reads through already-open file/directory descriptors.

The reader count shares `log.lock` with journal admission. A pending publisher
increments `spec_waiters` before draining current readers and outstanding
journal operations. New readers and new `begin_op()` operations wait, so a
stream of readers or writers cannot indefinitely prevent the drain. Once all
admitted work completes, the publisher retains exclusive journal ownership
through all cache copies and synchronous `commit()` before reopening admission.
Other waiting publishers keep admission closed between publications.

Path traversal, open/create/unlink/link/chdir, inode cleanup and executable
loading already operate inside `begin_op()`/`end_op()` in this tree. The only
read-only user inode entrypoints outside that admission were `fileread()` and
`filestat()`, which receive the minimal hooks. Never nest `begin_op()` inside a
read admission. Acquire admission before inode/buffer locks; never sleep while
holding `log.lock`. Pipe and console reads are excluded from read admission:
waiting for external input must not prevent a commit that releases that input.

This guarantees a complete inode read/stat before or after publication. It
does not turn a sequence of separate syscalls into one filesystem snapshot.

## R11: completion fence and verification

`spec_log_publish()` calls the synchronous `commit()` directly while owning the
journal, rather than assuming an ordinary `end_op()` return guarantees commit.
That function writes journal data, writes the committing header, installs home
blocks, clears the in-memory header, and writes the cleared disk header before
returning. `release_spec_log()` now asserts an empty journal, exclusive owner,
exactly one outstanding reservation and no active readers before reopening
admission. `spec_fs_commit()` returns success only after that fence; the
coordinator flushes buffered output and finishes the syscall afterward.

As in xv6, disk I/O failure panics rather than returning a recoverable commit
error. Existing output-delivery errors can still be reported after filesystem
publication; this work does not add rollback after a durable disk commit.

### Deterministic publication test

From `xv6-riscv`:

```
make -j4 kernel/kernel kernel/kernel-publication-test fs.img
python3 tests/run_spec_publication.py
python3 tests/run_spec_fs.py --quick-usertests
```

The optional test kernel compiles only `log.c` with `SPEC_COMMIT_TEST` and links
it with the normal objects. Production `kernel/kernel` contains no delay/phase
hooks. The separate target is ignored by Git and removed by `make clean`.

The host releases outside read/stat processes through console input after the
first shadow reaches the real cache. Both observers have descriptors opened
before `xbegin()`, so `open()` cannot accidentally hide the bug. The test pauses
again after all cache copies and before `commit()`. Both observers must wait;
the runner rejects read/stat results, outward output, or `xcommit()` success
reported before the durable marker. It checks a five-block replacement, inode
size growth, and a newly created file (directory/allocation metadata). It then
reboots the same disposable disk using the production kernel and verifies all
contents and metadata again. The repository's `fs.img` is never changed by the
runner.

Observed PASS with CPUS=1 and CPUS=3. The filesystem, commit, lifecycle and
ordinary `usertests -q` suites also passed on both CPU configurations; their
combined transcripts are `spec_fs_cpus1.log` and `spec_fs_cpus3.log`. Raw
publication-test evidence is in
`spec_publication_visibility_cpus{1,3}.log` and
`spec_publication_reboot_cpus{1,3}.log`.

Negative control: a temporary copy of `file.c` with only filestat's reader
enter/leave removed was compiled and linked into a temporary test kernel.
The runner rejected it with `STAT_RETURNED escaped before durable commit`.
`spec_publication_negative_control.log` shows this early result between
`CACHE_PARTIAL` and `CACHE_READY`. No production source/object was replaced.
This confirms the targeted regression detects S10's cache-visibility window.

Reboot after completed publication is verified. Crash injection mid-commit,
GDB evidence, and exhaustive SMP race exploration have not been performed.

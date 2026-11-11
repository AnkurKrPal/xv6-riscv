# R12–R14: commit races, crash recovery, and actual GDB evidence

## R12 implementation

The frozen snapshot is checked only after exclusive journal reservation and
before any real cache copy. The reservation drains earlier journal operations
and inode readers, blocks new admissions, and stays owned through disk commit.

The journal now records the exact `spec_owner`. `spec_conflict_check()` rejects
calls without that caller's exclusive reservation with `SPEC_ERR_BUSY`. This
makes the check-to-publish requirement executable rather than relying only on
call-site convention. Release clears ownership and asserts the journal is
empty and no outside readers are admitted.

`spec_commit_transaction()` has one P4 flush call, reached only after
`spec_fs_commit()` returns `SPEC_OK` from synchronous publication. Capacity,
allocation, enumeration and conflict failures discard output. The success
notification occurs after the journal data/header/home writes and cleared
header complete. No extra P4 hooks or autonomous flush paths were added.

The deterministic race test parks the committer immediately after the conflict
check, starts an ordinary writer using an already-open descriptor, and waits
for that writer to sleep in journal admission. GDB observes:

```
owner=1 outstanding=1 journal=0 readers=0
```

The host then resumes publication. The buffered output and commit return become
visible, the normal writer completes afterward, and the file contains its later
write. Both CPUS=1 and CPUS=3 passed. Evidence: `race/race_cpus{1,3}.log` and
`race/race_cpus{1,3}_reservation_gdb.log`.

## R13 regression source and executable

The owned source is now `user/spectest_commit.c`. Makefile explicitly links it
as `user/_speccommit`: `spectest_commit` is longer than xv6's 14-byte directory
name limit. Existing runners and guest commands therefore remain `speccommit`.

The normal suite checks:

1. Multi-block private reads, publication, empty commits and lifecycle cleanup.
2. Oversized rejection without changing original blocks or releasing queued
   outward pipe data; the ordinary supervisor must receive EOF instead.
3. The exact cache boundary: 29 total dirty blocks succeed; 30 reject before
   journal/cache mutation, retaining a scratch buffer for log I/O.
4. An ordinary overlapping write rejects the entire snapshot and outward data;
   a fresh speculation does not inherit stale counters.
5. Another speculation's publication conflicts with an older baseline.
6. Live children prevent freezing; their complete shadow set commits after exit.
7. File replacement/growth, creation, hard links, directory creation, and unlink
   either abort together or commit together. Contents, inode size, link counts,
   shared inode identity, directory type and deleted-file absence are checked.

`visibility` mode additionally tests pre-opened outside read/stat descriptors
while cache publication is deliberately paused. `crash`, `recover-old`, and
`recover-new` modes verify complete filesystem state across interrupted commits.
`race` and `capacity-gdb` support the deterministic host/debugger regressions.

## R14 crash experiment

GNU GDB 16.3 was built locally for `riscv64-unknown-elf`, using the system zlib
and Homebrew GMP/MPFR. It runs from `/tmp/os-project-gdb-build/gdb/gdb`; no GDB
installation or machine-wide configuration was changed. Downloaded source:
`https://ftp.gnu.org/gnu/gdb/gdb-16.3.tar.xz`, SHA-256:

```
bcfcd095528a987917acf9fff3f1672181694926cc18d609c99d0042c00224c5
```

`kernel/kernel-crash-test` links test variants of only `log.c` and
`spec_commit.c`. `SPEC_CRASH_TEST` adds nine checkpoints. GDB selects a checkpoint
before boot; after completing the relevant I/O, the journal owner parks there.
The host waits for the exact stop marker and kills QEMU with SIGKILL, without a
guest shutdown. Each case uses a fresh temporary copy of `fs.img` and restarts
that same image with the ordinary production kernel.

The workload replaces a three-block file with five new blocks, creates another
file, adds a hard link, creates a directory, and removes a baseline file. This
stages 12 blocks including inode/directory/bitmap metadata. Before crash, queued
console output and commit success must remain invisible. Recovery checks all
contents and metadata together, then creates/writes/unlinks a probe file to
exercise the recovered journal and allocator. Host-side checks independently
read the durable log-header count and require recovery to clear it.

| Phase | Stop boundary | Header before restart | Expected complete state |
| --- | --- | --- | --- |
| 1 | Exclusive reservation obtained | 0 | Old |
| 2 | Conflict check passed | 0 | Old |
| 3 | First real cache copy completed | 0 | Old |
| 4 | First journal payload block written | 0 | Old |
| 5 | All journal payload blocks written | 0 | Old |
| 6 | Committing journal header written | 12 | New, replay required |
| 7 | First home block installed | 12 | New, replay required |
| 8 | All home blocks installed and header cleared | 0 | New |
| 9 | Filesystem commit returned, before P4 flush | 0 | New |

**Observed: all 18 cases passed (nine phases × CPUS=1/3).** Recovery replayed
exactly for phases 6 and 7; all recovered headers were zero. Raw guest and GDB
setup transcripts are under `tests/crash/`; `crash/summary.log` records the full
matrix. This models process failure at completed block-I/O boundaries. It does
not claim torn-sector or host power-loss coverage.

## Actual historical GDB failure and fix

No defect was injected into the working implementation. The failing kernel is
an untouched archive/build of real historical revision
`c6d4dd935d2360e43fd12d5fac647a8bf06b8083`. The same current guest capacity workload
runs against that kernel and then against the current kernel on disposable
copies of the current disk image. This captures an existing capacity fix from
`a56eded10a6f95b8aae2ccce61ad67622c15e893`; it is not presented as a newly discovered
bug or a fix newly introduced for R14.

The historical kernel accepted 30 dirty blocks with NBUF=30. GDB stopped in:

```
panic (s="bget: no buffers")
#1 bget(dev=1, blockno=3)
#4 write_log()
#5 commit()
#6 spec_fs_commit(spec=2)
```

Actual inspected values: `log.lh.n=30`, `log.outstanding=1`,
`log.spec_committing=1`, cache size 30, and **all 30 cache entries referenced**.
`write_log()` could not acquire its journal scratch buffer because the entire
cache was pinned. This is a real kernel panic, not a synthetic assertion.

For the fixed kernel, GDB broke at the coordinator's capacity guard with
`count=30`, `log.lh.n=0`, and `log.spec_committing=0`. `finish` returned `-6`
(`SPEC_ERR_NO_SPACE`) before real publication. The guest then reported
`gdb: CAPACITY FIX VERIFIED`; the adjacent 29-block case had succeeded.

Raw evidence:

- `gdb/gdb_before.log`: commands, panic argument, actual backtrace, journal state,
  cache size and referenced-buffer count.
- `gdb/gdb_after.log`: commands, preflight values, stack and rejection return.
- `gdb/gdb_{before,after}.commands`: exact debugger commands.
- `gdb/gdb_{before,after}_guest.log`: guest console transcripts.

The required debugger failure/fix evidence has therefore been captured using
GNU GDB itself, not inferred from serial output.

## Reproduce

From `xv6-riscv`, with a RISC-V-capable GNU GDB available:

```
make -j4 kernel/kernel kernel/kernel-crash-test kernel/kernel-publication-test fs.img
python3 tests/run_spec_commit.py
python3 tests/run_spec_publication.py
python3 tests/run_spec_fs.py --quick-usertests
python3 tests/run_spec_r14.py --gdb /path/to/riscv-capable-gdb
```

The R14 runner builds the untouched historical revision from local Git history
unless `--legacy-kernel` supplies an already-built copy. It runs the crash
matrix, deterministic race and GDB before/after sessions. `--suites`, `--cpus`
and `--phases` allow targeted reruns. QEMU's localhost debugger connection may
need sandbox permission. Production builds contain no crash checkpoint symbols;
the test kernels are ignored by Git and removed by `make clean`.

The normal filesystem, commit, process lifecycle and ordinary `usertests -q`
regressions passed on both CPU configurations. Exhaustive scheduler exploration
and torn-sector/power-loss testing remain outside the demonstrated guarantees.

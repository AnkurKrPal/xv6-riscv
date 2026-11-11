#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "spec_commit.h"
#include "log.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGBLOCKS];
};

struct log {
  struct spinlock lock;
  int start;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  int ncommit;
  int spec_committing;
  int spec_capacity; // On-disk payload and buffer-cache bound.
  int readers;       // Whole inode read/stat operations outside begin_op().
  int spec_waiters;  // Close admission while publishers drain existing work.
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

#ifdef SPEC_COMMIT_TEST
// Deterministic visibility regression only. This is excluded from the normal
// kernel; the separate test target pauses with real buffers partially copied
// and again with all copies made but no durable commit yet.
static void
publication_test_pause(char *phase)
{
  uint start;
  printk("publication-test: %s\n", phase);
  acquire(&tickslock);
  start = ticks;
  while ((uint)(ticks - start) < 20) {
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
}
#endif

static int
reserve_spec_log(int blocks)
{
  if (blocks < 0 || blocks > log.spec_capacity)
    return SPEC_ERR_NO_SPACE;

  acquire(&log.lock);
  log.spec_waiters++;
  while (log.committing || log.spec_committing || log.outstanding != 0 ||
         log.lh.n != 0 || log.readers != 0) {
    sleep_prepare(&log);
    release(&log.lock);
    sleep();
    acquire(&log.lock);
  }
  log.spec_waiters--;
  log.spec_committing = 1;
  log.committing = 1;
  log.outstanding = 1;
  release(&log.lock);
  return SPEC_OK;
}

static void
release_spec_log(void)
{
  acquire(&log.lock);
  // A rejected snapshot never entered the journal; a published snapshot has
  // synchronously completed write_log, write_head, install_trans and clearing
  // the header. Readers must not be admitted until either case is complete.
  if (log.lh.n != 0 || !log.spec_committing || !log.committing ||
      log.outstanding != 1 || log.readers != 0)
    panic("release_spec_log: incomplete");
  log.outstanding = 0;
  log.committing = 0;
  log.spec_committing = 0;
  log.ncommit++;
  wakeup(&log);
  release(&log.lock);
}

// Register before acquiring any inode/buffer lock. Keeping admission for a
// whole read prevents an old inode size/address set from being combined with
// newly published data. Existing readers drain; pending publishers block new
// readers and begin_op() callers, so neither stream can starve publication.
void
log_read_enter(void)
{
#ifdef SPEC_COMMIT_TEST
  int reported = 0;
#endif
  acquire(&log.lock);
  while (log.spec_committing || log.spec_waiters != 0) {
#ifdef SPEC_COMMIT_TEST
    if (!reported) {
      printk("publication-test: READER_WAIT\n");
      reported = 1;
    }
#endif
    sleep_prepare(&log);
    release(&log.lock);
    sleep();
    acquire(&log.lock);
  }
  log.readers++;
  release(&log.lock);
}

void
log_read_leave(void)
{
  acquire(&log.lock);
  if (log.readers < 1)
    panic("log_read_leave");
  log.readers--;
  if (log.readers == 0)
    wakeup(&log);
  release(&log.lock);
}

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.dev = dev;
  log.spec_capacity = LOGBLOCKS;
  if (sb->nlog < 1)
    panic("initlog: no header");
  if (sb->nlog - 1 < (uint)log.spec_capacity)
    log.spec_capacity = sb->nlog - 1;
  if (log.spec_capacity >= NBUF)
    log.spec_capacity = NBUF - 1;
  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    if (recovering) {
      printk("recovering tail %d dst %d\n", tail, log.lh.block[tail]);
    }
    struct buf *lbuf = bread(log.dev, log.start + tail + 1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]);   // read dst
    memmove(dbuf->data, lbuf->data, BSIZE); // copy block to dst
    bwrite(dbuf);                           // write dst to disk
    if (recovering == 0)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *)(buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *)(buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head();
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);
  while (1) {
    if (log.committing || log.spec_committing || log.spec_waiters != 0) {
      sleep_prepare(&log);
      release(&log.lock);
      sleep();
      acquire(&log.lock);
    } else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGBLOCKS) {
      // this op might exhaust log space; wait for commit.
      sleep_prepare(&log);
      release(&log.lock);
      sleep();
      acquire(&log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if (log.committing)
    panic("log.committing");
  if (log.outstanding == 0) {
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if (do_commit) {
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    log.ncommit += 1;
    wakeup(&log);
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start + tail + 1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to); // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();      // Write modified blocks from cache to log
    write_head();     // Write header to disk -- the real commit
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;
    write_head(); // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;
  spec_id_t spec;
  enum spec_state state;

  acquire(&log.lock);
  spec = spec_current_id();
  if (!log.spec_committing && spec != SPEC_ID_NONE &&
      spec_get_state(spec, &state) == SPEC_OK &&
      state == SPEC_STATE_ACTIVE) {
    release(&log.lock);
    if (spec_shadow_store(spec, b->dev, b->blockno,
                          (const char *)b->data) != SPEC_OK)
      panic("log_write: speculative journal full");
    return;
  }
  if (log.lh.n >= LOGBLOCKS)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  // Record ordinary cache publications as well as S7's physical bwrite()
  // notifications. The last end_op() can belong to an ACTIVE speculation
  // even when another process supplied the ordinary journal writes; relying
  // solely on that flusher's ID would miss conflicts against its own shadows.
  // Counts are saturating conflict witnesses, not exact syscall totals.
  if (!log.spec_committing)
    spec_shadow_note_normal_write(b->dev, b->blockno);

  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno) // log absorption
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) { // Add new block to log?
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}

/*
 * The coordinator supplies a complete frozen snapshot. Reserve exclusive
 * journal ownership before consulting S7's counters: earlier ordinary writes
 * (and other speculative commits) must have reached bwrite() by this point.
 * Keep ownership through disk commit so no new writer can invalidate checks.
 */
int
spec_log_publish(spec_id_t spec, const struct spec_commit_block *blocks,
                 int count)
{
  const struct spec_commit_block *b, *previous;
  enum spec_state state;
  int result, actual = 0;

  if (spec_current_id() != spec || spec == SPEC_ID_NONE ||
      spec_get_state(spec, &state) != SPEC_OK ||
      state != SPEC_STATE_COMMITTING)
    return SPEC_ERR_BAD_ID;

  // Reject unsupported devices and malformed snapshots before any mutation.
  // The physical redo log stores block numbers for its one device only.
  for (b = blocks; b != 0; b = b->next) {
    if (++actual > log.spec_capacity)
      return SPEC_ERR_NO_SPACE;
    if (b->dev != (uint)log.dev)
      return SPEC_ERR_COMMIT;
    for (previous = blocks; previous != b; previous = previous->next) {
      if (previous->blockno == b->blockno)
        return SPEC_ERR_COMMIT;
    }
  }
  if (actual != count)
    return SPEC_ERR_COMMIT;

  result = reserve_spec_log(count);
  if (result != SPEC_OK)
    return result;

  result = spec_conflict_check(spec, blocks);
  if (result == SPEC_OK) {
    // No recoverable failures remain once the first real buffer changes.
    // S7's bwrite hook skips this owner but marks other overlapping owners.
    for (b = blocks; b != 0; b = b->next) {
      struct buf *bp = bread_original(b->dev, b->blockno);
      memmove(bp->data, b->data, BSIZE);
      log_write(bp);
      brelse(bp);
#ifdef SPEC_COMMIT_TEST
      if (b == blocks)
        publication_test_pause("CACHE_PARTIAL");
#endif
    }
#ifdef SPEC_COMMIT_TEST
    if (blocks != 0)
      publication_test_pause("CACHE_READY");
#endif
    commit(); // Synchronous: durable before the coordinator releases output.
#ifdef SPEC_COMMIT_TEST
    if (blocks != 0)
      printk("publication-test: DURABLE\n");
#endif
  }
  release_spec_log();
  return result;
}

uint64
sys_sync(void)
{
  acquire(&log.lock);
  if (log.committing || log.outstanding > 0) {
    int n = log.ncommit + 1;
    while (log.ncommit < n) {
      sleep_prepare(&log);
      release(&log.lock);
      sleep();
      acquire(&log.lock);
    }
  }
  release(&log.lock);
  return 0;
}

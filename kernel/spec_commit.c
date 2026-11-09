#include "types.h"
#include "spec.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "spec_commit.h"

static void
snapshot_free(struct spec_commit_block *blocks)
{
  while (blocks != 0) {
    struct spec_commit_block *next = blocks->next;
    kfree(blocks);
    blocks = next;
  }
}

// A8's spec_commit_start() must first close admission and verify that the
// only remaining member has no active operations. Thus P2's indexed count/get
// API is stable even while we allocate pages or wait for journal ownership.
int
spec_fs_commit(spec_id_t spec)
{
  struct spec_commit_block *blocks = 0, **tail = &blocks, *b;
  enum spec_state state;
  int count, result = SPEC_OK;

  if (spec == SPEC_ID_NONE || spec_current_id() != spec ||
      spec_get_state(spec, &state) != SPEC_OK ||
      state != SPEC_STATE_COMMITTING)
    return SPEC_ERR_BAD_ID;

  count = spec_shadow_count(spec);
  // Leave a cache buffer available for journal I/O while all dirty blocks
  // are pinned. The journal also checks the actual on-disk log capacity.
  if (count < 0 || count > LOGBLOCKS || count >= NBUF)
    return SPEC_ERR_NO_SPACE;

  for (int i = 0; i < count; i++) {
    b = (struct spec_commit_block *)kalloc();
    if (b == 0) {
      result = SPEC_ERR_NO_SPACE;
      goto out;
    }
    b->next = 0;
    *tail = b;
    tail = &b->next;
    result = spec_shadow_get(spec, i, &b->dev, &b->blockno, b->data);
    if (result != SPEC_OK)
      goto out;
  }

  // All fallible enumeration and allocation finishes before publication.
  result = spec_log_publish(spec, blocks, count);
out:
  snapshot_free(blocks);
  return result;
}

/*
 * Coordinate lifecycle transition, journal publication, and external output.
 * Output is released only after the filesystem log has completed.
 */
int
spec_commit_transaction(void)
{
  struct proc *p = myproc();
  spec_id_t spec;
  int result;

  if (p == 0)
    return SPEC_ERR_NO_ACTIVE;
  spec = p->spec;
  if (spec == SPEC_ID_NONE)
    return SPEC_ERR_NO_ACTIVE;

  // Freeze before reading either the shadow count or its contents. BUSY
  // preserves ACTIVE so live children/admitted operations can finish.
  result = spec_commit_start(spec);
  if (result != SPEC_OK)
    return result;

  result = spec_fs_commit(spec);
  if (result != SPEC_OK) {
    spec_shadow_discard(spec);
    spec_output_discard(spec);
    pipe_spec_discard(spec);
    spec_commit_finish(spec, 0);
    return result;
  }

  spec_shadow_discard(spec);
  result = spec_output_flush(spec);
  if (result != SPEC_OK) {
    spec_output_discard(spec);
    pipe_spec_discard(spec);
    spec_commit_finish(spec, 1);
    return result;
  }
  pipe_spec_commit(spec);
  return spec_commit_finish(spec, 1);
}

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// There cannot be more live speculation groups than processes.
#define NSPEC NPROC
#define SPEC_ID_MAX 0x7fffffff

struct spec_group {
  spec_id_t id;
  enum spec_state state;
  int members;
  int active_ops; // Admitted operations that must finish before freezing.
};

static struct {
  struct spinlock lock;
  spec_id_t next_id;
  struct spec_group groups[NSPEC];
} spectable;

// Caller must hold spectable.lock.
static struct spec_group *
find_group(spec_id_t id)
{
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return 0;

  for (g = spectable.groups; g < &spectable.groups[NSPEC]; g++) {
    if (g->id == id && g->state != SPEC_STATE_FREE)
      return g;
  }
  return 0;
}

// Caller must hold spectable.lock and must have found a free group slot.
static spec_id_t
alloc_id(void)
{
  spec_id_t candidate;
  int attempts;

  candidate = spectable.next_id;
  for (attempts = 0; attempts <= NSPEC; attempts++) {
    if (candidate == SPEC_ID_NONE)
      candidate = 1;
    if (find_group(candidate) == 0) {
      if (candidate == SPEC_ID_MAX)
        spectable.next_id = 1;
      else
        spectable.next_id = candidate + 1;
      return candidate;
    }
    if (candidate == SPEC_ID_MAX)
      candidate = 1;
    else
      candidate++;
  }
  return SPEC_ID_NONE;
}

void
specinit(void)
{
  struct spec_group *g;

  initlock(&spectable.lock, "spectable");
  spectable.next_id = 1;
  for (g = spectable.groups; g < &spectable.groups[NSPEC]; g++) {
    g->id = SPEC_ID_NONE;
    g->state = SPEC_STATE_FREE;
    g->members = 0;
    g->active_ops = 0;
  }
}

spec_id_t
spec_current_id(void)
{
  struct proc *p = myproc();
  spec_id_t id;

  if (p == 0)
    return SPEC_ID_NONE;

  acquire(&spectable.lock);
  id = p->spec;
  release(&spectable.lock);
  return id;
}

int
spec_get_state(spec_id_t id, enum spec_state *state)
{
  struct spec_group *g;

  if (state == 0 || id == SPEC_ID_NONE)
    return SPEC_ERR_BAD_ID;

  acquire(&spectable.lock);
  g = find_group(id);
  if (g == 0) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  *state = g->state;
  release(&spectable.lock);
  return SPEC_OK;
}

int
spec_begin(void)
{
  struct proc *p = myproc();
  struct spec_group *free_group;
  struct spec_group *g;
  spec_id_t id;

  if (p == 0)
    return SPEC_ERR_NO_ACTIVE;

  acquire(&spectable.lock);
  if (p->spec != SPEC_ID_NONE) {
    release(&spectable.lock);
    return SPEC_ERR_NESTED;
  }

  free_group = 0;
  for (g = spectable.groups; g < &spectable.groups[NSPEC]; g++) {
    if (g->state == SPEC_STATE_FREE) {
      free_group = g;
      break;
    }
  }
  if (free_group == 0) {
    release(&spectable.lock);
    return SPEC_ERR_NO_SLOT;
  }

  id = alloc_id();
  if (id == SPEC_ID_NONE) {
    release(&spectable.lock);
    return SPEC_ERR_NO_SLOT;
  }

  free_group->id = id;
  free_group->state = SPEC_STATE_ACTIVE;
  free_group->members = 1;
  free_group->active_ops = 0;
  p->spec = id;
  release(&spectable.lock);
  return SPEC_OK;
}

// Admit work and freeze under the same lock: once COMMITTING is visible,
// no new operation or fork can enter the group.  Callers must leave on every
// success/failure path, after releasing any process locks.
int
spec_op_enter(spec_id_t id)
{
  struct proc *p = myproc();
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return SPEC_OK;
  if (p == 0)
    return SPEC_ERR_NO_ACTIVE;

  acquire(&spectable.lock);
  g = find_group(id);
  if (p->spec != id || g == 0) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  if (g->state != SPEC_STATE_ACTIVE) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }
  if (g->active_ops >= NPROC) {
    release(&spectable.lock);
    return SPEC_ERR_NO_SLOT;
  }
  g->active_ops++;
  release(&spectable.lock);
  return SPEC_OK;
}

void
spec_op_leave(spec_id_t id)
{
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return;

  acquire(&spectable.lock);
  g = find_group(id);
  if (g == 0 || g->active_ops == 0)
    panic("spec_op_leave");
  g->active_ops--;
  release(&spectable.lock);
  // Wake outside the lifecycle lock: wakeup() takes process locks.
  wakeup(&spectable);
}

// Reserve exit cleanup before it can change files, cwd, or membership.
// Register the wait before checking the state, so commit completion cannot
// be missed.  Preparing/cancelling the wait outside spectable.lock avoids
// reversing the process-lock order used by failed-fork cleanup.
spec_id_t
spec_exit_enter(void)
{
  struct proc *p = myproc();
  struct spec_group *g;
  spec_id_t id;

  for (;;) {
    sleep_prepare(&spectable);
    acquire(&spectable.lock);
    id = p->spec;
    g = find_group(id);
    if (id != SPEC_ID_NONE && g == 0)
      panic("spec_exit_enter: group");
    if (id == SPEC_ID_NONE ||
        (g->state == SPEC_STATE_ACTIVE && g->active_ops < NPROC)) {
      if (id != SPEC_ID_NONE)
        g->active_ops++;
      release(&spectable.lock);
      acquire(&p->lock);
      p->chan = 0;
      release(&p->lock);
      return id;
    }
    release(&spectable.lock);
    sleep();
    // A wakeup may be spurious, or commit may have cleared p->spec.
  }
}

void
spec_exit_leave(spec_id_t id)
{
  struct proc *p = myproc();
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return;

  acquire(&spectable.lock);
  g = find_group(id);
  if (p->spec != id || g == 0 || g->state != SPEC_STATE_ACTIVE ||
      g->active_ops == 0)
    panic("spec_exit_leave");
  // Clear membership and release the operation atomically.  Keep the member
  // reference until spec_leave(): another member still sees the group busy
  // in this interval, and this process can no longer start a commit.
  p->spec = SPEC_ID_NONE;
  g->active_ops--;
  release(&spectable.lock);
  spec_leave(id);
  wakeup(&spectable);
}

// Freeze only a quiescent group.  Live children (including children still
// being forked) keep commit busy until they exit; admitted work must drain
// before ACTIVE can change.  Returning BUSY leaves the group ACTIVE so its
// members can finish, rather than sleeping on work that needs the caller.
int
spec_commit_start(spec_id_t id)
{
  struct proc *p = myproc();
  struct spec_group *g;

  if (p == 0 || id == SPEC_ID_NONE)
    return SPEC_ERR_NO_ACTIVE;

  acquire(&spectable.lock);
  if (p->spec != id) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  g = find_group(id);
  if (g == 0) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  if (g->state != SPEC_STATE_ACTIVE || g->members != 1 ||
      g->active_ops != 0) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }

  g->state = SPEC_STATE_COMMITTING;
  release(&spectable.lock);
  return SPEC_OK;
}

int
spec_commit_finish(spec_id_t id, int committed)
{
  struct proc *p = myproc();
  struct spec_group *g;

  if (p == 0 || id == SPEC_ID_NONE)
    return SPEC_ERR_NO_ACTIVE;

  acquire(&spectable.lock);
  if (p->spec != id) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  g = find_group(id);
  if (g == 0 || g->state != SPEC_STATE_COMMITTING ||
      g->members != 1 || g->active_ops != 0) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }

  // P3 calls this after publication succeeds or failed-commit cleanup ends.
  // Only successful publication may record COMMITTED; rejection is ABORTED.
  g->state = committed ? SPEC_STATE_COMMITTED : SPEC_STATE_ABORTED;
  p->spec = SPEC_ID_NONE;
  g->members = 0;
  g->id = SPEC_ID_NONE;
  g->state = SPEC_STATE_FREE;
  release(&spectable.lock);
  wakeup(&spectable);
  return SPEC_OK;
}

int
spec_commit(void)
{
  return spec_commit_transaction();
}

int
spec_abort(void)
{
  struct proc *p = myproc();
  struct spec_group *g;
  spec_id_t id;

  if (p == 0)
    return SPEC_ERR_NO_ACTIVE;

  acquire(&spectable.lock);
  id = p->spec;
  if (id == SPEC_ID_NONE) {
    release(&spectable.lock);
    return SPEC_ERR_NO_ACTIVE;
  }
  g = find_group(id);
  if (g == 0 || g->state != SPEC_STATE_ACTIVE || g->members != 1 ||
      g->active_ops != 0) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }
  g->state = SPEC_STATE_ABORTING;
  release(&spectable.lock);

  spec_shadow_discard(id);
  spec_output_discard(id);
  pipe_spec_discard(id); // added by Pal B24CS1078 (L10)

  acquire(&spectable.lock);
  p->spec = SPEC_ID_NONE;
  g->members = 0;
  g->id = SPEC_ID_NONE;
  g->state = SPEC_STATE_ABORTED;
  g->state = SPEC_STATE_FREE;
  release(&spectable.lock);
  wakeup(&spectable);
  return SPEC_OK;
}

int
spec_join(spec_id_t id)
{
  struct proc *p = myproc();
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return SPEC_ERR_BAD_ID;

  acquire(&spectable.lock);
  g = find_group(id);
  // Validate the parent and group together; a stale inherited ID must not
  // attach a child after the parent has detached or commit has finished.
  if (g == 0 || p == 0 || p->spec != id) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  if (g->state != SPEC_STATE_ACTIVE) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }
  if (g->members >= NPROC) {
    release(&spectable.lock);
    return SPEC_ERR_NO_SLOT;
  }
  g->members++;
  release(&spectable.lock);
  return SPEC_OK;
}

void
spec_leave(spec_id_t id)
{
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return;

  acquire(&spectable.lock);
  g = find_group(id);
  if (g == 0 || g->members == 0) {
    release(&spectable.lock);
    return;
  }
  if (g->state != SPEC_STATE_ACTIVE)
    panic("spec_leave: frozen");
  // A member must release its operation reservations before detaching.
  if (g->members == 1 && g->active_ops != 0)
    panic("spec_leave: active ops");
  g->members--;
  if (g->members != 0) {
    release(&spectable.lock);
    return;
  }

  // The last member implicitly aborts.  Keep the slot and ID reserved
  // until all shared resources are discarded, and reject new joins.
  g->state = SPEC_STATE_ABORTING;
  release(&spectable.lock);

  // Cleanup takes module locks; never hold spectable.lock across it.
  spec_shadow_discard(id);
  spec_output_discard(id);
  pipe_spec_discard(id); // added by Pal B24CS1078 (L10)

  acquire(&spectable.lock);
  g->state = SPEC_STATE_ABORTED;
  g->id = SPEC_ID_NONE;
  g->state = SPEC_STATE_FREE;
  release(&spectable.lock);
}

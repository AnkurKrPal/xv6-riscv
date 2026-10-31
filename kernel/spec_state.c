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
  p->spec = id;
  release(&spectable.lock);
  return SPEC_OK;
}

// Begin the commit transition only when this process is the sole member.
// This prevents a child from racing with the coordinator while the group is
// being frozen.
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
  if (g->state != SPEC_STATE_ACTIVE || g->members != 1) {
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
  if (g == 0 || g->state != SPEC_STATE_COMMITTING) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }

  g->state = committed ? SPEC_STATE_COMMITTED : SPEC_STATE_ABORTING;
  p->spec = SPEC_ID_NONE;
  g->members = 0;
  g->id = SPEC_ID_NONE;
  g->state = committed ? SPEC_STATE_FREE : SPEC_STATE_ABORTED;
  g->state = SPEC_STATE_FREE;
  release(&spectable.lock);
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
  if (g == 0 || g->state != SPEC_STATE_ACTIVE || g->members != 1) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }
  g->state = SPEC_STATE_ABORTING;
  release(&spectable.lock);

  spec_shadow_discard(id);
  spec_output_discard(id);
  pipe_spec_discard(id);

  acquire(&spectable.lock);
  p->spec = SPEC_ID_NONE;
  g->members = 0;
  g->id = SPEC_ID_NONE;
  g->state = SPEC_STATE_ABORTED;
  g->state = SPEC_STATE_FREE;
  release(&spectable.lock);
  return SPEC_OK;
}

int
spec_join(spec_id_t id)
{
  struct spec_group *g;

  if (id == SPEC_ID_NONE)
    return SPEC_ERR_BAD_ID;

  acquire(&spectable.lock);
  g = find_group(id);
  if (g == 0) {
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
  if (g != 0 && g->members > 0) {
    g->members--;
    if (g->members == 0) {
      g->id = SPEC_ID_NONE;
      g->state = SPEC_STATE_FREE;
      g->members = 0;
    }
  }
  release(&spectable.lock);
}

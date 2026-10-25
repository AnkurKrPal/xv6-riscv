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

// Finish a speculation that currently has only the calling process as a
// member.  Multi-process finalization needs the later freeze/quiescence work;
// reject it for now instead of allowing a child to race with finalization.
//
// At this stage no shadow or output modules are connected, so completing an
// empty speculation requires only the lifecycle state transition.  The commit
// and abort hooks will be inserted between the transitional and terminal
// states when those modules are integrated.
static int
finish_current(enum spec_state transition, enum spec_state terminal)
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
  if (g == 0) {
    release(&spectable.lock);
    return SPEC_ERR_BAD_ID;
  }
  if (g->state != SPEC_STATE_ACTIVE || g->members != 1) {
    release(&spectable.lock);
    return SPEC_ERR_BUSY;
  }

  g->state = transition;
  g->state = terminal;
  p->spec = SPEC_ID_NONE;
  g->members = 0;
  g->id = SPEC_ID_NONE;
  g->state = SPEC_STATE_FREE;
  release(&spectable.lock);
  return SPEC_OK;
}

int
spec_commit(void)
{
  return finish_current(SPEC_STATE_COMMITTING, SPEC_STATE_COMMITTED);
}

int
spec_abort(void)
{
  return finish_current(SPEC_STATE_ABORTING, SPEC_STATE_ABORTED);
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

// Private block copies used by speculative filesystem operations.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NSHADOW 64

struct shadow {
  spec_id_t spec;
  struct buf buf;
};

static struct {
  struct spinlock lock;
  struct shadow entries[NSHADOW];
} shadow_table;

// Find an entry for an exact speculation, device, and block key.
// Caller must hold shadow_table.lock.
//added/modified by Saharsh B24CS1105
static struct shadow *
shadow_find_locked(spec_id_t spec, uint dev, uint blockno)
{
  for (int i = 0; i < NSHADOW; i++) {
    struct shadow *entry = &shadow_table.entries[i];
    if (entry->spec == spec && entry->buf.dev == dev &&
        entry->buf.blockno == blockno)
      return entry;
  }
  return 0;
}

// Reserve a free entry and initialize its buffer identity.
// Caller must hold shadow_table.lock.
//added/modified by Saharsh B24CS1105
static struct shadow *
shadow_alloc_locked(uint dev, uint blockno)
{
  for (int i = 0; i < NSHADOW; i++) {
    struct shadow *entry = &shadow_table.entries[i];
    if (entry->spec == SPEC_ID_NONE && entry->buf.refcnt == 0) {
      entry->buf.valid = 1;
      entry->buf.disk = 0;
      entry->buf.dev = dev;
      entry->buf.blockno = blockno;
      entry->buf.refcnt = 1;
      return entry;
    }
  }
  return 0;
}

// Map an embedded shadow buffer back to its table entry.
// Caller must hold shadow_table.lock.
//added/modified by Saharsh B24CS1105
static struct shadow *
shadow_from_buf_locked(struct buf *b)
{
  for (int i = 0; i < NSHADOW; i++) {
    if (&shadow_table.entries[i].buf == b)
      return &shadow_table.entries[i];
  }
  return 0;
}

// Initialize the bounded shadow table and each entry's sleep lock.
//added/modified by Saharsh B24CS1105
void
spec_shadow_init(void)
{
  initlock(&shadow_table.lock, "shadow_table");
  for (int i = 0; i < NSHADOW; i++) {
    shadow_table.entries[i].spec = SPEC_ID_NONE;
    shadow_table.entries[i].buf.refcnt = 0;
    initsleeplock(&shadow_table.entries[i].buf.lock, "shadow");
  }
}

// Return the matching shadow buffer locked and referenced, or zero on a miss.
//added/modified by Saharsh B24CS1105
struct buf *
spec_shadow_acquire(spec_id_t spec, uint dev, uint blockno)
{
  struct shadow *entry;

  acquire(&shadow_table.lock);
  entry = shadow_find_locked(spec, dev, blockno);
  if (entry != 0)
    entry->buf.refcnt++;
  release(&shadow_table.lock);

  if (entry == 0)
    return 0;
  acquiresleep(&entry->buf.lock);
  return &entry->buf;
}

// Unlock and drop a shadow-buffer reference; return zero for normal buffers.
//added/modified by Saharsh B24CS1105
int
spec_shadow_release(struct buf *b)
{
  struct shadow *entry;

  acquire(&shadow_table.lock);
  entry = shadow_from_buf_locked(b);
  release(&shadow_table.lock);
  if (entry == 0)
    return 0;

  if (!holdingsleep(&b->lock))
    panic("shadow_release");
  releasesleep(&b->lock);

  acquire(&shadow_table.lock);
  if (b->refcnt < 1)
    panic("shadow refcnt");
  b->refcnt--;
  release(&shadow_table.lock);
  return 1;
}

// Report whether a buffer belongs to the shadow table.
//added/modified by Saharsh B24CS1105
int
spec_shadow_is_buffer(struct buf *b)
{
  int found;

  acquire(&shadow_table.lock);
  found = shadow_from_buf_locked(b) != 0;
  release(&shadow_table.lock);
  return found;
}

// Replace a locked real buffer with a locked private copy for an active spec.
// On failure, return zero and leave the original buffer locked and unchanged.
//added/modified by Saharsh B24CS1105
struct buf *
spec_shadow_prepare(struct buf *b)
{
  struct shadow *entry;
  spec_id_t spec;
  enum spec_state state;

  if (!holdingsleep(&b->lock))
    panic("shadow_prepare");

  spec = spec_current_id();
  if (spec == SPEC_ID_NONE)
    return b;
  if (spec_get_state(spec, &state) != SPEC_OK || state != SPEC_STATE_ACTIVE)
    return b;

  acquire(&shadow_table.lock);
  entry = shadow_from_buf_locked(b);
  if (entry != 0) {
    if (entry->spec != spec)
      panic("shadow owner");
    release(&shadow_table.lock);
    return b;
  }

  entry = shadow_find_locked(spec, b->dev, b->blockno);
  if (entry != 0) {
    entry->buf.refcnt++;
  } else {
    entry = shadow_alloc_locked(b->dev, b->blockno);
    if (entry != 0) {
      memmove(entry->buf.data, b->data, BSIZE);
      entry->spec = spec;
    }
  }
  release(&shadow_table.lock);

  if (entry == 0)
    return 0;

  acquiresleep(&entry->buf.lock);
  brelse(b);
  return &entry->buf;
}

// Add a persistent reference when the log pins a shadow buffer.
//added/modified by Saharsh B24CS1105
int
spec_shadow_pin(struct buf *b)
{
  struct shadow *entry;

  acquire(&shadow_table.lock);
  entry = shadow_from_buf_locked(b);
  if (entry != 0)
    b->refcnt++;
  release(&shadow_table.lock);
  return entry != 0;
}

// Remove a reference previously added by spec_shadow_pin().
//added/modified by Saharsh B24CS1105
int
spec_shadow_unpin(struct buf *b)
{
  struct shadow *entry;

  acquire(&shadow_table.lock);
  entry = shadow_from_buf_locked(b);
  if (entry != 0) {
    if (b->refcnt < 1)
      panic("shadow unpin");
    b->refcnt--;
  }
  release(&shadow_table.lock);
  return entry != 0;
}

// Copy a staged block into the table, updating an existing key if present.
//added/modified by Saharsh B24CS1105
int
spec_shadow_store(spec_id_t spec, uint dev, uint blockno, const char *data)
{
  struct shadow *entry;
  struct buf *b;

  if (spec == SPEC_ID_NONE || data == 0)
    return SPEC_ERR_BAD_ID;

  acquire(&shadow_table.lock);
  entry = shadow_find_locked(spec, dev, blockno);
  if (entry == 0) {
    entry = shadow_alloc_locked(dev, blockno);
    if (entry != 0) {
      memmove(entry->buf.data, data, BSIZE);
      entry->spec = spec;
      entry->buf.refcnt--;
    }
    release(&shadow_table.lock);
    return entry == 0 ? SPEC_ERR_NO_SPACE : SPEC_OK;
  }

  b = &entry->buf;
  b->refcnt++;
  release(&shadow_table.lock);

  acquiresleep(&b->lock);
  memmove(b->data, data, BSIZE);
  spec_shadow_release(b);
  return SPEC_OK;
}

// Return 1 and copy the block when found, 0 when the key is absent.
//added/modified by Saharsh B24CS1105
int
spec_shadow_lookup(spec_id_t spec, uint dev, uint blockno, char *data)
{
  struct buf *b;

  if (spec == SPEC_ID_NONE || data == 0)
    return SPEC_ERR_BAD_ID;

  b = spec_shadow_acquire(spec, dev, blockno);
  if (b == 0)
    return 0;
  memmove(data, b->data, BSIZE);
  spec_shadow_release(b);
  return 1;
}

// Count the blocks currently staged by one speculation.
//added/modified by Saharsh B24CS1105
int
spec_shadow_count(spec_id_t spec)
{
  int count = 0;

  if (spec == SPEC_ID_NONE)
    return 0;

  acquire(&shadow_table.lock);
  for (int i = 0; i < NSHADOW; i++) {
    if (shadow_table.entries[i].spec == spec)
      count++;
  }
  release(&shadow_table.lock);
  return count;
}

// Copy the indexed block of a speculation for commit-time enumeration.
//added/modified by Saharsh B24CS1105
int
spec_shadow_get(spec_id_t spec, int index, uint *dev, uint *blockno,
                char *data)
{
  struct buf *b = 0;
  int current = 0;

  if (spec == SPEC_ID_NONE || index < 0 || dev == 0 || blockno == 0 ||
      data == 0)
    return SPEC_ERR_BAD_ID;

  acquire(&shadow_table.lock);
  for (int i = 0; i < NSHADOW; i++) {
    struct shadow *entry = &shadow_table.entries[i];
    if (entry->spec != spec)
      continue;
    if (current++ != index)
      continue;

    b = &entry->buf;
    b->refcnt++;
    *dev = b->dev;
    *blockno = b->blockno;
    break;
  }
  release(&shadow_table.lock);
  if (b == 0)
    return SPEC_ERR_NO_SLOT;

  acquiresleep(&b->lock);
  memmove(data, b->data, BSIZE);
  spec_shadow_release(b);
  return SPEC_OK;
}

// Make every entry owned by a speculation available for later reuse.
//added/modified by Saharsh B24CS1105
void
spec_shadow_discard(spec_id_t spec)
{
  if (spec == SPEC_ID_NONE)
    return;

  acquire(&shadow_table.lock);
  for (int i = 0; i < NSHADOW; i++) {
    struct shadow *entry = &shadow_table.entries[i];
    if (entry->spec == spec)
      entry->spec = SPEC_ID_NONE;
  }
  release(&shadow_table.lock);
}

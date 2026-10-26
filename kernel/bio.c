// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

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
static struct shadow *
shadow_from_buf_locked(struct buf *b)
{
  for (int i = 0; i < NSHADOW; i++) {
    if (&shadow_table.entries[i].buf == b)
      return &shadow_table.entries[i];
  }
  return 0;
}

// Return the matching shadow buffer locked and referenced, or zero on a miss.
static struct buf *
shadow_acquire(spec_id_t spec, uint dev, uint blockno)
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

// Unlock and drop a shadow buffer reference; return zero for normal buffers.
static int
shadow_release(struct buf *b)
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
static int
shadow_is_buffer(struct buf *b)
{
  int found;

  acquire(&shadow_table.lock);
  found = shadow_from_buf_locked(b) != 0;
  release(&shadow_table.lock);
  return found;
}

// Add a persistent reference when the log pins a shadow buffer.
static int
shadow_pin(struct buf *b)
{
  struct shadow *entry;

  acquire(&shadow_table.lock);
  entry = shadow_from_buf_locked(b);
  if (entry != 0)
    b->refcnt++;
  release(&shadow_table.lock);
  return entry != 0;
}

// Remove a reference previously added by shadow_pin().
static int
shadow_unpin(struct buf *b)
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

// Initialize the normal buffer cache and the fixed-size shadow table.
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  initlock(&shadow_table.lock, "shadow_table");
  for (int i = 0; i < NSHADOW; i++) {
    shadow_table.entries[i].spec = SPEC_ID_NONE;
    shadow_table.entries[i].buf.refcnt = 0;
    initsleeplock(&shadow_table.entries[i].buf.lock, "shadow");
  }

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Copy a staged block into the table, updating an existing key if present.
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
  shadow_release(b);
  return SPEC_OK;
}

// Return 1 and copy the block when found, 0 when the key is absent.
int
spec_shadow_lookup(spec_id_t spec, uint dev, uint blockno, char *data)
{
  struct buf *b;

  if (spec == SPEC_ID_NONE || data == 0)
    return SPEC_ERR_BAD_ID;

  b = shadow_acquire(spec, dev, blockno);
  if (b == 0)
    return 0;
  memmove(data, b->data, BSIZE);
  shadow_release(b);
  return 1;
}

// Count the blocks currently staged by one speculation.
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
  if (b == 0) {
    return SPEC_ERR_NO_SLOT;
  }

  acquiresleep(&b->lock);
  memmove(data, b->data, BSIZE);
  shadow_release(b);
  return SPEC_OK;
}

// Make every entry owned by a speculation available for later reuse.
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

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for (b = bcache.head.next; b != &bcache.head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
    if (b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
// Prefer an active speculation's private block, then use the normal cache.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;
  spec_id_t spec;
  enum spec_state state;

  spec = spec_current_id();
  if (spec != SPEC_ID_NONE &&
      spec_get_state(spec, &state) == SPEC_OK &&
      state == SPEC_STATE_ACTIVE) {
    b = shadow_acquire(spec, dev, blockno);
    if (b != 0)
      return b;
  }

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
// Only the log calls bwrite.
// Write ordinary buffers to disk; keep shadow buffers private.
void
bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  if (shadow_is_buffer(b))
    return;
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
// Release a shadow buffer or return a normal buffer to the cache LRU.
void
brelse(struct buf *b)
{
  if (shadow_release(b))
    return;

  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }

  release(&bcache.lock);
}

// Keep either kind of buffer referenced across journal operations.
void
bpin(struct buf *b)
{
  if (shadow_pin(b))
    return;

  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

// Drop a reference previously added by bpin().
void
bunpin(struct buf *b)
{
  if (shadow_unpin(b))
    return;

  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

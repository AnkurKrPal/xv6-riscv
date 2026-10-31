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

void spec_shadow_init(void);
struct buf *spec_shadow_read(uint, uint);
int spec_shadow_release(struct buf *);
int spec_shadow_pin(struct buf *);
int spec_shadow_unpin(struct buf *);
int spec_shadow_is_buffer(struct buf *);

// Initialize the normal buffer cache and the fixed-size shadow table.
//modified by Saharsh B24CS1105
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  spec_shadow_init();

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
//modified by Saharsh B24CS1105
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = spec_shadow_read(dev, blockno);
  if (b != 0)
    return b;

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
//modified by Saharsh B24CS1105
void
bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  if (spec_shadow_is_buffer(b))
    return;
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
// Release a shadow buffer or return a normal buffer to the cache LRU.
//modified by Saharsh B24CS1105
void
brelse(struct buf *b)
{
  if (spec_shadow_release(b))
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
//modified by Saharsh B24CS1105
void
bpin(struct buf *b)
{
  if (spec_shadow_pin(b))
    return;

  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

// Drop a reference previously added by bpin().
//modified by Saharsh B24CS1105
void
bunpin(struct buf *b)
{
  if (spec_shadow_unpin(b))
    return;

  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

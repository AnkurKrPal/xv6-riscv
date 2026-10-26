#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

/*
 * Maximum number of concurrent speculative groups that can buffer output.
 * NSPEC is defined as NPROC (64) in spec_state.c.
 */
#define SPEC_OUTPUT_MAX_SPECS  NPROC

/*
 * Bounded queue geometry per speculation group:
 * Each chunk stores up to SPEC_OUTPUT_CHUNK_SIZE bytes.
 * Each spec group queue can hold at most SPEC_OUTPUT_MAX_CHUNKS chunks.
 * Total bounded buffer capacity = 32 * 64 = 2048 bytes (2 KB) per spec.
 */
#define SPEC_OUTPUT_CHUNK_SIZE 64
#define SPEC_OUTPUT_MAX_CHUNKS 32

/*
 * Output target destination.
 */
enum spec_output_target {
  SPEC_OUT_NONE    = 0,
  SPEC_OUT_CONSOLE = 1,
  SPEC_OUT_PIPE    = 2,
};

/*
 * One chunk in the per-spec FIFO queue.
 */
struct spec_out_chunk {
  enum spec_output_target target;
  struct pipe *pipe;                // Destination pipe (if target == SPEC_OUT_PIPE)
  short len;                        // Valid bytes currently in buf (0..SPEC_OUTPUT_CHUNK_SIZE)
  char buf[SPEC_OUTPUT_CHUNK_SIZE]; // Buffered byte payload
};

/*
 * Bounded circular FIFO queue for a single speculation group.
 */
struct spec_out_queue {
  spec_id_t spec;                   // SPEC_ID_NONE (0) if this queue slot is free
  int head;                         // Index of oldest chunk (for dequeue/flush)
  int tail;                         // Index of next free chunk (for enqueue)
  int count;                        // Number of active chunks in this queue
  int total_bytes;                  // Total payload bytes across all chunks in this queue
  struct spec_out_chunk chunks[SPEC_OUTPUT_MAX_CHUNKS];
};

/*
 * Global table of per-spec output queues protected by a spinlock.
 */
static struct {
  struct spinlock lock;
  struct spec_out_queue queues[SPEC_OUTPUT_MAX_SPECS];
} spec_out_table;

// Caller must hold spec_out_table.lock.
static struct spec_out_queue *
find_queue_locked(spec_id_t spec)
{
  if (spec == SPEC_ID_NONE)
    return 0;

  for (int i = 0; i < SPEC_OUTPUT_MAX_SPECS; i++) {
    if (spec_out_table.queues[i].spec == spec)
      return &spec_out_table.queues[i];
  }
  return 0;
}

// Caller must hold spec_out_table.lock.
static struct spec_out_queue *
get_or_alloc_queue_locked(spec_id_t spec)
{
  struct spec_out_queue *free_q = 0;

  if (spec == SPEC_ID_NONE)
    return 0;

  for (int i = 0; i < SPEC_OUTPUT_MAX_SPECS; i++) {
    struct spec_out_queue *q = &spec_out_table.queues[i];
    if (q->spec == spec)
      return q;
    if (free_q == 0 && q->spec == SPEC_ID_NONE)
      free_q = q;
  }

  if (free_q) {
    free_q->spec = spec;
    free_q->head = 0;
    free_q->tail = 0;
    free_q->count = 0;
    free_q->total_bytes = 0;
    return free_q;
  }
  return 0; // No free queue slot
}

int
spec_output_enqueue(spec_id_t spec, int target, struct pipe *pi, const char *data, int len)
{
  struct spec_out_queue *q;

  if (spec == SPEC_ID_NONE || len <= 0 || data == 0)
    return SPEC_ERR_BAD_ID;

  acquire(&spec_out_table.lock);
  q = get_or_alloc_queue_locked(spec);
  if (q == 0) {
    release(&spec_out_table.lock);
    return SPEC_ERR_NO_SLOT;
  }

  // If the last queued chunk has the same destination and has free space,
  // coalesce into it to conserve chunks.
  if (q->count > 0) {
    int last_idx = (q->tail - 1 + SPEC_OUTPUT_MAX_CHUNKS) % SPEC_OUTPUT_MAX_CHUNKS;
    struct spec_out_chunk *last = &q->chunks[last_idx];
    if (last->target == target && last->pipe == pi && last->len < SPEC_OUTPUT_CHUNK_SIZE) {
      int space = SPEC_OUTPUT_CHUNK_SIZE - last->len;
      int take = (len < space) ? len : space;
      memmove(last->buf + last->len, data, take);
      last->len += take;
      q->total_bytes += take;
      data += take;
      len -= take;
    }
  }

  // Push remaining data into new chunks in the FIFO ring
  while (len > 0) {
    if (q->count >= SPEC_OUTPUT_MAX_CHUNKS) {
      // Bounded queue is full: fail safely, do not overwrite or externalize.
      release(&spec_out_table.lock);
      return SPEC_ERR_NO_SPACE;
    }

    struct spec_out_chunk *c = &q->chunks[q->tail];
    c->target = target;
    c->pipe = pi;
    int take = (len < SPEC_OUTPUT_CHUNK_SIZE) ? len : SPEC_OUTPUT_CHUNK_SIZE;
    memmove(c->buf, data, take);
    c->len = take;

    q->tail = (q->tail + 1) % SPEC_OUTPUT_MAX_CHUNKS;
    q->count++;
    q->total_bytes += take;
    data += take;
    len -= take;
  }

  release(&spec_out_table.lock);
  return SPEC_OK;
}

int
spec_output_avail(spec_id_t spec)
{
  struct spec_out_queue *q;
  int avail;

  if (spec == SPEC_ID_NONE)
    return 0;

  acquire(&spec_out_table.lock);
  q = find_queue_locked(spec);
  if (q == 0) {
    int has_free = 0;
    for (int i = 0; i < SPEC_OUTPUT_MAX_SPECS; i++) {
      if (spec_out_table.queues[i].spec == SPEC_ID_NONE) {
        has_free = 1;
        break;
      }
    }
    avail = has_free ? (SPEC_OUTPUT_MAX_CHUNKS * SPEC_OUTPUT_CHUNK_SIZE) : 0;
  } else {
    int free_chunks = SPEC_OUTPUT_MAX_CHUNKS - q->count;
    avail = free_chunks * SPEC_OUTPUT_CHUNK_SIZE;
    if (q->count > 0) {
      int last_idx = (q->tail - 1 + SPEC_OUTPUT_MAX_CHUNKS) % SPEC_OUTPUT_MAX_CHUNKS;
      avail += (SPEC_OUTPUT_CHUNK_SIZE - q->chunks[last_idx].len);
    }
  }
  release(&spec_out_table.lock);
  return avail;
}

int
spec_output_write_console(spec_id_t spec, int user_src, uint64 src, int n)
{
  char kbuf[SPEC_OUTPUT_CHUNK_SIZE];
  int written = 0;

  if (spec == SPEC_ID_NONE || n <= 0)
    return -1;

  // Step 6 (L9): Fail safely with -1 if output queue is full or cannot fit the write
  if (spec_output_avail(spec) < n)
    return -1;

  while (written < n) {
    int chunk_len = sizeof(kbuf);
    if (chunk_len > n - written)
      chunk_len = n - written;

    if (either_copyin(kbuf, user_src, src + written, chunk_len) == -1) {
      if (written == 0)
        return -1;
      break;
    }

    if (spec_output_enqueue(spec, SPEC_OUT_CONSOLE, 0, kbuf, chunk_len) != SPEC_OK) {
      // Queue is full; fail safely without externalizing
      return -1;
    }
    written += chunk_len;
  }
  return written;
}

int
spec_output_write_pipe(spec_id_t spec, struct pipe *pi, const char *kbuf, int n)
{
  int written = 0;

  if (spec == SPEC_ID_NONE || pi == 0 || n <= 0)
    return -1;

  // Step 6 (L9): Fail safely with -1 if output queue is full or cannot fit the write
  if (spec_output_avail(spec) < n)
    return -1;

  while (written < n) {
    int chunk_len = SPEC_OUTPUT_CHUNK_SIZE;
    if (chunk_len > n - written)
      chunk_len = n - written;

    if (spec_output_enqueue(spec, SPEC_OUT_PIPE, pi, kbuf + written, chunk_len) != SPEC_OK) {
      return -1;
    }
    written += chunk_len;
  }
  return written;
}

int
spec_output_flush(spec_id_t spec)
{
  struct spec_out_queue *q;
  struct spec_out_chunk item;
  int has_item;

  if (spec == SPEC_ID_NONE)
    return SPEC_ERR_BAD_ID;

  while (1) {
    has_item = 0;
    acquire(&spec_out_table.lock);
    q = find_queue_locked(spec);
    if (q == 0 || q->count == 0) {
      if (q != 0) {
        // Queue is completely drained, free the queue slot
        q->spec = SPEC_ID_NONE;
        q->head = 0;
        q->tail = 0;
        q->count = 0;
        q->total_bytes = 0;
      }
      release(&spec_out_table.lock);
      break;
    }

    // Dequeue the oldest chunk in FIFO order
    item = q->chunks[q->head];
    q->head = (q->head + 1) % SPEC_OUTPUT_MAX_CHUNKS;
    q->count--;
    q->total_bytes -= item.len;
    has_item = 1;
    release(&spec_out_table.lock);

    // Deliver the chunk outside the spinlock so that functions which may sleep
    // (such as uartwrite acquiring tx_lock sleeplock) are called safely.
    if (has_item) {
      if (item.target == SPEC_OUT_CONSOLE) {
        if (myproc() != 0) {
          uartwrite(item.buf, item.len);
        } else {
          for (int i = 0; i < item.len; i++)
            uartputc_sync(item.buf[i]);
        }
      } else if (item.target == SPEC_OUT_PIPE) {
        // Hook for pipe delivery in subsequent step
      }
    }
  }

  return SPEC_OK;
}

void
spec_output_discard(spec_id_t spec)
{
  struct spec_out_queue *q;

  if (spec == SPEC_ID_NONE)
    return;

  acquire(&spec_out_table.lock);
  q = find_queue_locked(spec);
  if (q != 0) {
    q->spec = SPEC_ID_NONE;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->total_bytes = 0;
  }
  release(&spec_out_table.lock);
}

int
spec_output_count(spec_id_t spec)
{
  struct spec_out_queue *q;
  int count = 0;

  if (spec == SPEC_ID_NONE)
    return 0;

  acquire(&spec_out_table.lock);
  q = find_queue_locked(spec);
  if (q != 0)
    count = q->count;
  release(&spec_out_table.lock);
  return count;
}

int
spec_output_bytes(spec_id_t spec)
{
  struct spec_out_queue *q;
  int bytes = 0;

  if (spec == SPEC_ID_NONE)
    return 0;

  acquire(&spec_out_table.lock);
  q = find_queue_locked(spec);
  if (q != 0)
    bytes = q->total_bytes;
  release(&spec_out_table.lock);
  return bytes;
}

int
spec_output_is_empty(spec_id_t spec)
{
  return (spec_output_count(spec) == 0);
}

// Verification self-test for FIFO ordering, bounded capacity, coalescing, and discard
static void
spec_output_selftest(void)
{
  spec_id_t test_id = 9999;
  char testbuf[100];
  int rc;

  // 1. Initial state should be empty
  if (!spec_output_is_empty(test_id) || spec_output_count(test_id) != 0 || spec_output_bytes(test_id) != 0)
    panic("spec_output_selftest: initial not empty");

  // 2. Enqueue small batch and check count and bytes
  rc = spec_output_enqueue(test_id, SPEC_OUT_CONSOLE, 0, "Hello ", 6);
  if (rc != SPEC_OK || spec_output_count(test_id) != 1 || spec_output_bytes(test_id) != 6)
    panic("spec_output_selftest: enqueue 1 failed");

  // 3. Coalescing: enqueue to same destination should append to current chunk
  rc = spec_output_enqueue(test_id, SPEC_OUT_CONSOLE, 0, "World!\n", 7);
  if (rc != SPEC_OK || spec_output_count(test_id) != 1 || spec_output_bytes(test_id) != 13)
    panic("spec_output_selftest: coalescing failed");

  // 4. Fill up to chunk boundary
  memset(testbuf, 'A', sizeof(testbuf));
  rc = spec_output_enqueue(test_id, SPEC_OUT_CONSOLE, 0, testbuf, 60);
  if (rc != SPEC_OK || spec_output_count(test_id) != 2 || spec_output_bytes(test_id) != 73)
    panic("spec_output_selftest: split chunk failed");

  // 5. Test capacity bound: fill all chunks until queue is full (total capacity 2048 bytes)
  int full_reached = 0;
  for (int i = 0; i < 60; i++) {
    rc = spec_output_enqueue(test_id, SPEC_OUT_PIPE, 0, testbuf, 50);
    if (rc == SPEC_ERR_NO_SPACE) {
      full_reached = 1;
      break;
    }
  }
  if (!full_reached)
    panic("spec_output_selftest: bounded queue did not reject on overflow");

  // 6. Test fail safely if queue is full: write must return -1
  if (spec_output_write_pipe(test_id, (struct pipe*)1, testbuf, 50) != -1)
    panic("spec_output_selftest: full queue did not fail safely with -1");

  // 7. Test discard: aborting must clear all buffered entries cleanly
  spec_output_discard(test_id);
  if (!spec_output_is_empty(test_id) || spec_output_count(test_id) != 0 || spec_output_bytes(test_id) != 0)
    panic("spec_output_selftest: discard failed to reset queue");

  // 8. Test flush: enqueue, flush, and ensure queue is drained
  rc = spec_output_enqueue(test_id, SPEC_OUT_CONSOLE, 0, "test", 4);
  if (rc != SPEC_OK || spec_output_is_empty(test_id))
    panic("spec_output_selftest: enqueue before flush failed");
  if (spec_output_flush(test_id) != SPEC_OK || !spec_output_is_empty(test_id))
    panic("spec_output_selftest: flush failed to drain queue");

  printk("\nspec_output: steps 5 & 6 (L4 flush/discard & L9 safe failure) PASSED\n");
}

void
spec_output_init(void)
{
  initlock(&spec_out_table.lock, "spec_output");
  for (int i = 0; i < SPEC_OUTPUT_MAX_SPECS; i++) {
    spec_out_table.queues[i].spec = SPEC_ID_NONE;
    spec_out_table.queues[i].head = 0;
    spec_out_table.queues[i].tail = 0;
    spec_out_table.queues[i].count = 0;
    spec_out_table.queues[i].total_bytes = 0;
  }

  spec_output_selftest();
}

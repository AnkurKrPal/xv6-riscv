#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;    // number of bytes read
  uint nwrite;   // number of bytes written
  int readopen;  // read fd is still open
  int writeopen; // write fd is still open
  spec_id_t spec; // speculation ID for uncommitted same-spec data, or SPEC_ID_NONE
  int spec_refs; // count of buffered speculative output references
};

extern struct proc proc[NPROC];

enum pipe_reader_type {
  PIPE_READERS_CLOSED = 0,
  PIPE_READERS_SAME_SPEC = 1,
  PIPE_READERS_OUTSIDE = 2,
  PIPE_READERS_MIXED = 3,
};

enum pipe_reader_type
pipe_classify_readers(struct pipe *pi, spec_id_t writer_spec)
{
  acquire(&pi->lock);
  if (pi->readopen == 0) {
    release(&pi->lock);
    return PIPE_READERS_CLOSED;
  }
  release(&pi->lock);

  int same_spec_count = 0;
  int outside_count = 0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    int is_reader = 0;
    for (int i = 0; i < NOFILE; i++) {
      struct file *f = p->ofile[i];
      if (f && f->type == FD_PIPE && f->pipe == pi && f->readable) {
        is_reader = 1;
        break;
      }
    }
    if (is_reader) {
      if (p->spec == writer_spec)
        same_spec_count++;
      else
        outside_count++;
    }
  }

  if (same_spec_count > 0 && outside_count == 0)
    return PIPE_READERS_SAME_SPEC;
  if (outside_count > 0 && same_spec_count == 0)
    return PIPE_READERS_OUTSIDE;
  return PIPE_READERS_MIXED;
}

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if ((pi = (struct pipe *)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  pi->spec = SPEC_ID_NONE;
  pi->spec_refs = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

bad:
  if (pi)
    kfree((char *)pi);
  if (*f0)
    fileclose(*f0);
  if (*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if (writable) {
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if (pi->readopen == 0 && pi->writeopen == 0 && pi->spec_refs == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else
    release(&pi->lock);
}

void
pipe_add_spec_ref(struct pipe *pi)
{
  if (pi == 0)
    return;
  acquire(&pi->lock);
  pi->spec_refs++;
  release(&pi->lock);
}

void
pipe_drop_spec_ref(struct pipe *pi)
{
  if (pi == 0)
    return;
  acquire(&pi->lock);
  if (pi->spec_refs > 0)
    pi->spec_refs--;
  if (pi->readopen == 0 && pi->writeopen == 0 && pi->spec_refs == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else {
    release(&pi->lock);
  }
}

int
pipe_deliver_spec_chunk(struct pipe *pi, const char *buf, int len)
{
  if (pi == 0 || buf == 0 || len <= 0)
    return -1;

  acquire(&pi->lock);
  int i = 0;
  while (i < len) {
    if (pi->readopen == 0) {
      release(&pi->lock);
      return -1;
    }
    if (pi->nwrite == pi->nread + PIPESIZE) {
      wakeup(&pi->nread);
      if (myproc() != 0) {
        sleep_prepare(&pi->nwrite);
        release(&pi->lock);
        sleep();
        acquire(&pi->lock);
      } else {
        release(&pi->lock);
        return -1;
      }
    } else {
      pi->data[pi->nwrite++ % PIPESIZE] = buf[i++];
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  return i;
}

void
pipe_reset_spec(struct pipe *pi, spec_id_t spec, int discard)
{
  if (pi == 0)
    return;
  acquire(&pi->lock);
  if (pi->spec == spec) {
    if (discard) {
      pi->nwrite = pi->nread;
      wakeup(&pi->nwrite);
    }
    pi->spec = SPEC_ID_NONE;
  }
  release(&pi->lock);
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();
  spec_id_t spec = (pr != 0) ? pr->spec : SPEC_ID_NONE;

  if (pi == 0 || n < 0)
    return -1;

  // Step 10 (L10): Handle closed pipe edge case
  acquire(&pi->lock);
  if (pi->readopen == 0 || (pr && killed(pr))) {
    release(&pi->lock);
    return -1;
  }
  release(&pi->lock);

  // If writing process is executing speculatively:
  if (spec != SPEC_ID_NONE) {
    enum pipe_reader_type rtype = pipe_classify_readers(pi, spec);

    if (rtype == PIPE_READERS_CLOSED) {
      return -1;
    } else if (rtype == PIPE_READERS_MIXED) {
      // Step 9 (L7): Protect against ambiguous/mixed reader ownership leaks
      return -1;
    } else if (rtype == PIPE_READERS_OUTSIDE) {
      // Step 8 (L6): Buffer speculative writes to outside pipe readers
      // Step 6 (L9) & Step 10 (L10): Safe failure if output queue cannot fit the write
      if (spec_output_avail(spec) < n)
        return -1;

      char kbuf[64];
      int total_written = 0;
      while (total_written < n) {
        int chunk_len = sizeof(kbuf);
        if (chunk_len > n - total_written)
          chunk_len = n - total_written;

        if (pr) {
          if (copyin(pr->pagetable, pr->sz, kbuf, addr + total_written, chunk_len) == -1) {
            if (total_written == 0)
              return -1;
            break;
          }
        } else {
          memmove(kbuf, (char *)addr + total_written, chunk_len);
        }

        if (spec_output_write_pipe(spec, pi, kbuf, chunk_len) != chunk_len) {
          if (total_written == 0)
            return -1;
          break;
        }
        total_written += chunk_len;
      }
      return total_written;
    }
    // rtype == PIPE_READERS_SAME_SPEC:
    // Step 7 (L5): Keep same-spec pipe communication immediate
  }

  // Non-speculative write OR immediate same-spec write (L5):
  acquire(&pi->lock);
  while (i < n) {
    if (pi->readopen == 0 || (pr && killed(pr))) {
      release(&pi->lock);
      return -1;
    }
    if (pi->nwrite == pi->nread + PIPESIZE) { //DOC: pipewrite-full
      if (pr == 0) {
        release(&pi->lock);
        return -1;
      }
      wakeup(&pi->nread);
      sleep_prepare(&pi->nwrite);
      release(&pi->lock);
      sleep();
      acquire(&pi->lock);
    } else {
      char ch;
      if (pr) {
        if (copyin(pr->pagetable, pr->sz, &ch, addr + i, 1) == -1) {
          if (i == 0)
            i = -1;
          break;
        }
      } else {
        ch = ((char *)addr)[i];
      }
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      if (spec != SPEC_ID_NONE)
        pi->spec = spec;
      i++;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  spec_id_t spec = (pr != 0) ? pr->spec : SPEC_ID_NONE;
  char ch;

  if (pi == 0 || n < 0)
    return -1;

  acquire(&pi->lock);
  while (pi->writeopen && (pi->nread == pi->nwrite || (pi->spec != SPEC_ID_NONE && spec != pi->spec))) { //DOC: pipe-empty
    if (pr == 0) {
      // In kernel boot / no-proc context, cannot sleep
      release(&pi->lock);
      return 0;
    }
    if (killed(pr)) {
      release(&pi->lock);
      return -1;
    }
    sleep_prepare(&pi->nread); //DOC: piperead-sleep
    release(&pi->lock);
    sleep();
    acquire(&pi->lock);
  }

  // Step 9 (L7): Outside reader must not see uncommitted speculative data
  if (pi->spec != SPEC_ID_NONE && spec != pi->spec) {
    release(&pi->lock);
    return 0;
  }

  for (i = 0; i < n; i++) { //DOC: piperead-copy
    if (pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread % PIPESIZE];
    if (pr) {
      if (copyout(pr->pagetable, pr->sz, addr + i, &ch, 1) == -1) {
        if (i == 0)
          i = -1;
        break;
      }
    } else {
      ((char *)addr)[i] = ch;
    }
    pi->nread++;
    if (pi->nread == pi->nwrite) {
      pi->spec = SPEC_ID_NONE;
    }
  }
  wakeup(&pi->nwrite); //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}

// Verification selftest for L5, L6, L7, L10 pipe isolation and buffering
void
pipe_selftest(void)
{
  struct file *f0 = 0, *f1 = 0;
  char readbuf[32];
  spec_id_t test_id = 9999;
  int rc;

  if (pipealloc(&f0, &f1) != 0)
    panic("pipe_selftest: pipealloc failed");

  struct pipe *p = f0->pipe;

  // 1. Test L6 & L10: Outside reader buffering
  rc = spec_output_write_pipe(test_id, p, "outside_pipe", 12);
  if (rc != 12 || spec_output_is_empty(test_id))
    panic("pipe_selftest: write_pipe buffering failed");

  // Before commit/flush, real pipe buffer must have 0 bytes (no leak to outside)
  if (p->nwrite != 0)
    panic("pipe_selftest: outside pipe leaked before commit");

  // 2. Test L10 & L6: Flush on commit delivers to outside reader
  if (spec_output_flush(test_id) != SPEC_OK || !spec_output_is_empty(test_id))
    panic("pipe_selftest: pipe flush failed");

  if (p->nwrite != 12)
    panic("pipe_selftest: pipe flush did not deliver bytes");

  // Outside reader reads data
  memset(readbuf, 0, sizeof(readbuf));
  int r = piperead(p, (uint64)readbuf, 12);
  if (r != 12 || memcmp(readbuf, "outside_pipe", 12) != 0)
    panic("pipe_selftest: piperead mismatch");

  // 3. Test discard on abort:
  rc = spec_output_write_pipe(test_id, p, "discarded", 9);
  if (rc != 9)
    panic("pipe_selftest: enqueue failed");
  spec_output_discard(test_id);
  if (!spec_output_is_empty(test_id))
    panic("pipe_selftest: discard failed");

  // 4. Test L5 & L7: Same-spec pipe write is immediate, outside reader isolated
  p->spec = test_id;
  char same_data[] = "same_spec";
  acquire(&p->lock);
  for (int i = 0; i < 9; i++) {
    p->data[p->nwrite++ % PIPESIZE] = same_data[i];
  }
  wakeup(&p->nread);
  release(&p->lock);

  // Outside reader cannot read uncommitted same-spec data (L7)
  // With myproc() == 0, spec is SPEC_ID_NONE (!= test_id)
  r = piperead(p, (uint64)readbuf, 9);
  if (r != 0)
    panic("pipe_selftest: outside reader read uncommitted same-spec data");

  // After commit, data is made permanent and readable
  pipe_spec_commit(test_id);
  r = piperead(p, (uint64)readbuf, 9);
  if (r != 9 || memcmp(readbuf, "same_spec", 9) != 0)
    panic("pipe_selftest: committed read failed");

  // 5. Test L10: Closed pipe returns -1
  fileclose(f0); // closes read end
  if (pipewrite(p, (uint64)"fail", 4) != -1)
    panic("pipe_selftest: closed pipe did not return -1");

  fileclose(f1); // closes write end

  printk("pipe: steps 7, 8, 9, 10 (L5, L6, L7, L10 pipe isolation and flush) PASSED\n");
}

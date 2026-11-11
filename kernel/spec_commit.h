#ifndef XV6_SPEC_COMMIT_H
#define XV6_SPEC_COMMIT_H

// Private P3 interface. Each snapshot block occupies one allocated page;
// never put the complete write set on xv6's one-page kernel stack.
struct spec_commit_block {
  struct spec_commit_block *next;
  uint dev;
  uint blockno;
  char data[BSIZE];
};

int spec_conflict_check(spec_id_t, const struct spec_commit_block *);
int spec_log_publish(spec_id_t, const struct spec_commit_block *, int);
int spec_log_is_owner(spec_id_t);

#ifdef SPEC_CRASH_TEST
// Test-only checkpoints; no crash hooks are linked into the production kernel.
enum spec_commit_phase {
  SPEC_PHASE_RESERVED = 1,
  SPEC_PHASE_CHECKED,
  SPEC_PHASE_CACHE_PARTIAL,
  SPEC_PHASE_LOG_PARTIAL,
  SPEC_PHASE_LOG_READY,
  SPEC_PHASE_HEADER_COMMITTED,
  SPEC_PHASE_INSTALL_PARTIAL,
  SPEC_PHASE_HEADER_CLEARED,
  SPEC_PHASE_BEFORE_FLUSH
};
void spec_commit_checkpoint(int, int);
#endif

#endif

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

#endif

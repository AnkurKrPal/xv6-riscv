#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spec_commit.h"

// S7 owns the saturating per-shadow counters and bwrite() publication hook.
// Check the entire frozen snapshot after journal reservation, before touching
// real buffers. The reservation drains earlier writers and excludes new ones
// through commit(), closing the check-to-publish race. A missing counter is
// an invalid snapshot, never permission to overwrite committed data.
int
spec_conflict_check(spec_id_t spec, const struct spec_commit_block *blocks)
{
  const struct spec_commit_block *b;
  uint writes;
  int result;

  for (b = blocks; b != 0; b = b->next) {
    result = spec_shadow_get_write_count(spec, b->dev, b->blockno, &writes);
    if (result != SPEC_OK)
      return result;
    if (writes != 0)
      return SPEC_ERR_CONFLICT;
  }
  return SPEC_OK;
}

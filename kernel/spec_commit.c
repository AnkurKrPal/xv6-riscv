#include "types.h"
#include "spec.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

/*
 * Coordinate lifecycle transition, journal publication, and external output.
 * Output is released only after the filesystem log has completed.
 */
int
spec_commit_transaction(void)
{
  struct proc *p = myproc();
  spec_id_t spec;
  int result;

  if (p == 0)
    return SPEC_ERR_NO_ACTIVE;
  spec = p->spec;
  if (spec == SPEC_ID_NONE)
    return SPEC_ERR_NO_ACTIVE;

  result = spec_commit_start(spec);
  if (result != SPEC_OK)
    return result;

  result = spec_fs_commit(spec);
  if (result != SPEC_OK) {
    spec_shadow_discard(spec);
    spec_output_discard(spec);
    spec_commit_finish(spec, 0);
    return result;
  }

  spec_shadow_discard(spec);
  result = spec_output_flush(spec);
  if (result != SPEC_OK) {
    spec_output_discard(spec);
    pipe_spec_discard(spec);
    spec_commit_finish(spec, 1);
    return result;
  }
  pipe_spec_commit(spec);
  return spec_commit_finish(spec, 1);
}

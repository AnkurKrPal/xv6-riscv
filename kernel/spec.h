#ifndef XV6_SPEC_H
#define XV6_SPEC_H

/*
 * spec_id_t identifies one speculation shared by a process and
 * children forked after xbegin().
 *
 * Zero always means ordinary, non-speculative execution.
 */
typedef int spec_id_t;

#define SPEC_ID_NONE ((spec_id_t)0)

/*
 * A completed or aborted state may exist briefly while process IDs and
 * module resources are being cleaned before the slot becomes FREE.
 */
enum spec_state {
  SPEC_STATE_FREE       = 0,
  SPEC_STATE_ACTIVE     = 1,
  SPEC_STATE_COMMITTING = 2,
  SPEC_STATE_ABORTING   = 3,
  SPEC_STATE_COMMITTED  = 4,
  SPEC_STATE_ABORTED    = 5
};

/*
 * Shared return codes.
 *
 * xbegin(), xcommit(), and xabort() return SPEC_OK on success.
 * xbegin() stores the new nonzero ID in myproc()->spec; the ID does
 * not need to be exposed to user space.
 */
enum spec_error {
  SPEC_OK               =  0,
  SPEC_ERR_NO_ACTIVE    = -1,
  SPEC_ERR_NESTED       = -2,
  SPEC_ERR_BAD_ID       = -3,
  SPEC_ERR_BUSY         = -4,
  SPEC_ERR_NO_SLOT      = -5,
  SPEC_ERR_NO_SPACE     = -6,
  SPEC_ERR_CONFLICT     = -7,
  SPEC_ERR_COMMIT       = -8
};

/*
 * State transitions:
 *
 * FREE -> ACTIVE -> COMMITTING -> COMMITTED -> FREE
 *                   |
 *                   +-> ABORTING -> ABORTED -> FREE
 *
 * ACTIVE -> ABORTING -> ABORTED -> FREE
 */

#endif

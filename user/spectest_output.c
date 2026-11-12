// created by Pal B24CS1078
// L11, L12, L8: User-space output and pipe speculative tests
// Verifies same-spec immediacy, outside buffering, shared-FD inheritance,
// mixed reader rejection, discard on abort, and console visibility.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "kernel/spec.h"
#include "user/user.h"

#define CHECK(condition, step) do { \
  if (!(condition)) { \
    printf("spectest_output: assertion failed at step %d\n", step); \
    exit(step); \
  } \
} while (0)

static void
wait_child(int pid, int expected, int step)
{
  int status = -1;
  int w = wait(&status);
  CHECK(w == pid && status == expected, step);
}

// 1. Normal console output is immediate (L3)
static void
test_console_immediate(void)
{
  const char *msg = "spectest_output: testing normal immediate console write\n";
  int len = strlen(msg);
  int n = write(1, msg, len);
  CHECK(n == len, 101);
}

// 2. Speculative console output is discarded on abort (L11, L4)
static void
test_console_abort_discard(void)
{
  CHECK(xbegin() == SPEC_OK, 201);

  // Write speculative data to console
  const char *secret = "HIDDEN_SPECULATIVE_MESSAGE_SHOULD_ABORT\n";
  int len = strlen(secret);
  int n = write(1, secret, len);
  CHECK(n == len, 202);

  // Abort speculation: buffered output must be discarded
  CHECK(xabort() == SPEC_OK, 203);

  // After abort, subsequent transaction should have clean queue
  CHECK(xbegin() == SPEC_OK, 204);
  const char *valid = "spectest_output: console after abort committed\n";
  len = strlen(valid);
  n = write(1, valid, len);
  CHECK(n == len, 205);
  CHECK(xcommit() == SPEC_OK, 206);
}

// 3. Speculative console output flushes on commit (L11, L8)
static void
test_console_commit_flush(void)
{
  CHECK(xbegin() == SPEC_OK, 301);

  // Multiple small writes that exercise chunk coalescing
  CHECK(write(1, "spectest_output: [1/3] ", 23) == 23, 302);
  CHECK(write(1, "coalesced ", 10) == 10, 303);
  CHECK(write(1, "chunk\n", 6) == 6, 304);

  // A write spanning more than one chunk (chunk size is 64)
  char big[100];
  memset(big, '.', sizeof(big));
  big[0] = 'S';
  big[sizeof(big) - 2] = '\n';
  big[sizeof(big) - 1] = '\0';
  CHECK(write(1, big, sizeof(big) - 1) == sizeof(big) - 1, 305);

  CHECK(xcommit() == SPEC_OK, 306);
}

// 4. Same-spec pipe communication is immediate (L5, L12)
static void
test_pipe_same_spec_immediate(void)
{
  int pfd[2];
  CHECK(pipe(pfd) == 0, 401);

  CHECK(xbegin() == SPEC_OK, 402);

  int pid = fork();
  CHECK(pid >= 0, 403);

  if (pid == 0) {
    // Child inherits same speculation ID
    close(pfd[0]);
    const char *msg = "same_spec_payload";
    CHECK(write(pfd[1], msg, 17) == 17, 404);
    close(pfd[1]);
    exit(0);
  }

  // Parent reads from child while still in speculative mode
  close(pfd[1]);
  char buf[32];
  memset(buf, 0, sizeof(buf));
  int n = read(pfd[0], buf, sizeof(buf) - 1);
  CHECK(n == 17, 405);
  CHECK(strcmp(buf, "same_spec_payload") == 0, 406);
  close(pfd[0]);

  wait_child(pid, 0, 407);
  CHECK(xcommit() == SPEC_OK, 408);
}

// 5. Speculative writes to outside reader are buffered until commit (L6, L8, L12)
static void
test_pipe_outside_buffered_until_commit(void)
{
  int pfd[2];
  CHECK(pipe(pfd) == 0, 501);

  int pid = fork();
  CHECK(pid >= 0, 502);

  if (pid == 0) {
    // Outside reader child: remains non-speculative
    close(pfd[1]);

    // Read the pipe: blocks until parent commits and flushes buffer
    char buf[32];
    memset(buf, 0, sizeof(buf));
    int n = read(pfd[0], buf, sizeof(buf) - 1);
    CHECK(n == 14, 503);
    CHECK(strcmp(buf, "outside_target") == 0, 504);
    close(pfd[0]);
    exit(0);
  }

  // Parent enters speculation
  close(pfd[0]);

  CHECK(xbegin() == SPEC_OK, 505);

  // Write to pipe with outside reader: gets queued in spec_output
  CHECK(write(pfd[1], "outside_target", 14) == 14, 506);

  // Brief pause: child is blocked sleeping in read() because data is held in spec_output
  pause(2);

  // Commit: delivers the queued pipe chunk to the outside reader
  CHECK(xcommit() == SPEC_OK, 507);
  close(pfd[1]);

  wait_child(pid, 0, 508);
}

// 6. Speculative writes to outside reader are discarded on abort (L6, L9, L12)
static void
test_pipe_outside_discard_on_abort(void)
{
  int pfd[2];
  CHECK(pipe(pfd) == 0, 601);

  int pid = fork();
  CHECK(pid >= 0, 602);

  if (pid == 0) {
    // Outside reader child
    close(pfd[1]);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    // Reader should read EOF (0) when write end is closed and data is discarded
    int n = read(pfd[0], buf, sizeof(buf));
    CHECK(n == 0, 603);
    close(pfd[0]);
    exit(0);
  }

  close(pfd[0]);

  CHECK(xbegin() == SPEC_OK, 604);
  CHECK(write(pfd[1], "discard_this_outside", 20) == 20, 605);

  // Abort: drops the buffered pipe chunk without delivery
  CHECK(xabort() == SPEC_OK, 606);

  // Close write end to signal EOF to reader
  close(pfd[1]);

  wait_child(pid, 0, 607);
}

// 7. Shared-FD pipe tests across fork (L12)
static void
test_pipe_shared_fd(void)
{
  int pfd[2];
  CHECK(pipe(pfd) == 0, 701);

  // Fork child while non-speculative: child inherits shared open file table
  int pid = fork();
  CHECK(pid >= 0, 702);

  if (pid == 0) {
    // Child is outside reader on shared pipe
    close(pfd[1]);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    // Child blocks until speculative parent commits
    int n = read(pfd[0], buf, sizeof(buf) - 1);
    CHECK(n == 14, 703);
    CHECK(strcmp(buf, "shared_fd_data") == 0, 704);
    close(pfd[0]);
    exit(0);
  }

  // Parent enters speculation
  CHECK(xbegin() == SPEC_OK, 705);

  // Close parent's read end so child is the only reader
  close(pfd[0]);

  // Write through shared write descriptor: buffered for outside child
  CHECK(write(pfd[1], "shared_fd_data", 14) == 14, 706);

  pause(2);

  // Commit: flushes buffered data to shared pipe
  CHECK(xcommit() == SPEC_OK, 707);
  close(pfd[1]);

  wait_child(pid, 0, 708);
}

// 8. Protect against mixed same-spec and outside readers (L7, L12)
static void
test_pipe_mixed_readers_rejected(void)
{
  int pfd[2];
  int sync_out[2];
  int sync_in[2];
  CHECK(pipe(pfd) == 0, 801);
  CHECK(pipe(sync_out) == 0, 802);
  CHECK(pipe(sync_in) == 0, 803);

  // Fork outside child (spec = 0) that keeps read fd open
  int outside_pid = fork();
  CHECK(outside_pid >= 0, 804);

  if (outside_pid == 0) {
    close(pfd[1]);
    close(sync_out[1]);
    close(sync_in[0]);
    close(sync_in[1]);
    char token;
    read(sync_out[0], &token, 1);
    close(sync_out[0]);
    close(pfd[0]);
    exit(0);
  }

  // Parent enters speculation
  CHECK(xbegin() == SPEC_OK, 805);

  // Fork inside child (same spec) that keeps read fd open
  int inside_pid = fork();
  CHECK(inside_pid >= 0, 806);

  if (inside_pid == 0) {
    close(pfd[1]);
    close(sync_out[0]);
    close(sync_out[1]);
    close(sync_in[1]);
    char token;
    read(sync_in[0], &token, 1);
    close(sync_in[0]);
    close(pfd[0]);
    exit(0);
  }

  // Now pfd has MIXED readers: outside_pid (spec 0) and inside_pid (same spec)
  // Writing from speculative process must be rejected with -1
  int ret = write(pfd[1], "hazardous_data", 14);
  CHECK(ret == -1, 807);

  // First terminate inside child so group members drops back to 1
  close(sync_in[1]);
  close(sync_in[0]);
  wait_child(inside_pid, 0, 808);

  // Now that inside child exited, parent can abort cleanly
  CHECK(xabort() == SPEC_OK, 809);

  // Now terminate outside child
  close(pfd[0]);
  close(pfd[1]);
  close(sync_out[1]);
  close(sync_out[0]);
  wait_child(outside_pid, 0, 810);
}

// 9. Closed pipe and safe capacity failure (L10, L9, L12)
static void
test_pipe_closed_and_safe_failure(void)
{
  int pfd[2];
  CHECK(pipe(pfd) == 0, 901);

  // Close read end before writing
  close(pfd[0]);

  CHECK(xbegin() == SPEC_OK, 902);

  // Writing to a closed pipe must return -1 safely (L10)
  CHECK(write(pfd[1], "closed", 6) == -1, 903);
  close(pfd[1]);

  CHECK(xabort() == SPEC_OK, 904);

  // Now test output queue capacity limits (L9 safe failure)
  int pfd2[2];
  CHECK(pipe(pfd2) == 0, 905);

  // Fork outside reader to trigger buffering
  int pid = fork();
  CHECK(pid >= 0, 906);
  if (pid == 0) {
    close(pfd2[1]);
    char buf[16];
    while (read(pfd2[0], buf, sizeof(buf)) > 0)
      ;
    close(pfd2[0]);
    exit(0);
  }

  close(pfd2[0]);
  CHECK(xbegin() == SPEC_OK, 907);

  // Try writing more than max queue capacity (32 chunks * 64 bytes = 2048 bytes)
  char huge[2500];
  memset(huge, 'X', sizeof(huge));
  int res = write(pfd2[1], huge, sizeof(huge));
  // Must fail safely with -1 rather than corrupting memory or hanging
  CHECK(res == -1, 908);

  close(pfd2[1]);
  CHECK(xabort() == SPEC_OK, 909);
  wait_child(pid, 0, 910);
}

// 10. Interleaved console and pipe ordering is preserved (L8)
static void
test_output_ordering(void)
{
  int pfd[2];
  CHECK(pipe(pfd) == 0, 1001);

  int pid = fork();
  CHECK(pid >= 0, 1002);

  if (pid == 0) {
    // Outside reader
    close(pfd[1]);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    int n = read(pfd[0], buf, sizeof(buf) - 1);
    CHECK(n == 15, 1003);
    CHECK(strcmp(buf, "pipe_interleave") == 0, 1004);
    close(pfd[0]);
    exit(0);
  }

  close(pfd[0]);
  CHECK(xbegin() == SPEC_OK, 1005);

  CHECK(write(1, "spectest_output: [ordering 1/2] console write\n", 46) == 46, 1006);
  CHECK(write(pfd[1], "pipe_interleave", 15) == 15, 1007);
  CHECK(write(1, "spectest_output: [ordering 2/2] console write\n", 46) == 46, 1008);

  CHECK(xcommit() == SPEC_OK, 1009);
  close(pfd[1]);

  wait_child(pid, 0, 1010);
}

static int
run(const char *name, void (*fn)(void))
{
  printf("spectest_output: running %s...\n", name);
  int pid = fork();
  if (pid < 0) {
    printf("spectest_output: fork failed\n");
    return 1;
  }
  if (pid == 0) {
    fn();
    exit(0);
  }
  int status = -1;
  wait(&status);
  if (status != 0) {
    printf("spectest_output: %s FAILED (status %d)\n", name, status);
    return 1;
  }
  printf("spectest_output: %s OK\n", name);
  return 0;
}

int
main(void)
{
  static const struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
    {"console immediate", test_console_immediate},
    {"console abort discard", test_console_abort_discard},
    {"console commit flush", test_console_commit_flush},
    {"pipe same-spec immediate", test_pipe_same_spec_immediate},
    {"pipe outside buffered", test_pipe_outside_buffered_until_commit},
    {"pipe outside discard", test_pipe_outside_discard_on_abort},
    {"pipe shared fd", test_pipe_shared_fd},
    {"pipe mixed readers rejected", test_pipe_mixed_readers_rejected},
    {"pipe closed and capacity limits", test_pipe_closed_and_safe_failure},
    {"interleaved output ordering", test_output_ordering},
  };

  for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (run(tests[i].name, tests[i].fn) != 0) {
      exit(1);
    }
  }

  printf("spectest_output: ALL TESTS PASSED\n");
  exit(0);
}

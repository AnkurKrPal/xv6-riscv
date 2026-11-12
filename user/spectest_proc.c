// A12/A14 (Ankur): syscall, integrated effects and lifecycle regressions.
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "kernel/spec.h"
#include "user/user.h"

#define REUSE_ROUNDS (2 * NPROC + 8)
#define TEST_TIMEOUT 1000
#define CHECK(condition, step) do { if (!(condition)) exit(step); } while (0)

// Assertions in a speculative process use exit status: console diagnostics
// could be buffered and then discarded. The ordinary runner reports failures.
static void
wait_child(int pid, int expected, int step)
{
  int status = -1;
  CHECK(wait(&status) == pid && status == expected, step);
}

static void
invalid_transitions(void)
{
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 1);
  CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 2);
  CHECK(xbegin() == SPEC_OK, 3);
  CHECK(xbegin() == SPEC_ERR_NESTED, 4);
  CHECK(xabort() == SPEC_OK, 5);
  CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 6);
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 7);
  CHECK(xbegin() == SPEC_OK, 8);
  CHECK(xbegin() == SPEC_ERR_NESTED, 9);
  CHECK(xcommit() == SPEC_OK, 10);
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 11);
  CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 12);
  CHECK(xbegin() == SPEC_OK, 13);
  CHECK(xabort() == SPEC_OK, 14);
}

static void
fork_membership(int commit)
{
  int ready[2], gate[2];
  int pid;
  char token;

  CHECK(pipe(ready) == 0, 1);
  CHECK(pipe(gate) == 0, 2);
  CHECK(xbegin() == SPEC_OK, 3);
  pid = fork();
  CHECK(pid >= 0, 4);
  if (pid == 0) {
    close(ready[0]);
    close(gate[1]);
    CHECK(xbegin() == SPEC_ERR_NESTED, 5);
    CHECK(xcommit() == SPEC_ERR_BUSY, 6);
    CHECK(xabort() == SPEC_ERR_BUSY, 7);
    CHECK(write(ready[1], "R", 1) == 1, 8);
    CHECK(read(gate[0], &token, 1) == 1 && token == 'G', 9);
    close(ready[1]);
    close(gate[0]);
    exit(37);
  }

  close(ready[1]);
  close(gate[0]);
  // Both endpoints are in the group, so this handshake is immediately visible.
  CHECK(read(ready[0], &token, 1) == 1 && token == 'R', 10);
  CHECK(xcommit() == SPEC_ERR_BUSY, 11);
  CHECK(xabort() == SPEC_ERR_BUSY, 12);
  CHECK(xbegin() == SPEC_ERR_NESTED, 13);
  CHECK(write(gate[1], "G", 1) == 1, 14);
  close(ready[0]);
  close(gate[1]);
  wait_child(pid, 37, 15);
  CHECK((commit ? xcommit() : xabort()) == SPEC_OK, 16);
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 17);
  CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 18);
  CHECK(wait(0) == -1, 19);
  CHECK(xbegin() == SPEC_OK, 20);
  CHECK(xabort() == SPEC_OK, 21);
}

static void
fork_then_commit(void)
{
  fork_membership(1);
}

static void
fork_then_abort(void)
{
  fork_membership(0);
}

static void
fork_before_begin(void)
{
  int gate[2];
  char token;
  int pid;

  CHECK(pipe(gate) == 0, 1);
  pid = fork();
  CHECK(pid >= 0, 2);
  if (pid == 0) {
    close(gate[1]);
    CHECK(read(gate[0], &token, 1) == 1 && token == 'G', 3);
    close(gate[0]);
    CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 4);
    CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 5);
    CHECK(xbegin() == SPEC_OK, 6);
    CHECK(xabort() == SPEC_OK, 7);
    exit(41);
  }
  close(gate[0]);
  CHECK(xbegin() == SPEC_OK, 8);
  // This child predates xbegin(). It stays alive behind the gate, but must
  // not be counted in this new group or prevent its commit.
  CHECK(xcommit() == SPEC_OK, 9);
  CHECK(write(gate[1], "G", 1) == 1, 10);
  close(gate[1]);
  wait_child(pid, 41, 11);
}

static void
parent_exits_first(void)
{
  int gate[2], result_pipe[2];
  int parent, child, result = -1;
  char token;

  CHECK(pipe(gate) == 0, 1);
  CHECK(pipe(result_pipe) == 0, 2);
  parent = fork();
  CHECK(parent >= 0, 3);
  if (parent == 0) {
    close(result_pipe[0]);
    CHECK(xbegin() == SPEC_OK, 4);
    child = fork();
    CHECK(child >= 0, 5);
    if (child == 0) {
      close(gate[1]);
      result = xbegin() == SPEC_ERR_NESTED ? 0 : 6;
      // EOF proves the original member has reached file cleanup. Its
      // membership may still be pinned until the rest of exit completes.
      if (read(gate[0], &token, 1) != 0)
        result = 7;
      close(gate[0]);
      int rc = SPEC_ERR_BUSY;
      for (int tries = 0; tries < 100 && rc == SPEC_ERR_BUSY; tries++) {
        rc = xcommit();
        if (rc == SPEC_ERR_BUSY)
          pause(1);
      }
      if (rc != SPEC_OK) {
        result = 8;
        xabort();
      }
      // Successful commit detaches us, making the report visible to the
      // ordinary supervisor. An uncommitted report disappears on exit.
      CHECK(write(result_pipe[1], &result, sizeof(result)) == sizeof(result), 9);
      close(result_pipe[1]);
      exit(0);
    }
    close(gate[0]);
    exit(43);
  }
  close(gate[0]);
  close(gate[1]);
  close(result_pipe[1]);
  CHECK(read(result_pipe[0], &result, sizeof(result)) == sizeof(result), 10);
  close(result_pipe[0]);
  wait_child(parent, 43, 11);
  CHECK(result == 0, 12);
  CHECK(xbegin() == SPEC_OK, 13);
  CHECK(xabort() == SPEC_OK, 14);
}

static void
reuse_after_finish(void)
{
  // More cycles than the group-table capacity catch slots left allocated
  // after either commit or abort, as well as stale per-process membership.
  for (int i = 0; i < REUSE_ROUNDS; i++) {
    CHECK(xbegin() == SPEC_OK, 1);
    CHECK(xbegin() == SPEC_ERR_NESTED, 2);
    CHECK((i & 1 ? xcommit() : xabort()) == SPEC_OK, 3);
    CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 4);
    CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 5);
  }
}

static void
reuse_after_exit(void)
{
  for (int i = 0; i < REUSE_ROUNDS; i++) {
    int pid = fork();
    CHECK(pid >= 0, 1);
    if (pid == 0) {
      CHECK(xbegin() == SPEC_OK, 2);
      // Exiting the last member must implicitly abort and free its slot.
      exit(47);
    }
    wait_child(pid, 47, 3);
  }
  CHECK(xbegin() == SPEC_OK, 4);
  CHECK(xabort() == SPEC_OK, 5);
  CHECK(wait(0) == -1, 6);
}

static void
failed_fork_cleanup(void)
{
  int count;
  int failed = 0;

  CHECK(xbegin() == SPEC_OK, 1);
  // Children exit immediately but remain zombies until reaped, forcing a
  // fork failure without relying on allocation sizes or scheduling delays.
  for (count = 0; count < NPROC + 1; count++) {
    int pid = fork();
    if (pid < 0) {
      failed = 1;
      break;
    }
    if (pid == 0)
      exit(0);
  }
  for (int i = 0; i < count; i++) {
    int status = -1;
    CHECK(wait(&status) > 0 && status == 0, 2);
  }
  CHECK(failed && count > 0, 3);
  CHECK(wait(0) == -1, 4);
  // A failed fork must release its admission and membership reservations.
  CHECK(xcommit() == SPEC_OK, 5);
  CHECK(xbegin() == SPEC_OK, 6);
  CHECK(xabort() == SPEC_OK, 7);
}

static void
expect_contents(const char *path, const char *expected, int step)
{
  char data[4];
  int fd = open(path, O_RDONLY);
  CHECK(fd >= 0, step);
  CHECK(read(fd, data, sizeof(data)) == 3, step + 1);
  CHECK(memcmp(data, expected, 3) == 0, step + 2);
  close(fd);
}

// One inherited group stages both file data and outside pipe output. A live
// member blocks both finish requests; after exit, abort/commit must agree
// across lifecycle, storage and output, and the next session must be clean.
static void
integrated_finish(int commit)
{
  const char *path = "proc-integrate";
  int outward[2], ready[2], gate[2];
  char token;
  unlink(path);
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  CHECK(fd >= 0, 1);
  CHECK(write(fd, "old", 3) == 3, 2);
  close(fd);
  sync();
  CHECK(pipe(outward) == 0, 3);
  int observer = fork(); // Predates begin, so it stays outside the group.
  CHECK(observer >= 0, 4);
  if (observer == 0) {
    char data[4];
    close(outward[1]);
    if (commit) {
      CHECK(read(outward[0], data, sizeof(data)) == 3, 5);
      CHECK(memcmp(data, "new", 3) == 0, 6);
    }
    CHECK(read(outward[0], data, sizeof(data)) == 0, 7);
    close(outward[0]);
    expect_contents(path, commit ? "new" : "old", 8);
    exit(0);
  }

  close(outward[0]);
  CHECK(pipe(ready) == 0 && pipe(gate) == 0, 11);
  CHECK(xbegin() == SPEC_OK, 12);
  int member = fork();
  CHECK(member >= 0, 13);
  if (member == 0) {
    close(ready[0]);
    close(gate[1]);
    fd = open(path, O_WRONLY);
    CHECK(fd >= 0, 14);
    CHECK(write(fd, "new", 3) == 3, 15);
    close(fd);
    CHECK(write(outward[1], "new", 3) == 3, 16);
    CHECK(write(ready[1], "R", 1) == 1, 17);
    CHECK(read(gate[0], &token, 1) == 1 && token == 'G', 18);
    exit(0);
  }
  close(ready[1]);
  close(gate[0]);
  CHECK(read(ready[0], &token, 1) == 1 && token == 'R', 19);
  CHECK(xcommit() == SPEC_ERR_BUSY, 20);
  CHECK(xabort() == SPEC_ERR_BUSY, 21);
  expect_contents(path, "new", 22);
  CHECK(write(gate[1], "G", 1) == 1, 25);
  close(ready[0]);
  close(gate[1]);
  wait_child(member, 0, 26);
  CHECK((commit ? xcommit() : xabort()) == SPEC_OK, 27);
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 28);
  CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 29);
  // Commit a fresh empty session before closing the pipe: stale output from
  // an abort would now reach the observer and fail its EOF assertion.
  CHECK(xbegin() == SPEC_OK, 30);
  CHECK(xcommit() == SPEC_OK, 31);
  close(outward[1]);
  wait_child(observer, 0, 32);
  expect_contents(path, commit ? "new" : "old", 33);
  CHECK(unlink(path) == 0, 36);
}

static void
integrated_abort(void)
{
  integrated_finish(0);
}

static void
integrated_commit(void)
{
  integrated_finish(1);
}

// Release independent groups together so SMP runs exercise the shared table
// while other groups join, return BUSY, exit, finish and reuse their slots.
static void
concurrent_lifecycle(void)
{
  enum { WORKERS = 4, ROUNDS = 16 };
  int gate[2], pids[WORKERS];
  char token;
  CHECK(pipe(gate) == 0, 1);
  for (int i = 0; i < WORKERS; i++) {
    pids[i] = fork();
    CHECK(pids[i] >= 0, 2);
    if (pids[i] == 0) {
      close(gate[1]);
      CHECK(read(gate[0], &token, 1) == 1 && token == 'G', 3);
      close(gate[0]);
      for (int round = 0; round < ROUNDS; round++)
        fork_membership((round + i) & 1);
      exit(0);
    }
  }
  close(gate[0]);
  for (int i = 0; i < WORKERS; i++)
    CHECK(write(gate[1], "G", 1) == 1, 4);
  close(gate[1]);
  for (int i = 0; i < WORKERS; i++) {
    int status = -1;
    int pid = wait(&status);
    CHECK(pid > 0 && status == 0, 5);
    int found = 0;
    for (int j = 0; j < WORKERS; j++) {
      if (pids[j] == pid) {
        pids[j] = -1;
        found = 1;
      }
    }
    CHECK(found, 6);
  }
  CHECK(wait(0) == -1, 7);
  CHECK(xbegin() == SPEC_OK, 8);
  CHECK(xcommit() == SPEC_OK, 9);
}

static int
run(const char *name, void (*test)(void))
{
  int status = -1;
  int worker = fork();
  if (worker < 0) {
    fprintf(2, "spectest_proc: %s: cannot fork worker\n", name);
    return 1;
  }
  if (worker == 0) {
    test();
    exit(0);
  }

  int watchdog = fork();
  if (watchdog < 0) {
    kill(worker);
    wait(0);
    fprintf(2, "spectest_proc: %s: cannot fork watchdog\n", name);
    return 1;
  }
  if (watchdog == 0) {
    pause(TEST_TIMEOUT);
    kill(worker);
    exit(124);
  }

  int first = wait(&status);
  int timed_out = first == watchdog;
  if (first == worker) {
    kill(watchdog);
    if (wait(0) != watchdog)
      status = -1;
  } else if (first == watchdog) {
    wait_child(worker, -1, 125);
  } else {
    kill(worker);
    kill(watchdog);
    while (wait(0) > 0)
      ;
    status = -1;
  }

  if (timed_out || status != 0) {
    fprintf(2, "spectest_proc: %s FAILED (step/status %d%s)\n",
            name, status, timed_out || status == -1 ? ", timeout or killed" : "");
    return 1;
  }
  printf("spectest_proc: %s OK\n", name);
  return 0;
}

int
main(void)
{
  static const struct {
    const char *name;
    void (*test)(void);
  } tests[] = {
    {"invalid transitions", invalid_transitions},
    {"fork then commit", fork_then_commit},
    {"fork then abort", fork_then_abort},
    {"fork before begin", fork_before_begin},
    {"parent exits first", parent_exits_first},
    {"reuse after finish", reuse_after_finish},
    {"reuse after exit", reuse_after_exit},
    {"failed fork cleanup", failed_fork_cleanup},
    {"integrated abort", integrated_abort},
    {"integrated commit", integrated_commit},
    {"concurrent lifecycle", concurrent_lifecycle},
  };

  for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (run(tests[i].name, tests[i].test))
      exit(1);
  }
  printf("spectest_proc: ALL TESTS PASSED\n");
  exit(0);
}

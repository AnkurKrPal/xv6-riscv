// P3: frozen snapshots, conflict rejection, and publication regressions.
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/spec.h"
#include "user/user.h"

#define CHECK(ok, step) do { if (!(ok)) exit(step); } while (0)
static char block[BSIZE];
static const char *path = "commit-test";

static void
fill(int fd, int count, char value)
{
  memset(block, value, sizeof(block));
  for (int i = 0; i < count; i++)
    CHECK(write(fd, block, sizeof(block)) == sizeof(block), 10);
}

static void
prepare(int count)
{
  unlink(path);
  int fd = open(path, O_CREATE | O_WRONLY);
  CHECK(fd >= 0, 11);
  fill(fd, count, 'O');
  close(fd);
  sync();
}

static void
verify(int count, char value, int outside_last)
{
  int fd = open(path, O_RDONLY);
  CHECK(fd >= 0, 12);
  for (int i = 0; i < count; i++) {
    CHECK(read(fd, block, sizeof(block)) == sizeof(block), 13);
    char expected = outside_last && i == count - 1 ? 'W' : value;
    for (int j = 0; j < sizeof(block); j++)
      CHECK(block[j] == expected, 14);
  }
  CHECK(read(fd, block, 1) == 0, 15);
  close(fd);
}

static void
publish(void)
{
  prepare(3);
  CHECK(xbegin() == SPEC_OK, 20);
  int fd = open(path, O_WRONLY);
  CHECK(fd >= 0, 21);
  fill(fd, 3, 'P');
  close(fd);
  verify(3, 'P', 0); // Stable private view before commit.
  CHECK(xcommit() == SPEC_OK, 22);
  verify(3, 'P', 0);
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 23);
  CHECK(xbegin() == SPEC_OK, 24);
  CHECK(xcommit() == SPEC_OK, 25); // Empty snapshot.
  unlink(path);
}

static void
capacity(void)
{
  // Existing blocks avoid involving speculative allocation in this test.
  prepare(LOGBLOCKS + 2);
  CHECK(xbegin() == SPEC_OK, 30);
  int fd = open(path, O_WRONLY);
  CHECK(fd >= 0, 31);
  fill(fd, LOGBLOCKS + 2, 'P');
  close(fd);
  CHECK(xcommit() == SPEC_ERR_NO_SPACE, 32);
  verify(LOGBLOCKS + 2, 'O', 0);
  CHECK(xabort() == SPEC_ERR_NO_ACTIVE, 33);
  CHECK(xbegin() == SPEC_OK, 34);
  CHECK(xcommit() == SPEC_OK, 35);
  unlink(path);
}

static void
cache_limit(void)
{
  // Overwriting existing data also stages one inode block. NBUF-2 data
  // blocks therefore leave exactly one cache buffer for journal I/O.
  prepare(NBUF - 1);
  CHECK(xbegin() == SPEC_OK, 36);
  int fd = open(path, O_WRONLY);
  CHECK(fd >= 0, 37);
  fill(fd, NBUF - 2, 'P');
  close(fd);
  CHECK(xcommit() == SPEC_OK, 38);
  fd = open(path, O_RDONLY);
  CHECK(fd >= 0, 39);
  for (int i = 0; i < NBUF - 1; i++) {
    CHECK(read(fd, block, BSIZE) == BSIZE, 40);
    for (int j = 0; j < BSIZE; j++)
      CHECK(block[j] == (i == NBUF - 2 ? 'O' : 'P'), 41);
  }
  close(fd);
  CHECK(xbegin() == SPEC_OK, 42);
  fd = open(path, O_WRONLY);
  CHECK(fd >= 0, 43);
  fill(fd, NBUF - 1, 'X');
  close(fd);
  CHECK(xcommit() == SPEC_ERR_NO_SPACE, 44);
  unlink(path);
}

static void
conflict(void)
{
  int go[2], ack[2], outward[2], pid, status;
  char token;
  prepare(3);
  CHECK(pipe(go) == 0 && pipe(ack) == 0 && pipe(outward) == 0, 50);
  pid = fork(); // This writer predates xbegin(), and stays ordinary.
  CHECK(pid >= 0, 51);
  if (pid == 0) {
    close(go[1]);
    close(ack[0]);
    close(outward[1]);
    CHECK(read(go[0], &token, 1) == 1, 52);
    pause(20);
    // Confirm that the speculation's first two blocks remain invisible.
    verify(3, 'O', 0);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0, 53);
    for (int i = 0; i < 2; i++)
      CHECK(read(fd, block, BSIZE) == BSIZE, 54);
    fill(fd, 1, 'W');
    close(fd);
    sync();
    CHECK(write(ack[1], "A", 1) == 1, 55);
    // Failed commit must discard buffered outward output.
    CHECK(read(outward[0], &token, 1) == 0, 56);
    exit(0);
  }
  close(go[0]);
  close(ack[1]);
  close(outward[0]);
  int start = uptime();
  CHECK(write(go[1], "G", 1) == 1, 57);
  close(go[1]);
  CHECK(xbegin() == SPEC_OK, 58);
  int fd = open(path, O_WRONLY);
  CHECK(fd >= 0, 59);
  fill(fd, 3, 'P');
  close(fd);
  // This bound proves staging precedes the writer's 20-tick delay; a slow
  // machine fails explicitly instead of silently testing the wrong ordering.
  CHECK((uint)(uptime() - start) < 20, 60);
  CHECK(write(outward[1], "X", 1) == 1, 61);
  CHECK(read(ack[0], &token, 1) == 1 && token == 'A', 62);
  CHECK(xcommit() == SPEC_ERR_CONFLICT, 63);
  close(outward[1]);
  close(ack[0]);
  CHECK(wait(&status) == pid && status == 0, 64);
  // No earlier snapshot block may have entered the real cache on rejection.
  verify(3, 'O', 1);
  CHECK(xcommit() == SPEC_ERR_NO_ACTIVE, 65);
  // Fresh baselines must not inherit the discarded shadow's counters.
  CHECK(xbegin() == SPEC_OK, 66);
  fd = open(path, O_WRONLY);
  CHECK(fd >= 0, 67);
  fill(fd, 3, 'F');
  close(fd);
  CHECK(xcommit() == SPEC_OK, 68);
  verify(3, 'F', 0);
  unlink(path);
}

static void
other_speculation(void)
{
  int gate[2], first, second, status;
  char token;
  prepare(3);
  CHECK(pipe(gate) == 0, 70);
  int start = uptime();
  first = fork();
  CHECK(first >= 0, 71);
  if (first == 0) {
    close(gate[1]);
    CHECK(xbegin() == SPEC_OK, 72);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0, 73);
    fill(fd, 3, 'P');
    close(fd);
    CHECK((uint)(uptime() - start) < 20, 74);
    CHECK(read(gate[0], &token, 1) == 1, 75);
    CHECK(xcommit() == SPEC_ERR_CONFLICT, 76);
    verify(3, 'W', 0);
    exit(0);
  }
  close(gate[0]);
  pause(20); // First shadow baseline precedes the second writer.
  second = fork();
  CHECK(second >= 0, 77);
  if (second == 0) {
    close(gate[1]);
    CHECK(xbegin() == SPEC_OK, 78);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0, 79);
    fill(fd, 3, 'W');
    close(fd);
    CHECK(xcommit() == SPEC_OK, 80); // Own publication must not conflict.
    exit(0);
  }
  CHECK(wait(&status) == second && status == 0, 81);
  CHECK(write(gate[1], "G", 1) == 1, 82);
  close(gate[1]);
  CHECK(wait(&status) == first && status == 0, 83);
  verify(3, 'W', 0);
  unlink(path);
}

static void
member_snapshot(void)
{
  int ready[2], gate[2], child, status;
  char token;
  prepare(3);
  CHECK(pipe(ready) == 0 && pipe(gate) == 0, 90);
  CHECK(xbegin() == SPEC_OK, 91);
  child = fork();
  CHECK(child >= 0, 92);
  if (child == 0) {
    close(ready[0]);
    close(gate[1]);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0, 93);
    fill(fd, 3, 'C');
    close(fd);
    CHECK(write(ready[1], "R", 1) == 1, 94);
    CHECK(read(gate[0], &token, 1) == 1, 95);
    exit(0);
  }
  close(ready[1]);
  close(gate[0]);
  CHECK(read(ready[0], &token, 1) == 1, 96);
  CHECK(xcommit() == SPEC_ERR_BUSY, 97); // Live member prevents freezing.
  CHECK(write(gate[1], "G", 1) == 1, 98);
  close(ready[0]);
  close(gate[1]);
  CHECK(wait(&status) == child && status == 0, 99);
  CHECK(xcommit() == SPEC_OK, 100);
  verify(3, 'C', 0); // The child's complete write set survives its exit.
  unlink(path);
}

static int
run(const char *name, void (*test)(void))
{
  int pid = fork(), status = -1;
  if (pid == 0) {
    test();
    exit(0);
  }
  if (pid < 0 || wait(&status) != pid || status != 0) {
    fprintf(2, "spectest_commit: %s FAILED (step %d)\n", name, status);
    return 1;
  }
  printf("spectest_commit: %s OK\n", name);
  return 0;
}

int
main(void)
{
  if (run("snapshot publication", publish) ||
      run("oversize leaves real blocks unchanged", capacity) ||
      run("buffer cache capacity boundary", cache_limit) ||
      run("overlap rejects all blocks and output", conflict) ||
      run("publication conflicts with another owner", other_speculation) ||
      run("freeze waits for sole member", member_snapshot))
    exit(1);
  printf("spectest_commit: ALL TESTS PASSED\n");
  exit(0);
}

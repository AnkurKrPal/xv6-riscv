// S13: filesystem isolation regressions for speculative metadata and data.
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/spec.h"
#include "user/user.h"

#define TEST_TIMEOUT 1000
#define CHECK(condition, step) do { if (!(condition)) exit(step); } while (0)

static const char *read_path = "specfs-read";
static const char *create_path = "specfs-create";
static const char *unlink_path = "specfs-unlink";
static const char *abort_old_path = "sfs-abort-old";
static const char *abort_new_path = "sfs-abort-new";
static const char *abort_nested_path = "sfs-abort-new/file";

static void expect_present(const char *, int);

// Exhaust private staging and require unlink to report failure without panic.
//added by Saharsh B24CS1105
static void
full_table_unlink(void)
{
  char block[BSIZE];
  int fd, victim;

  unlink("sfs-cap-data");
  unlink("sfs-cap-empty");
  fd = open("sfs-cap-data", O_CREATE | O_WRONLY);
  CHECK(fd >= 0, 150);
  memset(block, 'O', sizeof(block));
  for (int i = 0; i < 70; i++)
    CHECK(write(fd, block, sizeof(block)) == sizeof(block), 151);
  close(fd);
  victim = open("sfs-cap-empty", O_CREATE | O_WRONLY);
  CHECK(victim >= 0, 152);
  close(victim);
  sync();

  CHECK(xbegin() == SPEC_OK, 153);
  // Reserve only the victim's inode; its parent directory is still real.
  victim = open("sfs-cap-empty", O_WRONLY | O_TRUNC);
  CHECK(victim >= 0, 154);
  close(victim);
  fd = open("sfs-cap-data", O_WRONLY);
  CHECK(fd >= 0, 155);
  memset(block, 'P', sizeof(block));
  int i;
  for (i = 0; i < 70; i++) {
    if (write(fd, block, sizeof(block)) != sizeof(block))
      break;
  }
  CHECK(i < 70, 156);
  close(fd);
  CHECK(unlink("sfs-cap-empty") == -1, 157);
  expect_present("sfs-cap-empty", 158);
  CHECK(xabort() == SPEC_OK, 159);
  expect_present("sfs-cap-empty", 160);
  unlink("sfs-cap-data");
  unlink("sfs-cap-empty");
}

// Create a committed baseline file with the requested contents.
//added by Saharsh B24CS1105
static void
prepare_file(const char *path, const char *contents, int step)
{
  int length = strlen(contents);

  unlink(path);
  int fd = open(path, O_CREATE | O_WRONLY);
  CHECK(fd >= 0, step);
  CHECK(write(fd, contents, length) == length, step + 1);
  close(fd);
  sync();
}

// Create a committed empty directory for a namespace baseline.
//added by Saharsh B24CS1105
static void
prepare_directory(const char *path, int step)
{
  unlink(path);
  CHECK(mkdir(path) == 0, step);
  sync();
}

// Verify that a file contains exactly the requested bytes.
//added by Saharsh B24CS1105
static void
expect_file(const char *path, const char *contents, int step)
{
  char buffer[32];
  int length = strlen(contents);
  int fd = open(path, O_RDONLY);

  CHECK(fd >= 0, step);
  CHECK(length < sizeof(buffer), step + 1);
  CHECK(read(fd, buffer, length) == length, step + 2);
  CHECK(memcmp(buffer, contents, length) == 0, step + 3);
  CHECK(read(fd, buffer, 1) == 0, step + 4);
  close(fd);
}

// Verify that a pathname exists and can be opened read-only.
//added by Saharsh B24CS1105
static void
expect_present(const char *path, int step)
{
  int fd = open(path, O_RDONLY);

  CHECK(fd >= 0, step);
  close(fd);
}

// Verify that a pathname is absent from the caller's filesystem view.
//added by Saharsh B24CS1105
static void
expect_missing(const char *path, int step)
{
  int fd = open(path, O_RDONLY);

  if (fd >= 0) {
    close(fd);
    exit(step);
  }
}

// Wait for one child and require its expected exit status.
//added by Saharsh B24CS1105
static void
wait_child(int pid, int expected, int step)
{
  int status = -1;

  CHECK(wait(&status) == pid && status == expected, step);
}

// Confirm private reads and restore the committed contents on abort.
//added by Saharsh B24CS1105
static void
read_isolation_abort(void)
{
  int go[2], ack[2];
  int pid;
  char token;

  prepare_file(read_path, "old", 10);
  CHECK(pipe(go) == 0 && pipe(ack) == 0, 12);
  pid = fork();
  CHECK(pid >= 0, 13);
  if (pid == 0) {
    close(go[1]);
    close(ack[0]);
    CHECK(read(go[0], &token, 1) == 1, 14);
    pause(20);
    expect_file(read_path, "old", 15);
    CHECK(write(ack[1], "A", 1) == 1, 20);
    exit(0);
  }

  close(go[0]);
  close(ack[1]);
  int start = uptime();
  CHECK(write(go[1], "G", 1) == 1, 21);
  close(go[1]);
  CHECK(xbegin() == SPEC_OK, 22);
  int fd = open(read_path, O_WRONLY);
  CHECK(fd >= 0, 23);
  CHECK(write(fd, "new", 3) == 3, 24);
  close(fd);
  expect_file(read_path, "new", 25);
  CHECK((uint)(uptime() - start) < 20, 30);
  CHECK(read(ack[0], &token, 1) == 1 && token == 'A', 31);
  close(ack[0]);
  CHECK(xabort() == SPEC_OK, 32);
  wait_child(pid, 0, 33);
  expect_file(read_path, "old", 34);
  unlink(read_path);
}

// Confirm private creation and publish the new namespace entry on commit.
//added by Saharsh B24CS1105
static void
create_visibility_commit(void)
{
  int go[2], ack[2], gate[2];
  int pid;
  char token;

  unlink(create_path);
  sync();
  CHECK(pipe(go) == 0 && pipe(ack) == 0 && pipe(gate) == 0, 40);
  pid = fork();
  CHECK(pid >= 0, 41);
  if (pid == 0) {
    close(go[1]);
    close(ack[0]);
    close(gate[1]);
    CHECK(read(go[0], &token, 1) == 1, 42);
    pause(20);
    expect_missing(create_path, 43);
    CHECK(write(ack[1], "A", 1) == 1, 44);
    CHECK(read(gate[0], &token, 1) == 1, 45);
    expect_file(create_path, "made", 46);
    exit(0);
  }

  close(go[0]);
  close(ack[1]);
  close(gate[0]);
  int start = uptime();
  CHECK(write(go[1], "G", 1) == 1, 51);
  close(go[1]);
  CHECK(xbegin() == SPEC_OK, 52);
  int fd = open(create_path, O_CREATE | O_WRONLY);
  CHECK(fd >= 0, 53);
  CHECK(write(fd, "made", 4) == 4, 54);
  close(fd);
  expect_file(create_path, "made", 55);
  CHECK((uint)(uptime() - start) < 20, 60);
  CHECK(read(ack[0], &token, 1) == 1 && token == 'A', 61);
  close(ack[0]);
  CHECK(xcommit() == SPEC_OK, 62);
  CHECK(write(gate[1], "C", 1) == 1, 63);
  close(gate[1]);
  wait_child(pid, 0, 64);
  expect_file(create_path, "made", 65);
  unlink(create_path);
}

// Confirm private unlink and publish pathname removal on commit.
//added by Saharsh B24CS1105
static void
unlink_visibility_commit(void)
{
  int go[2], ack[2], gate[2];
  int pid;
  char token;

  prepare_file(unlink_path, "kept", 70);
  CHECK(pipe(go) == 0 && pipe(ack) == 0 && pipe(gate) == 0, 72);
  pid = fork();
  CHECK(pid >= 0, 73);
  if (pid == 0) {
    close(go[1]);
    close(ack[0]);
    close(gate[1]);
    CHECK(read(go[0], &token, 1) == 1, 74);
    pause(20);
    expect_file(unlink_path, "kept", 75);
    CHECK(write(ack[1], "A", 1) == 1, 80);
    CHECK(read(gate[0], &token, 1) == 1, 81);
    expect_missing(unlink_path, 82);
    exit(0);
  }

  close(go[0]);
  close(ack[1]);
  close(gate[0]);
  int start = uptime();
  CHECK(write(go[1], "G", 1) == 1, 83);
  close(go[1]);
  CHECK(xbegin() == SPEC_OK, 84);
  CHECK(unlink(unlink_path) == 0, 85);
  expect_missing(unlink_path, 86);
  CHECK((uint)(uptime() - start) < 20, 87);
  CHECK(read(ack[0], &token, 1) == 1 && token == 'A', 88);
  close(ack[0]);
  CHECK(xcommit() == SPEC_OK, 89);
  CHECK(write(gate[1], "C", 1) == 1, 90);
  close(gate[1]);
  wait_child(pid, 0, 91);
  expect_missing(unlink_path, 92);
}

// Confirm abort discards both a private create and a private unlink.
//added by Saharsh B24CS1105
static void
namespace_abort(void)
{
  int go[2], ack[2], gate[2];
  int pid;
  char token;

  prepare_directory(abort_old_path, 100);
  unlink(abort_nested_path);
  unlink(abort_new_path);
  sync();
  CHECK(pipe(go) == 0 && pipe(ack) == 0 && pipe(gate) == 0, 102);
  pid = fork();
  CHECK(pid >= 0, 103);
  if (pid == 0) {
    close(go[1]);
    close(ack[0]);
    close(gate[1]);
    CHECK(read(go[0], &token, 1) == 1, 104);
    pause(20);
    expect_present(abort_old_path, 105);
    expect_missing(abort_new_path, 110);
    CHECK(write(ack[1], "A", 1) == 1, 111);
    CHECK(read(gate[0], &token, 1) == 1, 112);
    expect_present(abort_old_path, 113);
    expect_missing(abort_new_path, 118);
    exit(0);
  }

  close(go[0]);
  close(ack[1]);
  close(gate[0]);
  int start = uptime();
  CHECK(write(go[1], "G", 1) == 1, 119);
  close(go[1]);
  CHECK(xbegin() == SPEC_OK, 120);
  CHECK(unlink(abort_old_path) == 0, 121);
  CHECK(mkdir(abort_new_path) == 0, 122);
  int fd = open(abort_nested_path, O_CREATE | O_WRONLY);
  CHECK(fd >= 0, 123);
  CHECK(write(fd, "drop", 4) == 4, 124);
  close(fd);
  expect_missing(abort_old_path, 125);
  expect_present(abort_new_path, 126);
  expect_file(abort_nested_path, "drop", 127);
  CHECK((uint)(uptime() - start) < 20, 130);
  CHECK(read(ack[0], &token, 1) == 1 && token == 'A', 131);
  close(ack[0]);
  CHECK(xabort() == SPEC_OK, 132);
  CHECK(write(gate[1], "R", 1) == 1, 133);
  close(gate[1]);
  wait_child(pid, 0, 134);
  expect_present(abort_old_path, 135);
  expect_missing(abort_new_path, 140);
  unlink(abort_old_path);
}

// Check private link counts, truncation, file size and same-group reads.
//added by Saharsh B24CS1105
static void
metadata_abort(void)
{
  struct stat st;
  int fd, pid;

  unlink("sfs-meta-link");
  prepare_file("sfs-meta", "before", 170);
  CHECK(xbegin() == SPEC_OK, 172);
  CHECK(link("sfs-meta", "sfs-meta-link") == 0, 173);
  fd = open("sfs-meta", O_WRONLY | O_TRUNC);
  CHECK(fd >= 0, 174);
  CHECK(write(fd, "new", 3) == 3, 175);
  CHECK(fstat(fd, &st) == 0 && st.size == 3 && st.nlink == 2, 176);
  close(fd);
  pid = fork();
  CHECK(pid >= 0, 177);
  if (pid == 0) {
    expect_file("sfs-meta-link", "new", 178);
    exit(0);
  }
  wait_child(pid, 0, 183);
  CHECK(xabort() == SPEC_OK, 184);
  expect_file("sfs-meta", "before", 185);
  expect_missing("sfs-meta-link", 190);
  CHECK(stat("sfs-meta", &st) == 0 && st.size == 6 && st.nlink == 1, 191);
  unlink("sfs-meta");
}

// Grow an existing indirect mapping privately, then restore it on abort.
//added by Saharsh B24CS1105
static void
indirect_growth_abort(void)
{
  char block[BSIZE];
  struct stat st;
  int fd;

  unlink("sfs-indirect");
  fd = open("sfs-indirect", O_CREATE | O_WRONLY);
  CHECK(fd >= 0, 200);
  memset(block, 'O', sizeof(block));
  for (int i = 0; i < NDIRECT + 1; i++)
    CHECK(write(fd, block, sizeof(block)) == sizeof(block), 201);
  close(fd);
  sync();
  CHECK(xbegin() == SPEC_OK, 202);
  fd = open("sfs-indirect", O_RDWR);
  CHECK(fd >= 0, 203);
  for (int i = 0; i < NDIRECT + 1; i++)
    CHECK(read(fd, block, sizeof(block)) == sizeof(block), 204);
  memset(block, 'P', sizeof(block));
  CHECK(write(fd, block, sizeof(block)) == sizeof(block), 205);
  CHECK(fstat(fd, &st) == 0 && st.size == (NDIRECT + 2) * BSIZE, 206);
  close(fd);
  CHECK(xabort() == SPEC_OK, 207);
  fd = open("sfs-indirect", O_RDONLY);
  CHECK(fd >= 0, 208);
  CHECK(fstat(fd, &st) == 0 && st.size == (NDIRECT + 1) * BSIZE, 209);
  for (int i = 0; i < NDIRECT + 1; i++) {
    CHECK(read(fd, block, sizeof(block)) == sizeof(block), 210);
    for (int j = 0; j < sizeof(block); j++)
      CHECK(block[j] == 'O', 211);
  }
  CHECK(read(fd, block, 1) == 0, 212);
  close(fd);
  unlink("sfs-indirect");
}

// Run one test in isolation and kill it if an operation deadlocks.
//added by Saharsh B24CS1105
static int
run(const char *name, void (*test)(void))
{
  int status = -1;
  int worker = fork();

  if (worker < 0) {
    fprintf(2, "spectest_fs: %s: cannot fork worker\n", name);
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
    fprintf(2, "spectest_fs: %s: cannot fork watchdog\n", name);
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
    kill(worker);
    wait(0);
  } else {
    kill(worker);
    kill(watchdog);
    while (wait(0) > 0)
      ;
    status = -1;
  }

  if (timed_out || status != 0) {
    fprintf(2, "spectest_fs: %s FAILED (step/status %d%s)\n",
            name, status, timed_out || status == -1 ? ", timeout or killed" : "");
    return 1;
  }
  printf("spectest_fs: %s OK\n", name);
  return 0;
}

// Run all filesystem isolation regressions.
//added by Saharsh B24CS1105
int
main(void)
{
  static const struct {
    const char *name;
    void (*test)(void);
  } tests[] = {
    {"private reads and abort", read_isolation_abort},
    {"create visibility and commit", create_visibility_commit},
    {"unlink visibility and commit", unlink_visibility_commit},
    {"namespace abort", namespace_abort},
    {"full table unlink failure", full_table_unlink},
    {"link counts, truncate and same-group reads", metadata_abort},
    {"indirect growth and abort", indirect_growth_abort},
  };

  for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    if (run(tests[i].name, tests[i].test))
      exit(1);
  }
  printf("spectest_fs: ALL TESTS PASSED\n");
  exit(0);
}

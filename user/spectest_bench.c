// L13: Performance benchmark comparing normal vs speculative writes (1 KB - 64 KB).
// Measures execution cost and demonstrates the shadow table capacity cliff (NSHADOW=64).

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "kernel/fs.h"
#include "kernel/spec.h"
#include "user/user.h"

#define ITERS 1

static char buf[BSIZE];

// Benchmark normal writes outside speculation
static int
bench_normal(int size_kb, int *out_ticks)
{
  int t0 = uptime();
  for (int iter = 0; iter < ITERS; iter++) {
    unlink("b_norm");
    int fd = open("b_norm", O_CREATE | O_WRONLY);
    if (fd < 0)
      return -1;

    for (int b = 0; b < size_kb; b++) {
      if (write(fd, buf, BSIZE) != BSIZE) {
        close(fd);
        unlink("b_norm");
        return -1;
      }
    }
    close(fd);
  }
  int t1 = uptime();
  unlink("b_norm");

  *out_ticks = (t1 - t0);
  return 0;
}

// Benchmark speculative writes inside xbegin() ... xabort()
static int
bench_speculative(int size_kb, int *out_ticks)
{
  int total_ticks = 0;
  int hit_cliff = 0;

  for (int iter = 0; iter < ITERS; iter++) {
    if (xbegin() != SPEC_OK)
      return -1;

    int t0 = uptime();
    int fd = open("b_spec", O_CREATE | O_WRONLY);
    if (fd < 0) {
      xabort();
      return -1;
    }

    for (int b = 0; b < size_kb; b++) {
      if (write(fd, buf, BSIZE) != BSIZE) {
        // Shadow table capacity exhausted (NSHADOW limit reached)
        hit_cliff = 1;
        break;
      }
    }
    close(fd);
    int t1 = uptime();

    xabort();

    if (hit_cliff) {
      *out_ticks = -1;
      return -1;
    }
    total_ticks += (t1 - t0);
  }

  *out_ticks = total_ticks;
  return 0;
}

int
main(int argc, char *argv[])
{
  // Sizes to evaluate: 1 KB up to 64 KB
  int sizes[] = {
    1, 2, 4, 8, 12, 16, 20, 24, 28, 32,
    36, 40, 44, 48, 52, 56, 58, 60, 62, 64
  };
  int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

  // Fill write buffer with arbitrary pattern
  memset(buf, 'X', sizeof(buf));

  printf("=== xv6 Write Benchmark: Normal vs Speculative (1KB-64KB) ===\n");
  printf("Comparing write latency across file sizes.\n");
  printf("------------------------------------------------------------\n");

  for (int i = 0; i < num_sizes; i++) {
    int sz = sizes[i];
    int norm_ticks = 0;
    int spec_ticks = 0;

    int norm_rc = bench_normal(sz, &norm_ticks);
    if (norm_rc != 0) {
      printf("size %d KB: normal write failed!\n", sz);
      exit(1);
    }
    int spec_rc = bench_speculative(sz, &spec_ticks);

    if (spec_rc == 0) {
      printf("size %d KB: normal = %d ticks, spec = %d ticks [OK]\n",
             sz, norm_ticks, spec_ticks);
      printf("DATA,%d,%d,OK,%d,OK\n", sz, norm_ticks, spec_ticks);
    } else {
      printf("size %d KB: normal = %d ticks, spec = CLIFF (out of shadow slots)\n",
             sz, norm_ticks);
      printf("DATA,%d,%d,OK,-1,CLIFF\n", sz, norm_ticks);
    }
  }

  printf("------------------------------------------------------------\n");
  printf("spectest_bench: ALL BENCHMARKS COMPLETED\n");
  exit(0);
}

#!/usr/bin/env python3
# created by Pal B24CS1078
"""
L14: Parse benchmark output and plot comparative graph of Normal vs Speculative writes.
Demonstrates write cost scaling from 1 KB to 64 KB and the shadow table capacity cliff.
"""

import sys
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

def parse_log(filename):
    sizes = []
    norm_times = []
    spec_times = []
    cliff_size = None

    if not os.path.exists(filename):
        print(f"Error: Log file '{filename}' not found.")
        sys.exit(1)

    with open(filename, 'r', errors='replace') as f:
        for line in f:
            line = line.strip()
            if line.startswith('DATA,'):
                parts = line.split(',')
                if len(parts) >= 6:
                    sz = int(parts[1])
                    norm_t = int(parts[2])
                    spec_t = int(parts[4])
                    spec_status = parts[5]

                    sizes.append(sz)
                    norm_times.append(norm_t)

                    if spec_status == 'CLIFF' or spec_t < 0:
                        if cliff_size is None:
                            cliff_size = sz
                        spec_times.append(None)
                    else:
                        spec_times.append(spec_t)

    return sizes, norm_times, spec_times, cliff_size

def plot_benchmark(sizes, norm_times, spec_times, cliff_size, out_img):
    plt.figure(figsize=(10, 6), dpi=150)

    # Plot normal writes
    plt.plot(sizes, norm_times, marker='o', linewidth=2.2, color='#1f77b4',
             label='Normal Writes (Outside Speculation)')

    # Filter valid speculative points
    valid_sizes = [sz for sz, st in zip(sizes, spec_times) if st is not None]
    valid_spec = [st for st in spec_times if st is not None]

    # Plot speculative writes
    plt.plot(valid_sizes, valid_spec, marker='s', linewidth=2.2, color='#2ca02c',
             label='Speculative Writes (In-Memory Shadow Table)')

    # Highlight capacity cliff
    if cliff_size is not None:
        plt.axvline(x=cliff_size, color='#d62728', linestyle='--', linewidth=2,
                    label=f'Capacity Cliff ({cliff_size} KB, NSHADOW=64 full)')
        plt.scatter([cliff_size], [0], color='#d62728', s=120, zorder=5)
        plt.annotate(
            f'Capacity Cliff at {cliff_size} KB\n(write() returns -1, no leak)',
            xy=(cliff_size, 2),
            xytext=(cliff_size - 18, max(norm_times) * 0.65),
            arrowprops=dict(facecolor='#d62728', shrink=0.08, width=1.5, headwidth=8),
            bbox=dict(boxstyle='round,pad=0.5', facecolor='#ffebee', edgecolor='#d62728', alpha=0.9),
            fontsize=10,
            fontweight='bold',
            color='#b71c1c'
        )

    plt.title('xv6 Write Performance: Normal vs. Speculative Execution (1 KB – 64 KB)',
              fontsize=13, fontweight='bold', pad=15)
    plt.xlabel('File Size (KB)', fontsize=11, labelpad=8)
    plt.ylabel('Write Latency (Timer Ticks)', fontsize=11, labelpad=8)
    plt.xlim(0, 66)
    plt.ylim(-1, max(norm_times) + 3)
    plt.xticks([1, 8, 16, 24, 32, 40, 48, 56, 60, 64])
    plt.grid(True, linestyle=':', alpha=0.6)
    plt.legend(loc='upper left', framealpha=0.95, fontsize=10)
    plt.tight_layout()

    plt.savefig(out_img)
    plt.close()
    print(f"Benchmark graph saved successfully to: {out_img}")

def main():
    log_file = sys.argv[1] if len(sys.argv) > 1 else 'bench_results.log'
    out_file = sys.argv[2] if len(sys.argv) > 2 else 'spec_write_benchmark.png'

    print(f"Parsing benchmark data from: {log_file}")
    sizes, norm_times, spec_times, cliff_size = parse_log(log_file)

    if not sizes:
        print("Error: No benchmark DATA points found in log.")
        sys.exit(1)

    print(f"Parsed {len(sizes)} size points.")
    if cliff_size:
        print(f"Detected Shadow Table Capacity Cliff at: {cliff_size} KB (write returns -1)")

    plot_benchmark(sizes, norm_times, spec_times, cliff_size, out_file)

if __name__ == '__main__':
    main()


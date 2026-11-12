#!/usr/bin/env bash
# created by Pal B24CS1078
# L14: Automate QEMU benchmark execution and generate performance graph.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

LOG_FILE="${ROOT_DIR}/bench_results.log"
GRAPH_FILE="${ROOT_DIR}/spec_write_benchmark.png"

echo "=== Building xv6 image with benchmark binary ==="
make -j4 fs.img

echo "=== Running 1-64 KB write benchmark in QEMU ==="
python3 -u -c "
import sys
sys.path.insert(0, '.')
from tests.spec_test_support import Guest

g = Guest('kernel/kernel', 'fs.img', 1)
try:
    g.until(b'$ ')
    g.send(b'specbench\n')
    res = g.until(b'spectest_bench: ALL BENCHMARKS COMPLETED', timeout=120)
    with open('${LOG_FILE}', 'wb') as f:
        f.write(g.output)
    print('Benchmark completed successfully.')
finally:
    g.close()
"

echo "=== Raw log written to: ${LOG_FILE} ==="

echo "=== Generating benchmark plot ==="
python3 "${SCRIPT_DIR}/plot_bench.py" "${LOG_FILE}" "${GRAPH_FILE}"

echo "=== All benchmark steps completed! ==="


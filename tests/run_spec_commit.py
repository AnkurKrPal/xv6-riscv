"""Run P3 and A8 regression suites in isolated xv6 images with 1 and 3 CPUs."""
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(cpus):
    output = bytearray()
    log = ROOT / 'tests' / f'spec_commit_cpus{cpus}.log'
    with tempfile.TemporaryDirectory(prefix='xv6-commit-') as directory:
        image = Path(directory) / 'fs.img'
        shutil.copyfile(ROOT / 'fs.img', image)
        command = [
            'qemu-system-riscv64', '-machine', 'virt', '-bios', 'none',
            '-kernel', str(ROOT / 'kernel/kernel'), '-m', '128M',
            '-smp', str(cpus), '-nographic',
            '-global', 'virtio-mmio.force-legacy=false',
            '-drive', f'file={image},if=none,format=raw,id=x0',
            '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0',
        ]
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   bufsize=0)
        def until(marker):
            deadline = time.monotonic() + 60
            start = len(output)
            while marker not in output[start:]:
                if time.monotonic() >= deadline:
                    raise RuntimeError(f'{cpus} CPUs: timed out waiting for {marker!r}')
                if select.select([process.stdout], [], [], 0.2)[0]:
                    data = process.stdout.read(4096)
                    if not data:
                        raise RuntimeError('QEMU exited unexpectedly')
                    output.extend(data)
                    if b'panic:' in data or b'FAILED' in data:
                        raise RuntimeError(data.decode(errors='replace'))
        try:
            until(b'$ ')
            for program, marker in [
                (b'speccommit', b'spectest_commit: ALL TESTS PASSED'),
                (b'spectest_proc', b'spectest_proc: ALL TESTS PASSED'),
            ]:
                process.stdin.write(program + b'\n')
                process.stdin.flush()
                until(marker)
            print(f'PASS: commit and lifecycle suites, CPUS={cpus}', flush=True)
        finally:
            process.stdin.write(b'\x01x')
            process.stdin.flush()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            log.write_bytes(output)
            process.stdin.close()
            process.stdout.close()


if __name__ == '__main__':
    for cpus in (1, 3):
        run(cpus)

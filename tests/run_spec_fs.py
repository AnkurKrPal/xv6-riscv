"""Run filesystem, commit and lifecycle regressions in disposable QEMU disks."""

import argparse
from pathlib import Path
import select
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


# Run each suite against a fresh QEMU overlay and preserve the console evidence.
# added by Saharsh B24CS1105
def run(cpus, log_directory, quick_usertests):
    output = bytearray()
    command = [
        "qemu-system-riscv64", "-machine", "virt", "-bios", "none",
        "-kernel", str(ROOT / "kernel/kernel"), "-m", "128M",
        "-smp", str(cpus), "-nographic", "-snapshot",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={ROOT / 'fs.img'},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
    ]
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0,
    )

    # Check the accumulated text so errors split across reads are also detected.
    # added by Saharsh B24CS1105
    def until(marker, start, timeout=180):
        deadline = time.monotonic() + timeout
        while marker not in output[start:]:
            if time.monotonic() >= deadline:
                raise RuntimeError(f"CPUS={cpus}: timeout waiting for {marker!r}")
            if select.select([process.stdout], [], [], 0.2)[0]:
                data = process.stdout.read(4096)
                if not data:
                    raise RuntimeError(f"CPUS={cpus}: QEMU exited unexpectedly")
                output.extend(data)
                if b"panic:" in output[start:] or b"FAILED" in output[start:]:
                    raise RuntimeError(output[start:].decode(errors="replace"))

    try:
        until(b"$ ", 0)
        suites = [
            (b"spectest_fs", b"spectest_fs: ALL TESTS PASSED"),
            (b"speccommit", b"spectest_commit: ALL TESTS PASSED"),
            (b"spectest_proc", b"spectest_proc: ALL TESTS PASSED"),
        ]
        if quick_usertests:
            suites.append((b"usertests -q", b"ALL TESTS PASSED"))
        for program, marker in suites:
            start = len(output)
            process.stdin.write(program + b"\n")
            process.stdin.flush()
            until(marker, start, 600 if program == b"usertests -q" else 180)
            until(b"$ ", start)
            print(f"PASS CPUS={cpus}: {program.decode()}", flush=True)
    finally:
        if process.poll() is None:
            try:
                process.stdin.write(b"\x01x")
                process.stdin.flush()
                process.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()
        output.extend(process.stdout.read())
        log_directory.mkdir(parents=True, exist_ok=True)
        # Normalize QEMU's final carriage return for readable Git text logs.
        transcript = output.replace(b"\r", b"").rstrip() + b"\n"
        (log_directory / f"spec_fs_cpus{cpus}.log").write_bytes(transcript)
        process.stdin.close()
        process.stdout.close()


# Select CPU configurations and optionally include ordinary xv6 quick tests.
# added by Saharsh B24CS1105
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    parser.add_argument("--log-dir", type=Path, default=ROOT / "tests")
    parser.add_argument("--quick-usertests", action="store_true")
    args = parser.parse_args()
    for cpus in args.cpus:
        if cpus < 1:
            parser.error("CPU counts must be positive")
        run(cpus, args.log_dir, args.quick_usertests)


if __name__ == "__main__":
    main()

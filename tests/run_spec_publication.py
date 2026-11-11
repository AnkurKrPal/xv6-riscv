"""R10/R11: force cache exposure windows, check ordering, then reboot the disk.

Build first with:
  make -j4 kernel/kernel kernel/kernel-publication-test fs.img
The normal kernel has no timing hooks. This runner never changes fs.img.
"""

import argparse
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def boot(cpus, image, kernel, visibility, log_directory):
    transcript = bytearray()
    command = [
        "qemu-system-riscv64", "-machine", "virt", "-bios", "none",
        "-kernel", str(kernel), "-m", "128M", "-smp", str(cpus),
        "-nographic", "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={image},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
    ]
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               bufsize=0)

    def send(data):
        process.stdin.write(data)
        process.stdin.flush()

    def until(marker, start=0):
        deadline = time.monotonic() + 60
        while marker not in transcript[start:]:
            if time.monotonic() >= deadline:
                raise RuntimeError(f"CPUS={cpus}: timeout waiting for {marker!r}")
            if select.select([process.stdout], [], [], 0.2)[0]:
                data = process.stdout.read(4096)
                if not data:
                    raise RuntimeError("QEMU exited unexpectedly")
                transcript.extend(data)
                if b"panic:" in transcript or b"FAILED" in transcript:
                    raise RuntimeError(transcript.decode(errors="replace"))

    try:
        until(b"$ ")
        start = len(transcript)
        if visibility:
            send(b"speccommit visibility\n")
            until(b"publication-test: CACHE_PARTIAL\n", start)
            # This console handshake releases two readers whose descriptors
            # were opened before xbegin(), bypassing open()'s begin_op gate.
            send(b"G\n")
            until(b"pub: VISIBILITY PASSED\n", start)
            until(b"$ ", start)
            events = transcript[start:]
            durable = events.index(b"publication-test: DURABLE\n")
            for event in (b"READ_RETURNED", b"STAT_RETURNED", b"COMMIT_RETURNED",
                          b"OUTPUT_RECEIVED"):
                if events.index(b"pub: " + event) <= durable:
                    raise RuntimeError(f"{event!r} escaped before durable commit")
            if events[:durable].count(b"publication-test: READER_WAIT\n") < 2:
                raise RuntimeError("both outside readers must attempt admission before commit")
            if events.count(b"publication-test: CACHE_PARTIAL\n") != 1:
                raise RuntimeError("expected exactly one speculative transaction")
            print(f"PASS CPUS={cpus}: readers, output and success follow durable commit", flush=True)
        else:
            send(b"speccommit verify\n")
            until(b"pub: REBOOT VERIFIED\n", start)
            until(b"$ ", start)
            print(f"PASS CPUS={cpus}: data, inode size and created file survive reboot", flush=True)
    finally:
        if process.poll() is None:
            try:
                send(b"\x01x")
                process.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()
        transcript.extend(process.stdout.read())
        suffix = "visibility" if visibility else "reboot"
        log_directory.mkdir(parents=True, exist_ok=True)
        (log_directory / f"spec_publication_{suffix}_cpus{cpus}.log").write_bytes(
            transcript.replace(b"\r", b"").rstrip() + b"\n")
        process.stdin.close()
        process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    parser.add_argument("--test-kernel", type=Path,
                        default=ROOT / "kernel/kernel-publication-test")
    parser.add_argument("--log-dir", type=Path, default=ROOT / "tests")
    args = parser.parse_args()
    for cpus in args.cpus:
        with tempfile.TemporaryDirectory(prefix="xv6-publication-") as directory:
            image = Path(directory) / "fs.img"
            shutil.copyfile(ROOT / "fs.img", image)
            boot(cpus, image, args.test_kernel, True, args.log_dir)
            boot(cpus, image, ROOT / "kernel/kernel", False, args.log_dir)


if __name__ == "__main__":
    main()

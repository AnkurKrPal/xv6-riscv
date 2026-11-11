"""Shared disposable QEMU process and GNU GDB helpers for P3 regressions."""
from pathlib import Path
import select
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


class Guest:
    def __init__(self, kernel, image, cpus, *, debug=False):
        self.output = bytearray()
        self.port = None
        command = [
            'qemu-system-riscv64', '-machine', 'virt', '-bios', 'none',
            '-kernel', str(kernel), '-m', '128M', '-smp', str(cpus), '-nographic',
            '-global', 'virtio-mmio.force-legacy=false',
            '-drive', f'file={image},if=none,format=raw,id=x0',
            '-device', 'virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0',
        ]
        if debug:
            with socket.socket() as address:
                address.bind(('127.0.0.1', 0))
                self.port = address.getsockname()[1]
            command += ['-S', '-gdb', f'tcp:127.0.0.1:{self.port}']
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, bufsize=0)

    def send(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def until(self, marker, start=0, timeout=60):
        deadline = time.monotonic() + timeout
        while marker not in self.output[start:]:
            if time.monotonic() >= deadline:
                raise RuntimeError(f'timeout waiting for {marker!r}:\n'
                                   + self.output[start:].decode(errors='replace'))
            if select.select([self.process.stdout], [], [], 0.2)[0]:
                data = self.process.stdout.read(4096)
                if not data:
                    raise RuntimeError('QEMU exited: ' + self.output.decode(errors='replace'))
                self.output.extend(data)
                if b'panic:' in self.output[start:] or b'FAILED' in self.output[start:]:
                    raise RuntimeError(self.output[start:].decode(errors='replace'))
        return bytes(self.output[start:])

    def close(self, *, crash=False):
        if self.process.poll() is None:
            if crash:
                self.process.kill() # Abrupt process failure; never guest shutdown.
            else:
                try:
                    self.send(b'\x01x')
                except BrokenPipeError:
                    pass
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.output.extend(self.process.stdout.read())
        self.process.stdin.close()
        self.process.stdout.close()

    def save(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.output.replace(b'\r', b'').rstrip() + b'\n')


def gdb_command(gdb, kernel, port, commands):
    result = [str(gdb), '-nx', '-batch', str(kernel),
              '-ex', 'set pagination off', '-ex', 'set confirm off',
              '-ex', 'set auto-load off', '-ex', 'set remotetimeout 10',
              '-ex', f'target remote 127.0.0.1:{port}']
    for command in commands:
        result += ['-ex', command]
    return result


def debugger(gdb, kernel, port, commands, log):
    result = subprocess.run(gdb_command(gdb, kernel, port, commands),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=45, cwd=kernel.parent.parent)
    log.write_bytes(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(result.stdout.decode(errors='replace'))
    return result.stdout

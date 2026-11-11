"""SIGKILL QEMU at nine commit boundaries; verify old/new state after replay.

Requires make kernel/kernel-crash-test fs.img and a RISC-V-capable GNU GDB.
No repository disk is modified. This models process failure with completed
block writes, not torn sectors or host power loss.
"""
import argparse
from pathlib import Path
import shutil
import struct
import tempfile

from spec_test_support import Guest, ROOT, debugger

PHASES = {
    1: 'reserved', 2: 'checked', 3: 'cache-partial', 4: 'log-partial',
    5: 'log-ready', 6: 'header-committed', 7: 'install-partial',
    8: 'header-cleared', 9: 'before-flush',
}


def header_count(image):
    with image.open('rb') as disk:
        disk.seek(1024)
        sb = struct.unpack('<8I', disk.read(32))
        disk.seek(sb[5] * 1024)
        return struct.unpack('<I', disk.read(4))[0]


def run(gdb, cpus, phase, logs):
    name = f'crash_cpus{cpus}_{phase}_{PHASES[phase]}'
    kernel = ROOT / 'kernel/kernel-crash-test'
    with tempfile.TemporaryDirectory(prefix='xv6-crash-') as directory:
        image = Path(directory) / 'fs.img'
        shutil.copyfile(ROOT / 'fs.img', image)
        guest = Guest(kernel, image, cpus, debug=True)
        try:
            debugger(gdb, kernel, guest.port,
                     [f'set variable spec_crash_target = {phase}', 'detach'],
                     logs / f'{name}_gdb.log')
            guest.until(b'$ ')
            guest.send(b'speccommit crash\n')
            guest.until(f'crash-test: STOP phase={phase} '.encode())
            if b'crash: OUTPUT RELEASED' in guest.output or b'crash: COMMIT RETURNED' in guest.output:
                raise RuntimeError('output or success escaped before crash checkpoint')
        finally:
            guest.close(crash=True)
            guest.save(logs / f'{name}_before.log')
        before = header_count(image)
        if (before > 0) != (phase in (6, 7)):
            raise RuntimeError(f'unexpected durable header: phase={phase} count={before}')
        recovered = Guest(ROOT / 'kernel/kernel', image, cpus)
        try:
            recovered.until(b'$ ')
            replayed = b'recovering tail' in recovered.output
            if replayed != (phase in (6, 7)):
                raise RuntimeError(f'unexpected replay: phase={phase} replayed={replayed}')
            start = len(recovered.output)
            mode = b'recover-new' if phase >= 6 else b'recover-old'
            recovered.send(b'speccommit ' + mode + b'\n')
            recovered.until(b'crash: RECOVERY VERIFIED\n', start)
            recovered.until(b'$ ', start)
        finally:
            recovered.close()
            recovered.save(logs / f'{name}_after.log')
        if header_count(image) != 0:
            raise RuntimeError('recovered journal header was not cleared')
        result = f'PASS CPUS={cpus} phase={phase} {PHASES[phase]}: {mode.decode()}, header={before}->0, replay={replayed}'
        print(result, flush=True)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=Path, required=True)
    parser.add_argument('--cpus', nargs='+', type=int, default=[1, 3])
    parser.add_argument('--phases', nargs='+', type=int, choices=PHASES, default=list(PHASES))
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'tests/crash')
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for cpus in args.cpus:
        for phase in args.phases:
            results.append(run(args.gdb, cpus, phase, args.log_dir))
    (args.log_dir / 'summary.log').write_text('\n'.join(results) + '\n')


if __name__ == '__main__':
    main()

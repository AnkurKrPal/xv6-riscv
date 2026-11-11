"""Capture a genuine historical xv6 failure and verify its current fix in GDB.

Legacy kernel is an untouched build of git revision c6d4dd9. The current test
binary runs from a copied current fs.img on both kernels. No defect is injected.
"""
import argparse
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time

from spec_test_support import Guest, ROOT, gdb_command


def session(gdb, kernel, cpus, logs, legacy):
    label = 'before' if legacy else 'after'
    with tempfile.TemporaryDirectory(prefix='xv6-gdb-disk-') as directory:
        image = Path(directory) / 'fs.img'
        shutil.copyfile(ROOT / 'fs.img', image)
        guest = Guest(kernel, image, cpus, debug=True)
        process = None
        try:
            # Resume the boot first; install workload breakpoints at the shell.
            from spec_test_support import debugger
            debugger(gdb, kernel, guest.port, ['detach'], logs / f'gdb_{label}_boot.log')
            guest.until(b'$ ')
            if legacy:
                pinned = ' + '.join(f'(bcache.buf[{i}].refcnt > 0)' for i in range(30))
                commands = [
                    'break panic', 'echo DEBUGGER_READY\\n', 'continue',
                    'echo HISTORICAL FAILURE: original kernel, no fault injection\\n',
                    'print s', 'bt', 'print log.lh.n', 'print log.outstanding',
                    'print log.spec_committing',
                    'print sizeof(bcache.buf) / sizeof(bcache.buf[0])',
                    f'set $pinned = {pinned}', 'print $pinned', 'detach',
                ]
            else:
                source = (ROOT / 'kernel/spec_commit.c').read_text().splitlines()
                line = next(i for i, text in enumerate(source, 1)
                            if 'if (count < 0 || count > LOGBLOCKS' in text)
                commands = [
                    f'break {ROOT / "kernel/spec_commit.c"}:{line} if count >= 30',
                    'echo DEBUGGER_READY\\n', 'continue',
                    'echo FIX: reject before cache or journal mutation\\n',
                    'print count', 'print log.lh.n', 'print log.spec_committing',
                    'bt', 'finish', 'print (int)$a0', 'detach',
                ]
            (logs / f'gdb_{label}.commands').write_text('\n'.join(commands) + '\n')
            log = logs / f'gdb_{label}.log'
            with log.open('wb', buffering=0) as output:
                process = subprocess.Popen(gdb_command(gdb, kernel, guest.port,
                    ['set trace-commands on'] + commands), stdout=output,
                    stderr=subprocess.STDOUT, cwd=kernel.parent.parent)
                deadline = time.monotonic() + 60
                while b'DEBUGGER_READY' not in log.read_bytes():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(log.read_text(errors='replace'))
                    time.sleep(0.05)
                guest.send(b'speccommit capacity-gdb\n')
                try:
                    process.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    process.kill()
                    raise RuntimeError('GDB failed to reach expected breakpoint: ' + log.read_text(errors='replace'))
            evidence = log.read_text(errors='replace')
            if process.returncode != 0 or 'Breakpoint' not in evidence:
                raise RuntimeError(evidence)
            if legacy:
                if 'bget: no buffers' not in evidence or 'write_log' not in evidence:
                    raise RuntimeError('historical failure did not reproduce: ' + evidence)
            else:
                if 'SPEC_ERR_NO_SPACE' not in evidence and '= -6' not in evidence:
                    # RISC-V returns signed -6 in the unsigned a0 register.
                    raise RuntimeError('missing rejection return value: ' + evidence)
                start = guest.output.index(b'$ ')
                guest.until(b'gdb: CAPACITY FIX VERIFIED\n', start)
                guest.until(b'$ ', guest.output.index(b'gdb: CAPACITY FIX VERIFIED'))
            print(f'PASS: GDB {label}, CPUS={cpus}', flush=True)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            guest.close(crash=legacy)
            guest.save(logs / f'gdb_{label}_guest.log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=Path, required=True)
    parser.add_argument('--legacy-kernel', type=Path)
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'tests/gdb')
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    if args.legacy_kernel:
        session(args.gdb, args.legacy_kernel.resolve(), 1, args.log_dir, True)
    else:
        with tempfile.TemporaryDirectory(prefix='xv6-historical-c6d4dd9-') as directory:
            historical = Path(directory)
            archive = subprocess.check_output(['git', 'archive', 'c6d4dd9'], cwd=ROOT)
            with tarfile.open(fileobj=io.BytesIO(archive)) as source:
                source.extractall(historical, filter='data')
            with (args.log_dir / 'historical_build.log').open('wb') as log:
                subprocess.run(['make', '-j4', 'kernel/kernel'], cwd=historical,
                               stdout=log, stderr=subprocess.STDOUT, check=True)
            session(args.gdb, historical / 'kernel/kernel', 1, args.log_dir, True)
    session(args.gdb, ROOT / 'kernel/kernel', 1, args.log_dir, False)


if __name__ == '__main__':
    main()

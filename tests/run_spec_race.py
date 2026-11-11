"""R12: attempt a real outside write between conflict check and publication."""
import argparse
import re
from pathlib import Path
import shutil
import tempfile

from spec_test_support import Guest, ROOT, debugger


def run(gdb, cpus, logs):
    kernel = ROOT / 'kernel/kernel-crash-test'
    with tempfile.TemporaryDirectory(prefix='xv6-race-') as directory:
        image = Path(directory) / 'fs.img'
        shutil.copyfile(ROOT / 'fs.img', image)
        guest = Guest(kernel, image, cpus, debug=True)
        try:
            debugger(gdb, kernel, guest.port,
                     ['set variable spec_crash_target = 2', 'detach'],
                     logs / f'race_cpus{cpus}_setup_gdb.log')
            guest.until(b'$ ')
            guest.send(b'speccommit race\n')
            guest.until(b'crash-test: STOP phase=2 ')
            guest.send(b'G\n')
            guest.until(b'race: WRITER ATTEMPT\n')
            guest.until(b'crash-test: JOURNAL_WAIT\n')
            # Inspect the actual reservation, then unpark the committer. The
            # ordinary writer cannot modify the filesystem while it is parked.
            inspection = debugger(gdb, kernel, guest.port, [
                'printf "owner=%d outstanding=%d journal=%d readers=%d\\n", log.spec_owner, log.outstanding, log.lh.n, log.readers',
                'set variable spec_crash_target = 0', 'detach',
            ], logs / f'race_cpus{cpus}_reservation_gdb.log')
            if not re.search(rb'owner=[1-9][0-9]* outstanding=1 journal=0 readers=0', inspection):
                raise RuntimeError(inspection.decode(errors='replace'))
            if b'race: WRITER FINISHED' in guest.output or b'race: BUFFERED OUTPUT' in guest.output:
                raise RuntimeError('a writer or output escaped while reservation was parked')
            guest.until(b'race: CHECK PUBLISH VERIFIED\n')
            guest.until(b'$ ', guest.output.index(b'race: CHECK PUBLISH VERIFIED'))
            for event in (b'WRITER FINISHED', b'BUFFERED OUTPUT', b'COMMIT RETURNED'):
                if b'race: ' + event not in guest.output:
                    raise RuntimeError(f'missing {event!r}')
            print(f'PASS CPUS={cpus}: outside writer excluded after conflict check; output released after commit', flush=True)
        finally:
            guest.close()
            guest.save(logs / f'race_cpus{cpus}.log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=Path, required=True)
    parser.add_argument('--cpus', nargs='+', type=int, default=[1, 3])
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'tests/race')
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    for cpus in args.cpus:
        run(args.gdb, cpus, args.log_dir)


if __name__ == '__main__':
    main()

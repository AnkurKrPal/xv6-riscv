"""Run the R12 race proof and R14 crash matrix plus actual GDB before/after."""
import argparse
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=Path, required=True)
    parser.add_argument('--suites', nargs='+', choices=['crash', 'race', 'gdb'],
                        default=['crash', 'race', 'gdb'])
    parser.add_argument('--cpus', nargs='+', type=int, default=[1, 3])
    parser.add_argument('--phases', nargs='+', type=int)
    parser.add_argument('--legacy-kernel', type=Path)
    args = parser.parse_args()
    for suite in args.suites:
        program = 'run_spec_crash.py' if suite == 'crash' else 'run_spec_race.py' if suite == 'race' else 'run_spec_gdb.py'
        command = [sys.executable, str(HERE / program), '--gdb', str(args.gdb)]
        if suite in ('crash', 'race'):
            command += ['--cpus'] + [str(cpu) for cpu in args.cpus]
        if suite == 'crash' and args.phases:
            command += ['--phases'] + [str(phase) for phase in args.phases]
        if suite == 'gdb' and args.legacy_kernel:
            command += ['--legacy-kernel', str(args.legacy_kernel)]
        subprocess.run(command, check=True)


if __name__ == '__main__':
    main()

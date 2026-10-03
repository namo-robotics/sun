"""Exercise the public GPU API through JIT and native compilation."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    """Run the Sun integration program, distinguishing absent hardware from failure."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--sun', required=True)
    parser.add_argument('--bundle-dir', required=True)
    parser.add_argument('--cuda-lib', required=True)
    parser.add_argument('--require-gpu', default='OFF')
    parser.add_argument('--backend-library', default=None)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    (root / 'tmp').mkdir(exist_ok=True)
    env = os.environ.copy()
    env['SUN_PATH'] = str(root)
    env['LD_LIBRARY_PATH'] = args.cuda_lib + ':' + env.get('LD_LIBRARY_PATH', '')
    backend = ['-l' + args.backend_library] if args.backend_library else ['-lcublas', '-lcudart']
    common = [args.sun, '--lib-path', args.bundle_dir, '-L', args.cuda_lib] + backend + ['-lpthread']
    source = str(root / 'cuda' / 'gpu_checks.sun')
    with tempfile.TemporaryDirectory(prefix='cuda-check-', dir=root / 'tmp') as directory:
        # Bind the test to this build's exact bundles, not a neighboring build.
        bundles = ', '.join(json.dumps(str(Path(args.bundle_dir) / name))
                            for name in ('stdlib.moon', 'cuda.moon'))
        invalid = Path(directory) / 'invalid.sun'
        invalid.write_text('''manifest { libraries: ["stdlib.moon", "cuda.moon"] }
/** Rejects integer device storage at compile time. */
function invalid(value: const ref cuda.DeviceMatrix<i32>) i64 { return value.size(); }
/** Provides a compilation entry point. */
function main() i32 { return 0; }
''')
        invalid.write_text(invalid.read_text().replace('"stdlib.moon", "cuda.moon"', bundles))
        rejected = subprocess.run(common + ['-c', '--dynamic', '-o', str(Path(directory) / 'invalid'), str(invalid)],
                                  env=env, text=True, capture_output=True)
        if rejected.returncode == 0 or '_Float' not in rejected.stdout + rejected.stderr:
            raise RuntimeError('Integer device storage was not rejected by the floating-point constraint: ' + rejected.stdout + rejected.stderr)
        binary = str(Path(directory) / 'gpu_checks')
        source_copy = Path(directory) / 'gpu_checks.sun'
        source_copy.write_text(Path(source).read_text().replace('"stdlib.moon", "cuda.moon"', bundles))
        source = str(source_copy)
        subprocess.run(common + ['-c', '--dynamic', '-o', binary, source], env=env, check=True)
        run = subprocess.run([binary], env=env)
        if run.returncode == 77:
            print('No usable CUDA device; hardware validation was not performed.', flush=True)
            return 1 if args.require_gpu == 'ON' else 77
        if run.returncode:
            return run.returncode
        subprocess.run(common + [source], env=env, check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

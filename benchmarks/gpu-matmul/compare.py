"""Run the Sun matrix benchmark's workload with PyTorch or CuPy."""

import argparse
import os
from time import perf_counter_ns


class Backend:
    """Adapt framework operations to the same synchronous matrix workload."""

    def __init__(self, framework):
        """Initialize device zero and full float32 precision before timing."""
        self.framework = framework
        # Set before either framework initializes CUDA; do not allow forced TF32.
        os.environ['NVIDIA_TF32_OVERRIDE'] = '0'
        os.environ['CUPY_TF32'] = '0'
        if framework == 'pytorch':
            import torch

            self.xp = torch
            torch.cuda.set_device(0)
            torch.set_float32_matmul_precision('highest')
            self.host = torch.ones((512, 512), dtype=torch.float32)
            print(f'PyTorch {torch.__version__}; {torch.cuda.get_device_name(0)}')
        else:
            import cupy
            import numpy

            self.xp = cupy
            cupy.cuda.Device(0).use()
            self.host = numpy.ones((512, 512), dtype=numpy.float32)
            name = cupy.cuda.runtime.getDeviceProperties(0)['name']
            if isinstance(name, bytes):
                name = name.decode()
            print(f'CuPy {cupy.__version__}; {name}')
        self.synchronize()

    def synchronize(self):
        """Wait for the current stream so timings include completion."""
        if self.framework == 'cupy':
            self.xp.cuda.get_current_stream().synchronize()
        else:
            self.xp.cuda.current_stream().synchronize()

    def upload(self):
        """Allocate a device matrix and copy the existing host input."""
        if self.framework == 'pytorch':
            result = self.host.to('cuda')
        else:
            result = self.xp.asarray(self.host)
        self.synchronize()
        return result

    def multiply(self, a, b, output=None):
        """Multiply, optionally reusing output storage, and wait for completion."""
        if self.framework == 'pytorch':
            result = self.xp.mm(a, b, out=output)
        else:
            result = self.xp.matmul(a, b, out=output)
        self.synchronize()
        return result

    def download(self, matrix):
        """Copy into existing host storage, matching Sun's download operation."""
        if self.framework == 'pytorch':
            self.host.copy_(matrix)
        else:
            matrix.get(out=self.host)
        self.synchronize()

    def validate(self):
        """Check every result element outside the timed regions."""
        if not bool((self.host == 512).all()):
            raise RuntimeError('Incorrect matrix product: expected every element to be 512')


def benchmark(backend):
    """Measure transfers and ten products per loop, as in main.sun."""
    start = perf_counter_ns()
    a = backend.upload()
    b = backend.upload()
    upload_us = (perf_counter_ns() - start) / 1000

    c = backend.multiply(a, b)
    start = perf_counter_ns()
    for _ in range(10):
        product = backend.multiply(a, b)
        # Release each temporary within the measured loop, as Sun does.
        del product
    allocating_us = (perf_counter_ns() - start) / 1000

    start = perf_counter_ns()
    for _ in range(10):
        backend.multiply(a, b, c)
    reuse_us = (perf_counter_ns() - start) / 1000

    start = perf_counter_ns()
    backend.download(c)
    download_us = (perf_counter_ns() - start) / 1000
    backend.validate()

    print('512 x 512 float32; 10 products per arithmetic loop; device 0')
    print(f'Upload with allocation (microseconds): {upload_us:.3f}')
    print(f'Allocating products (microseconds): {allocating_us:.3f}')
    print(f'Products reusing output (microseconds): {reuse_us:.3f}')
    print(f'Download (microseconds): {download_us:.3f}')
    print(f'First result element: {float(backend.host[0, 0])}')


def main():
    """Select one optional framework without requiring the other to be installed."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('framework', choices=('pytorch', 'cupy'))
    args = parser.parse_args()
    try:
        benchmark(Backend(args.framework))
    except ImportError as error:
        parser.exit(1, f'Missing comparison dependency: {error}. See README.md.\n')


if __name__ == '__main__':
    main()

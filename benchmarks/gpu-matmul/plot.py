#!/usr/bin/env python3
"""Chart completed GPU benchmark logs without importing CUDA frameworks."""

import argparse
import math
from pathlib import Path
import re
import sys

METRICS = (
    ('Upload with allocation', 'Upload two input matrices'),
    ('Allocating products', 'Allocating products — total for 10'),
    ('Products reusing output', 'Reusable output — total for 10'),
    ('Download', 'Download one result matrix'),
)
BACKENDS = (('sun', 'Sun', '#e69b18'), ('pytorch', 'PyTorch', '#4179a5'),
            ('cupy', 'CuPy', '#39856a'))


def read_result(path):
    """Read one complete, validated run, rejecting ambiguous or invalid timings."""
    output = path.read_text()
    values = []
    for metric, _ in METRICS:
        matches = re.findall(
            rf'^{re.escape(metric)} \(microseconds\):\s*([^\s]+)\s*$',
            output, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError(f'expected one timing for {metric}')
        value = float(matches[0])
        if not math.isfinite(value) or value < 0:
            raise ValueError(f'invalid timing for {metric}')
        values.append(value)
    checks = re.findall(r'^First result element:\s*([^\s]+)\s*$', output, re.MULTILINE)
    if len(checks) != 1 or float(checks[0]) != 512:
        raise ValueError('missing successful result validation')
    return values


def plot_results(rows, output):
    """Save independent, zero-based timing panels with explicit workload units."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 2, figsize=(11, 8), layout='constrained')
    fig.suptitle('GPU matrix multiplication · 512 × 512 float32 · device 0\n'
                 'Synchronous host timings; lower is faster', fontsize=16)
    for index, ax in enumerate(axes.flat):
        values = [row[2][index] for row in rows]
        bars = ax.bar([row[0] for row in rows], values,
                      color=[row[1] for row in rows], width=0.6)
        ax.bar_label(bars, fmt='%.3f', padding=4)
        ax.set_ylim(0, max(max(values) * 1.2, 1))
        ax.set_title(METRICS[index][1], pad=12)
        ax.set_ylabel('Microseconds')
        ax.spines[['top', 'right']].set_visible(False)
        ax.set_axisbelow(True)
        ax.yaxis.grid(True, alpha=0.2)
    fig.supxlabel('Panels use independent scales. Allocation and framework memory pools affect timings.\n'
                  'Only completed runs are shown; see accompanying logs for hardware and versions.',
                  fontsize=10)
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=180, bbox_inches='tight')
    plt.close(fig)


def main():
    """Plot available benchmark logs and report missing or incomplete backends."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, help='directory containing benchmark-{sun,pytorch,cupy}.log')
    parser.add_argument('output', type=Path, help='chart output path, such as gpu-matmul.png')
    args = parser.parse_args()
    rows = []
    for backend, label, color in BACKENDS:
        path = args.logs / f'benchmark-{backend}.log'
        try:
            values = read_result(path)
        except (OSError, ValueError) as error:
            print(f'Skipping {label}: {error}', file=sys.stderr)
            continue
        rows.append((label, color, values))
    if not rows:
        parser.exit(1, 'No completed GPU benchmark results to plot.\n')
    plot_results(rows, args.output)
    print(f'Saved {args.output}')


if __name__ == '__main__':
    main()

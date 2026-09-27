#!/usr/bin/env python3
"""Plot measured string-creation latency from the comparison runner's JSON."""

import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    """Save a standalone chart, highlighting Sun among the measured languages."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = json.loads(args.results.read_text())
    rows = sorted(data["results"], key=lambda row: row["ns_per_string"])
    labels = [row["language"] for row in rows]
    values = [row["ns_per_string"] for row in rows]
    colors = ["#e69b18" if label.startswith("Sun") else "#4179a5" for label in labels]
    fig, ax = plt.subplots(figsize=(12, 7), layout="constrained")
    bars = ax.bar(labels, values, color=colors, width=0.67)
    ax.bar_label(bars, fmt="%.2f", padding=4, fontsize=10)
    ax.set_ylim(0, max(values) * 1.15)
    ax.set_ylabel("Nanoseconds per new decimal string (lower is faster)")
    ax.tick_params(axis="x", labelrotation=30)
    plt.setp(ax.get_xticklabels(), ha="right", rotation_mode="anchor")
    ax.set_title("Integer-to-string creation, including Sun", loc="left", fontsize=17, pad=34)
    ax.text(0, 1.025, f"{data['cpu_model']} · Linux · CPU {data['cpu']} · best of five",
            transform=ax.transAxes, fontsize=10, color="#555555")
    ax.spines[["top", "right"]].set_visible(False)
    ax.tick_params(axis="x", length=0)
    ax.set_axisbelow(True)
    ax.yaxis.grid(True, alpha=0.2)
    fig.text(0.02, -0.10,
             "1,024 retained strings; 100 million conversions per run (Python: 10 million).\n"
             "All results measured on the same machine. Adapted from Daniel Lemire’s September 25, 2026 benchmark.",
             fontsize=9, color="#555555")
    fig.savefig(args.output, dpi=180, bbox_inches="tight")
    plt.close(fig)


if __name__ == "__main__":
    main()

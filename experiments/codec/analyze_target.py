#!/usr/bin/env python3
"""Summarize compiler stack frames/call paths; never a hardware high-water mark."""
import argparse
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    args = parser.parse_args()
    frames, labels, graph = {}, {}, {}
    for file in sorted(args.build_directory.glob("*.ci")):
        for line in file.read_text().splitlines():
            match = re.search(r'node: \{ title: "([^"]+)" label: "(.*)"', line)
            if match:
                key, label = match.groups()
                labels[key] = label.split(r"\n")[0]
                size = re.search(r"\\n(\d+) bytes \(([^)]+)\)", label)
                if size:
                    frames[key] = int(size.group(1))
            match = re.search(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', line)
            if match:
                source, target = match.groups()
                graph.setdefault(source, set()).add(target)

    def longest(node, trail):
        if node in trail:
            return 0, [], {"cycle:" + node}
        best_size, best_path, issues = 0, [], set()
        if node not in frames:
            issues.add("unknown-frame:" + node)
        for child in sorted(graph.get(node, set())):
            size, path, child_issues = longest(child, trail + (node,))
            issues |= child_issues
            if size > best_size:
                best_size, best_path = size, path
        return frames.get(node, 0) + best_size, [node] + best_path, issues

    roots = {}
    for root in ("dm32_imbe_encode", "dm32_imbe_decode"):
        total, path, issues = longest(root, ())
        roots[root] = {
            "known_frame_sum_along_longest_simple_path": total,
            "path": [{"function": labels.get(name, name), "frame_bytes": frames.get(name)} for name in path],
            "issues": sorted(issues),
        }
    report = {
        "method": "GCC -fstack-usage/-fcallgraph-info=su, known frame sum along longest simple call path. Cycles are cut on repetition. This is an estimate, not a whole-program bound or stack high-water measurement.",
        "limitations": "Interrupt nesting, scheduler, startup, external memset frame and any unresolved call edges require a final linked-image audit and on-target stack painting. Shift helper mutual call edges are input-sign dependent; actual recursion is bounded by the source's sign normalization.",
        "roots": roots,
        "largest_frames": [{"function": labels[name], "frame_bytes": size} for name, size in sorted(frames.items(), key=lambda x: x[1], reverse=True)[:15]],
    }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

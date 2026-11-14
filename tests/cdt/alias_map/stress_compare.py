#!/usr/bin/env python3
"""Compare two AliasMap stress reports.

Usage:
    stress_compare.py BASELINE.json CANDIDATE.json [--threshold PERCENT] [--json]

Exit status:
    0  within tolerance and no errors
    1  regression beyond tolerance, or a non-zero error count
    2  bad usage or malformed report
"""

import argparse
import json
import sys

THROUGHPUT_METRICS = (
    ("ops_per_s", "throughput"),
    ("adds_per_s", "adds/s"),
    ("gets_per_s", "gets/s"),
    ("removes_per_s", "removes/s"),
)

LATENCY_METRICS = (
    ("p50_ns", "p50"),
    ("p90_ns", "p90"),
    ("p99_ns", "p99"),
    ("p999_ns", "p99.9"),
    ("max_ns", "max"),
)


def load(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            report = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"stress_compare: cannot read {path}: {exc}", file=sys.stderr)
        raise SystemExit(2)

    if report.get("schema") != 1:
        print(f"stress_compare: {path}: unsupported schema {report.get('schema')!r}",
              file=sys.stderr)
        raise SystemExit(2)

    return report


def change(before, after):
    if before == 0:
        return None
    return (after - before) / before * 100.0


def main():
    parser = argparse.ArgumentParser(description="Compare two alias map stress reports")
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--threshold", type=float, default=10.0,
                        help="percent beyond which a change is a regression (default 10)")
    parser.add_argument("--json", action="store_true", help="emit a JSON summary")
    args = parser.parse_args()

    baseline = load(args.baseline)
    candidate = load(args.candidate)

    rows = []
    regressions = []

    for key, name in THROUGHPUT_METRICS:
        before = baseline["results"].get(key, 0)
        after = candidate["results"].get(key, 0)
        delta = change(before, after)
        rows.append((name, before, after, delta))

        if delta is not None and delta < -args.threshold:
            regressions.append(f"{name} {delta:+.1f}%")

    for key, name in LATENCY_METRICS:
        before = baseline["results"]["latency"].get(key, 0)
        after = candidate["results"]["latency"].get(key, 0)
        delta = change(before, after)
        rows.append((name + " (ns)", before, after, delta))

        if delta is not None and delta > args.threshold:
            regressions.append(f"{name} {delta:+.1f}%")

    errors = candidate["results"].get("errors", 0)

    if args.json:
        print(json.dumps({
            "baseline": args.baseline,
            "candidate": args.candidate,
            "threshold_percent": args.threshold,
            "metrics": [
                {"metric": name, "baseline": before, "candidate": after, "change_percent": delta}
                for name, before, after, delta in rows
            ],
            "errors": errors,
            "regressions": regressions,
        }, indent=2))
    else:
        print(f"baseline:  {args.baseline}  ({baseline.get('label', '')})")
        print(f"candidate: {args.candidate}  ({candidate.get('label', '')})")
        print()
        print(f"{'metric':<14}{'baseline':>14}{'candidate':>14}{'change':>12}")
        print("-" * 54)

        for name, before, after, delta in rows:
            shown = "n/a" if delta is None else f"{delta:+.1f}%"
            print(f"{name:<14}{before:>14.0f}{after:>14.0f}{shown:>12}")

        print()
        print(f"errors: {errors} (must be zero)")
        print(f"threshold: ±{args.threshold:.0f}%")

        if regressions:
            print("regressions:")
            for item in regressions:
                print(f"  - {item}")
        else:
            print("within tolerance")

    if errors:
        return 1

    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())

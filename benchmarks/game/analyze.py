#!/usr/bin/env python3

import json
import statistics
import sys
from pathlib import Path


METRICS = (
    "observed_input_records_per_s",
    "expensive_actions_per_s",
    "average_fps",
    "one_percent_low_fps",
    "zero_point_one_percent_low_fps",
    "frametime_p50_ns",
    "frametime_p95_ns",
    "frametime_p99_ns",
    "frametime_p99_9_ns",
    "sample_age_p50_ns",
    "sample_age_p95_ns",
    "sample_age_p99_ns",
    "sample_age_p99_9_ns",
    "process_cpu_ms_per_s",
    "voluntary_context_switches_per_s",
    "involuntary_context_switches_per_s",
)

FINGERPRINT = (
    "requested_rate_hz",
    "objects",
    "reaction_objects",
    "draw_repeats",
    "window_width",
    "window_height",
    "video_driver",
    "gl_renderer",
)


def fail(message: str) -> None:
    raise SystemExit(f"hfior-game-analyze: {message}")


def percent_change(before: float, after: float) -> float:
    if before == 0:
        fail("cannot calculate a change from a zero baseline")
    return (after / before - 1.0) * 100.0


def main() -> None:
    if len(sys.argv) < 3:
        fail("pass at least one eager and one HFIOR summary JSON")

    runs = []
    for argument in sys.argv[1:]:
        path = Path(argument)
        with path.open(encoding="utf-8") as stream:
            run = json.load(stream)
        run["summary_path"] = str(path)
        runs.append(run)

    expected = {key: runs[0].get(key) for key in FINGERPRINT}
    for run in runs[1:]:
        actual = {key: run.get(key) for key in FINGERPRINT}
        if actual != expected:
            fail(f"workload mismatch in {run['summary_path']}")
    if not all(run.get("valid") is True for run in runs):
        fail("at least one run is invalid")

    grouped = {}
    for run in runs:
        grouped.setdefault(run["mode"], []).append(run)
    eager_runs = grouped.get("eager-same-source", [])
    hfior_runs = grouped.get("hfior-late-latch", [])
    if not eager_runs or not hfior_runs:
        fail("both eager-same-source and hfior-late-latch runs are required")

    def summarize(mode_runs):
        summary = {
            "runs": len(mode_runs),
            "all_valid": True,
            "max_producer_drops": max(run["producer_drops"] for run in mode_runs),
            "max_sequence_errors": max(run["sequence_errors"] for run in mode_runs),
        }
        for metric in METRICS:
            values = [run[metric] for run in mode_runs]
            summary[f"median_{metric}"] = statistics.median(values)
            summary[f"min_{metric}"] = min(values)
            summary[f"max_{metric}"] = max(values)
        return summary

    eager = summarize(eager_runs)
    hfior = summarize(hfior_runs)

    comparison = {
        "observed_rate_change_percent": percent_change(
            eager["median_observed_input_records_per_s"],
            hfior["median_observed_input_records_per_s"],
        ),
        "expensive_actions_reduction_percent": -percent_change(
            eager["median_expensive_actions_per_s"],
            hfior["median_expensive_actions_per_s"],
        ),
        "average_fps_change_percent": percent_change(
            eager["median_average_fps"], hfior["median_average_fps"]
        ),
        "one_percent_low_change_percent": percent_change(
            eager["median_one_percent_low_fps"],
            hfior["median_one_percent_low_fps"],
        ),
        "zero_point_one_percent_low_change_percent": percent_change(
            eager["median_zero_point_one_percent_low_fps"],
            hfior["median_zero_point_one_percent_low_fps"],
        ),
        "frametime_p99_reduction_percent": -percent_change(
            eager["median_frametime_p99_ns"], hfior["median_frametime_p99_ns"]
        ),
        "sample_age_p50_reduction_percent": -percent_change(
            eager["median_sample_age_p50_ns"], hfior["median_sample_age_p50_ns"]
        ),
        "sample_age_p99_reduction_percent": -percent_change(
            eager["median_sample_age_p99_ns"], hfior["median_sample_age_p99_ns"]
        ),
    }

    result = {
        "schema": 1,
        "benchmark": "hfior-game",
        "evidence_class": "interactive-physical-candidate",
        "workload": expected,
        "modes": {
            "eager-same-source": eager,
            "hfior-late-latch": hfior,
        },
        "comparison": comparison,
        "source_summaries": [run["summary_path"] for run in runs],
    }
    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()

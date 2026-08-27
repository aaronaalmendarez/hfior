# Benchmark methodology

## Evidence classes

- **Physical:** records originate from the named physical evdev device.
- **Synthetic:** records originate from the harness timer/source.
These classes must never be merged without retaining the label.

## Controlled variables

Compare policies against the same source, duration, frame rate, work interval,
CPU placement, governor, compositor/session, device profile, and motion
scenario. Randomize policy order. Do not selectively repeat only favorable or
unfavorable trials. Record thermal and topology information.

The current physical game campaign requires at least five accepted runs per
mode, alternates policy order, and rejects runs below 5,500 observed reports/s
for the high-rate cohort.

## Measurements kept distinct

- requested device rate and achieved observed reports/s;
- IRQ rate where attribution is possible;
- expensive consumer actions/s and context switches/s;
- producer, ingress, consumer, and transport CPU where separable;
- frame rate and frame-time p50/p95/p99/p99.9/max;
- corrected useful-sample age p50/p95/p99/p99.9/max;
- records published/consumed, drops, sequence gaps, duplicate/reordered records,
  invalid ACKs, and final ring depth.

The primary latency timestamp occurs after the last latch whose records are
included in the measured input state.

## When a run is invalid

Ordinary runs are invalid after any ring/eager drop, sequence gap, duplication,
reordering, invalid ACK, impossible depth, nonzero final depth, or record-count
mismatch. The deliberate tiny-ring overflow test is expected to fail ordinary
integrity and must show both producer drops and consumer-visible gaps.

## Controlled HFIOR trace

The highlighted trace uses a synthetic 8 kHz producer, 240 Hz consumer,
1,700 µs normal work, and a controlled 100 µs final integration interval. It
isolates the mechanism. KVM scheduler behavior and synthetic source timing
prevent it from proving physical end-to-end latency.

## Reproduction commands

```bash
./benchmarks/game/run_ab.sh
```

See [`game/README.md`](game/README.md) for build requirements, workload controls,
acceptance rules, and individual-run commands.

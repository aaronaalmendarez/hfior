# Benchmarking HFIOR

Benchmarks answer two separate questions:

1. Does HFIOR preserve source information and decouple expensive reactions?
2. Is input at least as fresh at the actual useful boundary?

Neither requested polling rate nor a synthetic timer answers the physical
question. Record requested rate, observed evdev reports, IRQs, wakes, expensive
consumer actions, and frames separately.

## Included evidence

| Evidence | Class | Contents |
| --- | --- | --- |
| [`raw/physical-game-8k/`](raw/physical-game-8k/) | Physical input, rendered workload | Five accepted runs per mode with raw per-frame CSV and summaries |
| [`raw/controlled-late-latch/`](raw/controlled-late-latch/) | Synthetic | Four joined-trace policies with manifests and raw CSV/JSON |
| [`processed/hfior/`](processed/hfior/) | Synthetic | Controlled summaries and scaling models |
| [`plots/`](plots/) | Derived | Key plots generated from retained processed data |

The physical game result uses software timestamps at the useful camera boundary.
It is not an optical motion-to-photon result and does not predict the gain for
games with different input-dependent work.

## Run a synthetic smoke benchmark

```bash
./scripts/build.sh
./scripts/benchmark.sh ./benchmark-output/my-run
```

## Run the 3D game workload

The optional [3D game benchmark](game/README.md) compares a same-source
per-record eager consumer against the HFIOR Late-Latch consumer while rendering
the same simulated, culled, GPU-synchronized scene. It retains raw per-frame
results and rejects low observed input rates or HFIOR integrity faults.

## Run the physical campaign

Use [`game/run_ab.sh`](game/run_ab.sh) for the current five-pair physical
campaign. The game must receive an authorized HFIOR stream from the compositor.
Never upload raw traces containing unrelated user input.

## Submit a reproduction

Use the **Benchmark Result** issue template. Attach raw output, configuration,
environment, and integrity result, even when the result is negative. Community
results are not merged into a headline aggregate until their provenance and
methodology are reviewable.

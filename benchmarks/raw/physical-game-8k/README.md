# Physical-input game benchmark

This directory retains the final trajectory-preserving heavy game campaign run
on 2026-08-26.

## Workload

- CPU: AMD Ryzen 9 5900XT
- GPU: NVIDIA GeForce RTX 5070
- Display path: Wayland
- Requested polling rate: 8,000 Hz
- Render size: 1280 x 720
- Scene objects: 65,536
- Camera reaction graph nodes: 8,192
- Geometry passes per frame: 128
- Warmup: 2 seconds
- Measurement: 8 seconds
- Repetitions: 5 per mode with alternating order

Both modes consumed the same authorized physical HFIOR ring. The eager control
ran the camera reaction graph for every ordered record. HFIOR integrated every
ordered record, performed its final suffix latch, and ran that graph once at the
useful frame boundary. Both modes always simulated, culled, uploaded, and
rendered the same complete scene.

## Median result

| Metric | Eager 8K | HFIOR | Change |
| --- | ---: | ---: | ---: |
| Observed records/s | 7,966 | 7,991 | +0.31% |
| Expensive actions/s | 7,966 | 113 | -98.58% |
| Average FPS | 80.2 | 113.1 | +41.05% |
| 1% low FPS | 67.7 | 100.7 | +48.87% |
| 0.1% low FPS | 65.6 | 93.4 | +42.51% |
| Frame-time p99 | 14.52 ms | 9.70 ms | -33.21% |
| Useful-sample age p50 | 4,519 µs | 1,047 µs | -76.82% |
| Useful-sample age p99 | 6,278 µs | 1,893 µs | -69.84% |
| Drops / sequence errors | 0 / 0 | 0 / 0 | none observed |

All ten runs passed the source-rate and integrity checks. One eager run observed
7,569 records/s and remains included because it exceeded the predeclared 5,500
records/s acceptance floor.

`aggregate.json` is the machine-readable aggregate. Each CSV contains raw
per-frame measurements, and each sibling `summary.json` records the complete
run summary. The files retain their original generated output paths even though
they were relocated into this evidence directory for publication.
`MANIFEST.sha256` covers every retained evidence file and this README.

Aggregate SHA-256:

```text
8e62e1e640b8a916fb900f172708568c841599a656971adcb283f63701ea912f
```

This is a controlled, deliberately input-sensitive workload. It does not show
that every game gains 41%, and software useful-sample age is not an optical
motion-to-photon measurement.

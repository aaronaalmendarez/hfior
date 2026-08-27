# Contributing

HFIOR welcomes implementation work, independent reproduction, negative results,
documentation corrections, and design criticism.

## Build and test

```bash
./scripts/build.sh
./scripts/test.sh
./scripts/sanitize.sh
```

Code is C11 with strong GCC/Clang warnings treated as errors. Match surrounding
style, keep patches focused, and explain ownership and memory ordering when
touching shared state. Prefer commits that separate timestamp fidelity,
representation, transport, authorization, consumption, and instrumentation.

## Performance changes

A performance-changing pull request must include:

- before and after runs from the same controlled environment;
- raw evidence and the exact command/configuration;
- CPU, kernel, distribution, compositor, GPU, device, firmware when known,
  requested and observed rate, refresh/consumer rate, policy, and scenario;
- p50, p95, p99, p99.9, and maximum where meaningful; and
- drop, gap, duplication, reorder, and invalid-ACK validation.

Distinguish requested polling rate, observed report rate, IRQ rate, wake rate,
consumer action rate, and frame rate. Label synthetic and physical results.
Attach community reproductions through the Benchmark Result issue template.

## Integrations

Do not present a design sketch as a completed integration. Preserve the target
project's license and contribution rules. Small reviewable patch series are
preferred, with fallback behavior and authorization changes called out.

## Commits and pull requests

Use imperative commit subjects and describe motivation, evidence, correctness,
and rollback. Update relevant tests and docs when semantics change. By
contributing original work, you agree it may be distributed under this
repository's MIT license; third-party material must retain its own notices.

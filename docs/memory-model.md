# Memory model

The reference ring is single-producer and uses monotonic positions.

| Storage | Owner | Access pattern |
| --- | --- | --- |
| `head`, publication counters, generation | Producer | Producer writes; consumers acquire-load |
| Private read cursor | Consumer process | Consumer only; not shared |
| Acknowledged `tail` | Producer updates after validating consumer ACK | Producer/consumer observe |
| Record slot | Producer until publication; immutable afterward | Consumer reads after acquire |

Producer-owned and consumer-related hot fields occupy separate 64-byte cache
lines. Records are 32 bytes, so two fit in a 64-byte cache line.

## Publication

1. The producer acquire-loads the acknowledged tail to test capacity.
2. It writes the complete record into the slot selected by monotonic `head`.
3. It release-stores `head + 1`.
4. A consumer acquire-loads `head`, then reads only slots below that snapshot.

The release/acquire pair makes initialized record contents visible before the
consumer treats the new head as committed. Sequence numbers validate ordering.
A view where `head < local_tail` or `head - local_tail > capacity` is rejected.

## Reclamation

The read-only consumer advances its private cursor immediately, but sends an
ACK after useful work or after the frame. The producer validates generation and
bounds before moving the acknowledged tail. Reclamation is not part of making
the current record useful, which is why HFIOR removed it from the critical
drain path.

“Lock-free” alone would not describe these ownership and ordering rules. The
experimental ABI is specified in [record format](../spec/record-format.md) and
[producer model](../spec/producer-model.md).

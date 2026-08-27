# Consumer model

An authorized consumer owns a private monotonic read cursor. It acquire-loads a
head snapshot and copies records from its cursor up to that head in sequence
order. It rejects impossible depth, sequence, ABI, or generation state.

A full-history consumer integrates every required record exactly once. It may
read once per frame or deadline without causing producer-side coalescing.

A Late-Latch consumer performs normal history integration, then a bounded suffix
read at the latest safe point. It applies only newly committed records and ACKs
reclamation after the useful boundary or frame. A newest-state diagnostic is not
a substitute for full trajectory integration.

On revocation, disconnect, overflow, or generation mismatch, the consumer stops
using the stream and follows the integration's recovery/fallback policy.

# Producer model

The producer owns publication position and source generation.

For each accepted observation it:

1. validates available capacity against the acknowledged tail;
2. creates one timestamped record with the next sequence;
3. writes the selected slot completely;
4. release-publishes the new monotonic head; and
5. updates non-authoritative diagnostics.

If the ring is full, the reference policy drops the newest observation and
increments `dropped_newest`; it never overwrites an unread slot. A reconnect or
source reset advances generation and emits lifecycle state. `SYN_DROPPED` is a
recorded loss condition, not normal motion.

The producer must validate ACK generation and bounds before reclaiming capacity.
Invalid ACKs are counted and rejected.

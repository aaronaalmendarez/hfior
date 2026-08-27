# Record format

The experimental Linux ABI uses a 32-byte `struct hfior_record`:

| Field | Type | Meaning |
| --- | --- | --- |
| `sequence` | `uint64_t` | Monotonic observation identity |
| `timestamp_ns` | `uint64_t` | Source time in the advertised clock domain |
| `dx`, `dy` | `int32_t` | Relative motion |
| `wheel`, `hwheel` | `int16_t` | Scroll displacement |
| `buttons` | `uint16_t` | State bitmap for known buttons |
| `flags` | `uint16_t` | Motion, button, wheel, loss, lifecycle flags |

Two records fit a 64-byte cache line. A producer completely initializes a slot
before release-publishing the new head. A consumer must not infer that adjacent
records have equal spacing, and must validate sequence and generation.

This layout is experimental. Endianness, cross-architecture interoperability,
and extension records require specification before ABI stabilization.

# Protocol design

The reference design has two planes:

1. A Unix-domain control channel negotiates ABI version and mode, transfers a
   read-only memfd plus urgent eventfd, carries lifecycle state, and accepts
   validated reclamation ACKs.
2. A shared ring carries immutable observations without one message or wakeup
   per motion record.

The producer sends metadata containing record size, capacity, clock, feature
bits, device identity, and generation. The consumer rejects unknown ABI values
or incompatible sizes. Positions are monotonic 64-bit values; capacity is a
power of two only to make slot selection cheap, not to define sequence wrap.

Control messages are fixed-size for the current experimental ABI. Production
work must define extension rules, credentials, revocation, object lifetime, and
cross-architecture layout before declaring stability. The experimental Wayland
XML is an integration sketch, not an official protocol.

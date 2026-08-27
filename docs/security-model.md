# Security model

Input history can reveal sensitive behavior. HFIOR must not expose a global,
world-readable stream.

## Authority and eligibility

On Wayland, the compositor remains the authorization authority. A consumer is
eligible only for the correct seat and focused surface, normally while an
explicit pointer-lock or equivalent high-rate-input grant is active. Sandboxed
clients receive descriptors through a brokered protocol, not by opening an
ambient device node.

## Capabilities and lifecycle

- The client mapping is read-only; it cannot forge records or producer state.
- Stream descriptors are scoped capabilities and must not outlive authorization.
- Focus loss, pointer unlock, seat removal, client death, or policy change
  revokes the stream and closes/invalidates descriptors.
- Device reconnect increments a generation. Consumers must reject stale data
  and reauthorize rather than silently joining histories.
- ACKs carry the generation and are accepted only within the published range.
- `SYN_DROPPED`, overflow, disconnect, and reconnect are explicit states.

The current Unix-socket bridge verifies the peer UID, which is appropriate for
experiments but insufficient as the complete production Wayland policy. See
[authorization](../spec/authorization.md) for normative experimental rules.

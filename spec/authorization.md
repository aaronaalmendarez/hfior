# Authorization

An HFIOR stream is a revocable input capability.

The authority must bind a grant to the relevant user/session, seat, focused
surface, and high-rate-input condition such as pointer lock. It must prevent an
unfocused or unrelated client from observing the stream.

Descriptor transfer must authenticate the peer. The client receives no writable
mapping of producer-owned state. Focus loss, unlock, client destruction, seat
change, device removal, or policy revocation invalidates the grant. Reconnect
requires a new generation and, where authority changed, new authorization.

Same-UID checks in the reference bridge are an experimental local boundary, not
a sufficient production Wayland authorization policy.

# Experimental Wayland integration

Status: **EXPERIMENTAL / UNSTABLE / NOT AN OFFICIAL WAYLAND PROTOCOL**.

The XML records the current private Hyprland integration used by the reference
clients. It demonstrates descriptor negotiation for an authorized
high-frequency input stream. A production proposal still needs broader review
of object and seat lifetime, pointer-lock and focus eligibility, revocation,
generation, clock, feature negotiation, sandbox brokering, and error semantics.

The compositor remains the authority. The protocol must not expose a global
seat stream or let a client retain motion after focus or lock is lost. Existing
clients continue through standard relative-pointer and pointer-constraints
protocols.

The current XML is research material and may change incompatibly:
[`experimental-protocol/hyprland-hf-input-v1.xml`](experimental-protocol/hyprland-hf-input-v1.xml).

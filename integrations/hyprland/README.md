# Reference Integration: Hyprland + Aquamarine

Status: **experimental prototype work; no upstream-ready patch is claimed**.

Hyprland is the first compositor target because the research machine uses it,
not because HFIOR is a Hyprland optimization. The compositor is a natural
authorization authority: it knows the active seat, focused surface, pointer-lock
state, and client lifetime.

The intended patch series is split into reviewable concerns:

1. preserve kernel/evdev timestamp fidelity through Aquamarine;
2. represent immutable ordered observations internally;
3. add read-only shared-ring transport and loss accounting;
4. authorize and revoke a stream with focus and pointer lock;
5. keep button transitions on the immediate existing path;
6. consume motion at the latest safe compositor/application boundary; and
7. add instrumentation without changing fallback behavior.

Ordinary clients continue using existing relative-pointer behavior. HFIOR-aware
clients negotiate the unstable optional path and fall back safely on any error.
Sensitivity, acceleration, transforms, constraints, and pointer-lock lifetime
must be applied exactly once at a documented boundary.

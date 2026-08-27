# Linux integration

The reference implementation already uses a userspace evdev producer. A kernel
HFIOR producer is **not automatically required**.

A kernel design should be considered only after profiles show material
avoidable cost remains in HID, evdev, wakeup, copying, or userspace publication
after the compositor/userspace architecture is correct. Any proposal must
preserve Linux input security, device lifetime, `SYN_DROPPED`, namespace and
seat policy, and existing ABI behavior. A global world-readable `/dev/hfior`
raw mouse stream is explicitly outside the security model.

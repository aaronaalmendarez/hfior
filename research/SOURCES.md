# Research sources

This is the audit index for external facts and prior art used by HFIOR. It is
not benchmark evidence. HFIOR performance and integrity claims must cite the
retained data, methodology, and analysis in [`../benchmarks/`](../benchmarks/).

Unless an entry says otherwise, each source was accessed on **2026-08-26**.
Pinned commits identify the inspected text. “Normative” means the source owns
the interface contract; “implementation” means it describes one codebase at
one revision and does not generalize to every Linux or Wayland system.

## Normative and platform contracts

### Linux input protocol and UAPI

- **Source:** Linux kernel [input event-code specification](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/input/event-codes.rst?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f) and [`input_event` UAPI](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/input.h?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** `EV_REL` represents relative change; `EV_KEY` represents
  discrete key/button state; `SYN_REPORT` delimits a logical packet;
  `SYN_DROPPED` reports client-queue overrun and requires explicit state
  recovery; `input_event` carries a timestamp, type, code, and value. These
  contracts motivate preserving ordered records and making loss detectable.

- **Source:** Linux kernel [input driver API](https://docs.kernel.org/7.2/driver-api/input.html)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** `input_set_timestamp()` can preserve a more accurate
  `CLOCK_MONOTONIC` occurrence time acquired by a driver. It does **not** imply
  that every device supplies a hardware timestamp.

### Wayland core and input extensions

- **Source:** Wayland core [`wl_seat` and `wl_pointer` protocol](https://gitlab.freedesktop.org/wayland/wayland/-/blob/87cc8a8728a923fc57938faa81ba0e74f34ecdc7/protocol/wayland.xml)
- **Version:** Wayland 1.26.0, commit `87cc8a8728a923fc57938faa81ba0e74f34ecdc7`
- **Supports:** the compositor establishes pointer focus with enter/leave;
  motion is relative to the focused surface; motion and button notifications
  are distinct; `wl_pointer.frame` groups events that logically belong
  together. This is the compatibility baseline, not an HFIOR authorization
  policy.

- **Source:** wayland-protocols [relative pointer unstable v1](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/ee78491a237eaff9389a0ccf8680521d074407d3/unstable/relative-pointer/relative-pointer-unstable-v1.xml)
- **Version:** wayland-protocols 1.49, commit `ee78491a237eaff9389a0ccf8680521d074407d3`; unstable protocol v1
- **Supports:** a relative pointer shares `wl_pointer` focus for the same seat
  and emits only while focused; each motion event carries a split 64-bit
  microsecond timestamp plus accelerated and unaccelerated deltas; buttons and
  focus remain on `wl_pointer`. The protocol explicitly warns that
  unaccelerated deltas are not necessarily raw device reports. It defines
  individual events, not a shared history ABI.

- **Source:** wayland-protocols [pointer constraints unstable v1](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/ee78491a237eaff9389a0ccf8680521d074407d3/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)
- **Version:** wayland-protocols 1.49, commit `ee78491a237eaff9389a0ccf8680521d074407d3`; unstable protocol v1
- **Supports:** pointer locks bind a surface, pointer, and seat; activation is
  compositor-controlled; an active lock implies pointer focus; relative motion
  continues while core cursor motion is suppressed; button and axis events are
  unaffected. These lifecycle rules inform an authorization gate but do not,
  by themselves, authorize a raw history stream.

- **Source:** wayland-protocols [security context v1](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/ee78491a237eaff9389a0ccf8680521d074407d3/staging/security-context/security-context-v1.xml)
- **Version:** wayland-protocols 1.49, commit `ee78491a237eaff9389a0ccf8680521d074407d3`; staging protocol v1
- **Supports:** a compositor can receive sandbox engine, application ID, and
  instance ID metadata for clients accepted through a security-context
  listener. The protocol is staged and its metadata is policy input, not proof
  that a client should receive HFIOR data.

### Shared memory and atomic publication

- **Source:** Linux man-pages [`memfd_create(2)`](https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/tree/man/man2/memfd_create.2?id=66d786852379759d22c891d70ff9311d9f193fdc) and [file sealing operations](https://man7.org/linux/man-pages/man2/F_GET_SEALS.2const.html)
- **Version:** Linux man-pages 6.18, commit `66d786852379759d22c891d70ff9311d9f193fdc`
- **Supports:** `memfd_create()` produces an anonymous file descriptor suitable
  for shared mappings; sealing can prevent size or write-state changes. This
  documents the primitives used by the prototype, not its authorization or
  memory-consistency guarantees.

- **Source:** ISO C working draft [N1570](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf), sections 5.1.2.4 and 7.17
- **Version:** C11 committee draft N1570, 2011-04-12
- **Supports:** the language-level meaning of atomic objects, release stores,
  acquire loads, and happens-before. HFIOR's record publication must be argued
  against these rules; the word “lock-free” alone is not a memory model.

## Inspected implementations

### Linux input path

- **Source:** Linux v7.2 [`xhci-ring.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/usb/host/xhci-ring.c?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f), [`usbhid/hid-core.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/usbhid/hid-core.c?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f), [`hid-core.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/hid-core.c?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f), [`input.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/input/input.c?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f), and [`evdev.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/input/evdev.c?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** the inspected implementation boundaries from xHCI completion,
  through USB HID and input-core conversion, to evdev's per-client buffering,
  completed-packet publication, wake/poll path, and explicit overrun handling.
  It does not show that any one of these layers is the dominant cost on an
  arbitrary machine; that requires profiling.

### libinput

- **Source:** libinput [`libinput.h` pointer API](https://gitlab.freedesktop.org/libinput/libinput/-/blob/26191d396d74d505541d6311f0b4ae68d791b890/src/libinput.h), [`evdev.c`](https://gitlab.freedesktop.org/libinput/libinput/-/blob/26191d396d74d505541d6311f0b4ae68d791b890/src/evdev.c), and [`evdev-fallback.c`](https://gitlab.freedesktop.org/libinput/libinput/-/blob/26191d396d74d505541d6311f0b4ae68d791b890/src/evdev-fallback.c)
- **Version:** libinput 1.31.3, commit `26191d396d74d505541d6311f0b4ae68d791b890`
- **Supports:** libinput exposes distinct motion and button events,
  microsecond event times, and separate accelerated and unaccelerated deltas;
  its evdev dispatch drains available records and fallback pointer dispatch
  occurs at `SYN_REPORT`. The API notes that timestamps need not always
  increase, so sequence validation cannot be replaced by timestamp ordering.

### Reference integration: Aquamarine and Hyprland

- **Source:** Aquamarine [`Input.hpp`](https://github.com/hyprwm/aquamarine/blob/a79fb21b2e2a82dd061a6d071802bcf38bd5c383/include/aquamarine/input/Input.hpp) and [`Session.cpp`](https://github.com/hyprwm/aquamarine/blob/a79fb21b2e2a82dd061a6d071802bcf38bd5c383/src/backend/Session.cpp)
- **Version:** Aquamarine v0.14.0, commit `a79fb21b2e2a82dd061a6d071802bcf38bd5c383`
- **Supports:** the pointer abstraction has separate move, button, axis, and
  frame signals; move records contain accelerated and unaccelerated deltas.
  This revision converts libinput's microsecond pointer timestamp to a
  `uint32_t` millisecond field, identifying a timestamp-fidelity seam. It does
  not imply upstream HFIOR support.

- **Source:** Hyprland [`IPointer.hpp`](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/devices/IPointer.hpp), [`Mouse.cpp`](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/devices/Mouse.cpp), [`InputManager.cpp`](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/managers/input/InputManager.cpp), and [`RelativePointer.cpp`](https://github.com/hyprwm/Hyprland/blob/efb50993780079460b0cbed1363e2166a2de1d9f/src/protocols/RelativePointer.cpp)
- **Version:** Hyprland v0.56.2, commit `efb50993780079460b0cbed1363e2166a2de1d9f`
- **Supports:** Hyprland maps Aquamarine move/button/frame signals into
  distinct internal pointer events and sends relative motion through its
  protocol manager. This is the first reference-integration boundary, not the
  definition of HFIOR and not evidence that upstream Hyprland implements it.

### Wine and Windows Raw Input

- **Source:** Wine [`rawinput.c`](https://gitlab.winehq.org/wine/wine/-/blob/3489c6531091e307d5865e796e85cb871c90bc6e/dlls/win32u/rawinput.c) and native Wayland [`wayland_pointer.c`](https://gitlab.winehq.org/wine/wine/-/blob/3489c6531091e307d5865e796e85cb871c90bc6e/dlls/winewayland.drv/wayland_pointer.c)
- **Version:** Wine mainline snapshot, commit `3489c6531091e307d5865e796e85cb871c90bc6e`
- **Supports:** Wine implements Raw Input registration, buffered and per-record
  reads, and `WM_INPUT`-family processing. Its Wayland driver consumes relative
  pointer motion and forwards unaccelerated motion through the raw-input path.
  These are possible future adapter seams; no HFIOR Wine/Proton integration is
  claimed.

- **Source:** Microsoft [Raw Input overview](https://learn.microsoft.com/windows/win32/inputdev/about-raw-input) and [`GetRawInputBuffer`](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getrawinputbuffer)
- **Version:** Windows desktop API documentation; pages current on access date
- **Supports:** applications register for raw device classes and can drain an
  array of accumulated `RAWINPUT` structures; Microsoft explicitly documents
  buffered reading for high-frequency devices. Therefore “drain accumulated
  mouse records together” is prior art, and a Wine adapter must preserve the
  Windows API contract.

## Related ring and deadline-driven designs

These are design analogies, not equivalent input architectures and not
performance evidence for HFIOR.

- **Source:** Linux [perf userspace ring-buffer documentation](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/userspace-api/perf_ring_buffer.rst?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** precedent for mmap data rings with explicit producer head,
  consumer tail, memory-ordering rules, wakeup controls, and loss reporting.

- **Source:** Linux [BPF ring-buffer documentation](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/bpf/ringbuf.rst?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** precedent for ordered shared records and notification choices.
  BPF's multi-producer semantics and security boundary differ from HFIOR.

- **Source:** Linux [AF_XDP documentation](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/networking/af_xdp.rst?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** precedent for mmap producer/consumer rings and explicit
  ownership transitions. Packet buffers and pointer-input authorization have
  different correctness and confidentiality requirements.

- **Source:** liburing [`io_uring_enter(2)`](https://github.com/axboe/liburing/blob/27255f4ceda937b3540e425ed58284f5c03c8ad4/man/io_uring_enter.2)
- **Version:** liburing mainline snapshot, commit `27255f4ceda937b3540e425ed58284f5c03c8ad4`
- **Supports:** precedent for shared submission/completion queues and batched
  transitions. Generic I/O requests are not input observations.

- **Source:** Khronos [OpenXR 1.1 specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html)
- **Version:** OpenXR 1.1 specification, current on access date
- **Supports:** prior art for querying time-related spatial state around a
  render deadline. HFIOR pointer motion is not tracked-pose prediction, and no
  OpenXR performance result transfers to HFIOR.

## Measurement interfaces

- **Source:** Linux [usbmon documentation](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/usb/usbmon.rst?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** the kernel interface used to observe USB I/O. USB traffic,
  evdev reports, IRQs, wakeups, consumer actions, and frames are distinct
  quantities and must be measured separately.

- **Source:** Linux [event tracing documentation](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/trace/events.rst?id=8d3ae59288f1e7d58d76558a6ee96d533bc5019f)
- **Version:** Linux v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- **Supports:** tracefs event enablement and event-format discovery used by
  scheduler, IRQ, and input-boundary tracing. Trace availability is evidence
  availability, not a zero count.

- **Source:** Linux man-pages [`getrusage(2)`](https://git.kernel.org/pub/scm/docs/man-pages/man-pages.git/tree/man/man2/getrusage.2?id=66d786852379759d22c891d70ff9311d9f193fdc)
- **Version:** Linux man-pages 6.18, commit `66d786852379759d22c891d70ff9311d9f193fdc`
- **Supports:** the meaning of `ru_utime`, `ru_stime`, `ru_nvcsw`, and
  `ru_nivcsw` used by the process harness. These are process accounting fields,
  not whole-system CPU or end-to-end latency measurements.

## Evidence and novelty boundary

No external source above establishes HFIOR's benchmark results, validates its
experimental ABI, or proves novelty. Shared rings, explicit loss counters,
buffered Raw Input, focus and pointer-lock policy, and deadline-oriented state
consumption all have substantial prior art. The research claim is limited to
the measured HFIOR composition described by this repository. No legal
patentability opinion is offered.

External source code remains under its upstream license. Linking and analysis
here do not import or relicense Linux, Wayland, libinput, Hyprland, Aquamarine,
Wine, or Khronos material.

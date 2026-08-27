# Frequently asked questions

## Does HFIOR lower an 8 kHz mouse to 1 kHz?

No. It keeps each accepted movement report in order. The game can read them
once per frame without lowering the mouse's polling rate.

## Does it merge movement samples?

No. A game may add the movement together after reading it, but HFIOR does not
replace `+3 @ t0, −3 @ t1` with zero. Both direction changes remain available.

## Are individual observations retained?

Yes, unless the history fills up. If that happens, counters and missing record
numbers show the loss. A test with lost records cannot claim perfect retention.

## Why not just batch evdev events?

Reading several Linux input events at once can reduce overhead. It does not by
itself give a game shared, ordered history, clear loss reporting, controlled
access, or a final read close to the frame. HFIOR provides those rules together.

## Is HFIOR Hyprland-only?

No. Hyprland and Aquamarine are the first native reference integration target.

## Does HFIOR require a kernel patch?

Not necessarily. The current input source runs outside the kernel. A kernel
change only makes sense if measurements show that the remaining cost is there.

## Does it make 8 kHz free?

No. The system still has to receive and save every report. HFIOR aims to remove
repeated heavy work after that point.

## Does it improve FPS?

It can reduce CPU work and thread wakeups. The FPS result depends on the game,
so each game still needs a real before-and-after test.

## Is latency worse because input is batched?

Not automatically. HFIOR performs one last quick read close to the point where
the game uses mouse movement. In the physical game benchmark, the newest input
was fresher with HFIOR for that workload.

## Is Late-Latch physically proven?

Generated-input tests and the physical game benchmark support the final-read
design. Physical mouse-to-screen latency has not been measured yet.

## Is HFIOR actually as good as it sounds?

The results are strong on the paths tested so far. HFIOR kept a comparable
measured input rate and preserved record order while cutting repeated game work.
The [heavy game benchmark](../README.md#real-mouse-test-in-a-heavy-3d-scene) also
improved frame rate and software-measured input freshness.

That does not prove every game will improve. The benchmark was deliberately
designed to expose the cost of doing heavy work for every mouse report. HFIOR
still needs testing in more games, on more machines, and with physical
mouse-to-screen equipment.

## Can a bad HFIOR integration increase latency?

Yes. Reading the history too early, doing too much work during the final check,
or delaying buttons until the next frame can make latency worse. The current
design avoids those mistakes by keeping buttons immediate and performing one
small movement check close to camera or render use. Every integration still
needs a before-and-after latency test.

## What overhead does HFIOR add?

Every report still has to be received, timestamped, saved, checked, and later
applied. The shared history uses a fixed amount of memory, and the game tracks
its reading position and marks handled records. HFIOR does not remove that work.
It aims to stop heavier game logic from running thousands of times per second.

## How do I add HFIOR to my game or application?

It is not a drop-in switch yet. The application needs an approved HFIOR stream
from the compositor or another trusted input source. It then:

1. opens the read-only history and starts a cursor;
2. reads and applies every new movement record once per frame;
3. performs a small final read just before the camera or render uses input;
4. marks records as handled after use; and
5. keeps buttons on the normal immediate path and falls back to normal input if
   HFIOR is unavailable.

Start with the [minimal reader](../examples/minimal-consumer/main.c),
[full-history reader](../examples/full-history-consumer/main.c), and
[final-read example](../examples/late-latch-consumer/main.c). The examples teach
the C API. Real applications also need the current
[experimental platform integration](../integrations/README.md).

## Can an existing game benefit without being changed?

Not fully. The game, engine, or a compatibility layer must read the history at
the right point in the frame to get the main benefit. Ordinary applications
continue to use normal input APIs. Future engine, SDL, Wine, or Proton work
could hide some integration details, but those paths are not shipped today.

## Is HFIOR ready for a production game?

Not yet. The C interface and Wayland integration are experimental and may
change. HFIOR is ready for prototypes, benchmarks, and integration work, but a
shipping application should keep a tested normal-input fallback.

## What happens to buttons?

Button presses and releases take an immediate path. Movement uses the saved
history.

## Could this apply to touch, stylus, or VR input?

It may work for them, but they have not been tested.

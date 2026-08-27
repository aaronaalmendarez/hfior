# Motivation

High-rate mice report movement more often. An 8 kHz mouse can send up to 8,000
reports each second. Many input systems wake the game and repeat the same work
for every report, even if the game only draws 240 frames each second.

HFIOR asks a simple question: why should 8,000 mouse reports require 8,000 heavy
game updates? HFIOR quickly saves each report in order. The game can then read
that history when it is preparing a frame.

This does not make 8 kHz free. The mouse, USB system, Linux input code, and HFIOR
still have to handle every report. HFIOR cuts repeated work later in the chain.
Benchmarks must still measure the real report rate, game work, and frame rate
separately.

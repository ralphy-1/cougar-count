# Cougar Count

Live occupancy counter for the Fitness & Lifestyle Centre at the University of Regina,
so you can check how busy the gym is before you walk over.

Eildvin Logrono · Aaron Borja

## How it works

Two ESP32-C3 boards, one per door. Each door has infrared beams across it; a person
breaking one beam and then the other is a crossing, and which beam broke first is the
direction. Nothing is counted unless both beams break, in that order, in a plausible
amount of time.

The boards push into a Firebase database that only ever holds two numbers: total people
in, total people out. Occupancy is the difference, computed when someone loads the page.
There are no cameras and no identities — a beam break is the only thing a sensor can see.

## Status

Pre-hardware. Nothing is purchased and nothing is mounted; the install is waiting on
approval from Kinesiology & Health Studies. Everything here is built and tested against
firmware simulation mode until then.

## Layout

    firmware/   ESP32 code, plus host-side tests that need no hardware
    web/        the page students actually load
    rules/      Firebase database rules
    functions/  nightly reset
    docs/       hardware notes, approvals, security

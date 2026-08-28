# How it works

Two doors, each with a pair of infrared beams. The order the beams break tells
you which way a person went. Those events become two numbers in a database, and
the website subtracts them.

## 1. At the door

Two beams across the doorway, 20 cm apart, at chest height. Each beam is a sensor
aimed at a reflector -- it emits infrared and watches for its own light to come
back.

            hallway
       ═══════════════   beam A   (always the outside one, at both doors)
       ═══════════════   beam B
              gym

The sensor gives the board one bit: blocked, or clear. It cannot see a person, a
face, or a shape. It knows only that something interrupted the light.

## 2. On the board

The board receives a trickle of events -- *this beam, this new state, at this
millisecond*. Individually they mean nothing, so it remembers what it is in the
middle of. That is `CrossingFSM`.

It waits until both beams are clear again, then asks three questions:

- Did the **other** beam break too? If not it was an arm, a bag, or a door swing.
- Was it slower than 120 ms? Faster than that is not a human.
- Was it faster than 3 s? Slower than that is someone standing in the doorway.

All three yes means one crossing, and the direction is whichever beam broke
first. A first means the person came from the hallway, so that is an entry.

This is why identical code runs on both boards: direction is measured, never
assumed. Someone walking out through the entrance counts as -1, correctly.

The exit is wide enough for two people abreast, so it runs two of these
independently, one per leaf.

## 3. Getting off the board

Only one board is planned to have campus network access. The other reaches it
over ESP-NOW, a direct radio link between the two ESP32s. That keeps the ask to
IT as small as possible -- one registered device instead of two.

If the network drops, crossings queue in flash and go up when it returns.

The exit board holds each crossing until the entrance board says it has taken
it, and resends until that acknowledgement arrives. Every crossing carries a
sequence number and the sender's boot id, so a resend caused by a lost
acknowledgement is recognised as the same person rather than counted as a
second one. The link is encrypted, because an open radio at a gym entrance
could be injected into by anyone standing nearby.

## 4. In the database

Four values. This is everything that leaves the building.

    in_total      812      only ever goes up
    out_total     765      only ever goes up
    updated_at    <time>
    capacity      150

There is no "people in the gym" value stored anywhere. Occupancy is 812 - 765,
worked out by whoever loads the page. Why: see `data-model.md`.

The security rules permit exactly one kind of write -- the new value must be
exactly one more than the old. No jumping, no resetting, no skipping.

## 5. On someone's phone

The page asks for those numbers every 30 seconds and subtracts them. It needs no
login, because a count of people in a public gym is public information -- which
also means there is no key in the frontend to leak.

It is a web page rather than an app on purpose. The question is "should I go to
the gym right now", asked while putting shoes on. A link answers it in one tap. An
app store listing adds an install, a review process, and $99 a year, and would
lose most people before they ever saw the number. Built as a PWA, it can still be
added to a home screen and open like an app.

## 6. At night

A scheduled job copies the day into `history/`, records the leftover, and sets
both counters to zero. It runs with admin privileges, because nothing holding a
device credential should be able to erase a day.

## 7. How we know it works

The gym is empty at close, so the number should be zero. Whatever it actually
reads is that day's error, and it is written down every night. Positive means we
missed people leaving; negative means we missed people arriving.

That is a free daily accuracy measurement with nobody standing at a door with a
clicker, and it is the reason for two counters instead of one. A single
occupancy number has no way of telling you it is lying.

## Where this can go wrong

**People walking abreast, or tailgating.** The real weakness. Two people shoulder
to shoulder in one lane read as one crossing. Expect the count to run low at 5pm
and be close the rest of the day. The nightly residual says how bad. If it is
ugly the fix is an overhead depth sensor, not a rewrite.

**The 250 ms deaf window.** After a count the FSM ignores everything briefly so
one person is not counted twice. Anyone entering inside that window is lost. The
number is a guess and needs tuning against real data.

**Something parked in a beam.** A propped door, a bin, a mat. That lane goes dead
and nothing announces it. `updated_at` is the early warning: a lane that has not
reported during open hours is broken.

**Site facts we do not have.** Whether both exit leaves open, whether there is
power near the doors, the real hours.

**One writer or two.** If the ESP-NOW link works, one board writes both counters.
If both end up networked, there are two writers. Split counters are safe either
way, which is why they were chosen before knowing the answer.

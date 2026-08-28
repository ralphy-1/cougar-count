# What is in the database

The whole thing is four numbers.

    gym/
      live/
        in_total       812      people who have walked in today
        out_total      765      people who have walked out today
        updated_at     <ms>     last time a board wrote
        session_date   "2026-08-27"
        capacity       150      shown as "how full", never written by a board
        all_lanes_ok   true     is every door still counting?

      history/
        2026-08-26/
          in_total     903
          out_total    901
          residual     2        should be 0 -- this is the day's error
          peak         58

Occupancy is `in_total - out_total`, computed by whoever is looking at it. It is
never stored.

## Why occupancy is not a stored number

Two boards write to this database and they do not know about each other. If they
both stored "occupancy", then:

    entrance reads 40, adds 1, writes 41
    exit     reads 40, subtracts 1, writes 39     <- entrance's person vanished

Both boards did exactly what they were told and a person disappeared. This is a
race, and it does not show up in testing because it needs two writes to land in
the same few milliseconds -- which is a busy Tuesday at 5pm, not a quiet
afternoon at a desk.

Split counters remove the race instead of trying to time around it. The entrance
board only ever touches `in_total`, the exit board only ever touches `out_total`,
and neither one ever reads a value it depends on. There is no shared number to
fight over.

## Why the counters only go up

`in_total` and `out_total` are monotonic. They never decrease during the day, and
the security rules enforce it: a write is rejected unless the new value is
**exactly one more** than the old one. Against a node that does not exist yet, a
write of exactly `1` is allowed -- that is what one person walking through
produces, since the increment sentinel resolves to 1 on a missing node.

That means a board cannot set the counter to 5000. It cannot set it back to 0. It
cannot skip. The only thing a valid write can do is add one person. So the worst
case for a stolen device credential is that someone slowly inflates a number on a
webpage, one request at a time -- and every one of those requests is a server
round trip.

It also means the counters cannot be batched. If a board is offline for ten
minutes and has thirty crossings queued, it must send thirty separate increments
on reconnect. That is fine -- thirty small requests take a couple of seconds --
and it is a cheap price for a rule this strict.

## updated_at is a heartbeat, not a side effect

**The boards must write `updated_at` on a timer -- every two minutes -- whether
or not anyone crossed a beam.** This is a contract the firmware has to honour,
not an optimisation.

The web page refuses to display a count it believes is stale: older than six
minutes, or with no timestamp at all. It shows "the counter hasn't reported
recently" instead of a number. That exists because the alternative is worse -- a
board that dies at 2pm would otherwise show 2pm's number all evening, and someone
reading a confident "12 - Quiet" walks across campus to a packed gym.

If `updated_at` were only written when someone crossed a beam, then an empty gym
at 6am would be indistinguishable from a dead board, and the page would call a
perfectly healthy system broken. Hence the timer.

Write it with the server timestamp sentinel:

    { ".sv": "timestamp" }

not the board's own clock. The security rules require `updated_at` to land within
five minutes of server time, which the sentinel satisfies by definition -- and an
ESP32's clock is wrong until NTP answers, which may be never if the network is
down.

Six minutes is three missed heartbeats. That is deliberately forgiving: a single
dropped write on flaky wifi must not blank the page.

## all_lanes_ok

False means at least one lane has stopped counting -- a beam blocked by a
propped door or a bin, or the exit board having gone quiet on the radio. The
page still shows the number, because it is a floor rather than a fiction, but it
says out loud that the real figure is higher.

The entrance board owns this value and reports for both doors: its own lanes,
plus the exit board's health as last heard over the radio. Silence counts as
not-ok after three missed reports, because from the entrance board a silent
peer and a blocked beam look identical, and both mean people are being missed.

It is written on change rather than on a timer. A door going blind is the thing
a reader most needs to be told, and waiting up to two minutes for the next
heartbeat to carry it is two minutes of a number nobody knows to distrust.

## What resets the counters

Nothing on a board can. `+1` is the only legal write, so a board is physically
incapable of zeroing anything.

The nightly Cloud Function does it, using the admin SDK, which runs with
privileges above the security rules. It copies the day into `history/`, records
the residual, and sets both counters to 0.

## The residual is the accuracy metric

The gym is empty when it closes, so `in_total - out_total` should be zero at
close. Whatever it actually is, is the error accumulated that day. Positive means
we missed people leaving; negative means we missed people arriving.

This is why occupancy is derived from two separate counters rather than tracked
as one number -- one number would have no way to tell you it was wrong.

## Why the page is world-readable

`gym/live` is readable without authentication on purpose. It is a count of people
in a public gym, which is information the gym would happily put on a sign. Making
it public means the web page needs no credentials, so there is no key to leak in
the frontend.

`events/` is closed off. Nothing writes there yet; it exists so that per-crossing
diagnostics can be added later without anyone assuming that path is public.

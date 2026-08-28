# Security

Written for the University of Regina's IS Service Desk and information security
review. Information Services will ask for most of this again when the device
registration work order is opened, so it lives here rather than in an inbox.

Nothing described here is installed yet. See `approvals.md` for where that stands.

## What we need from IT

- **One MAC address registered on the main campus network.** Only one of the two
  boards is networked; the second reaches it over ESP-NOW, a direct radio link
  between the two ESP32s, and never touches campus infrastructure.
- **DHCP.** No static IP or reservation.
- **Outbound HTTPS (TCP 443) to Firebase.** Nothing else outbound, nothing inbound.
- **No open ports.** No listening services, no local web server, no remote shell.
  The device only ever makes outbound requests.
- **A dedicated device credential**, if one can be issued. We would much rather have
  that than put a personal account on hardware bolted to a wall, and we are not
  going to flash personal credentials onto it either way.

Bandwidth is negligible: a few hundred bytes per person who walks through, so on the
order of a megabyte on a busy day.

## Risks, and what is done about each

### Physical tampering

It is in a public gym, so someone can pull it off the wall. It sits in a locked
enclosure mounted out of easy reach, but that is a speed bump, not a wall. Which
leads directly to the next one.

### Credentials readable from flash — ACCEPTED RISK

ESP32 flash can be read out by anyone holding the board and a USB cable. The device
credential is therefore recoverable by anyone who physically removes the hardware.

This is the risk we take most seriously and it is the reason for asking for a
dedicated credential: if the board walks off, one thing is revoked and nothing else
is exposed. The MAC registration can also be dropped at any time without needing
anything from us.

What that credential can actually do is deliberately tiny — see the next section.

### Rogue device on the network

It is an outbound-only client with nothing listening. If an isolated VLAN or a
segment with no route to internal resources is preferred, that is entirely fine; the
only thing it needs to reach is Firebase.

### Privacy

No cameras and no images. The sensor is an infrared beam and the only thing it can
report is that the beam broke at a particular millisecond. It cannot identify anyone,
cannot tell two people apart, and has no connection to card scanner data or any
student record.

What leaves the building is two integers — total in and total out. The public page
shows one number: how many people are currently in the gym.

Cameras were considered and rejected. They would count more accurately, but video of
identifiable people is personal information, the gym entrance is near change rooms,
and "the hardware cannot see" is a property that can be verified by looking at the
part, where "it sees but forgets" would have to be taken on trust.

### Database exposure

The database is readable without authentication on purpose — the occupancy number is
meant to be public, and that means there is no key in the web page to leak.

Writes require authentication, and the security rules permit exactly one operation:
increasing a counter by exactly one. A device cannot set the counter to an arbitrary
value, cannot reset it, cannot skip, and cannot delete it. The nightly reset that
zeroes the day runs server-side with admin privileges, which nothing on a board has.

So the worst case with a stolen device credential is a wrong number on a webpage,
one server round trip at a time.

### No rate limit on the increment — ACCEPTED RISK

A stolen credential cannot jump the counter, but it can add one, then one, then one,
indefinitely. Firebase Realtime Database rules cannot express a rate limit without
storing a last-write timestamp per writer and validating against it, which is more
machinery than this is worth.

We are accepting this knowingly. The consequence is a wrong occupancy number, which
is self-evident to anyone standing in the gym and is corrected at the nightly reset.
There is no personal data to reach and no other system to pivot into.

### TLS certificate verification — CONDITIONAL

The firmware verifies the Firebase certificate against a root CA supplied in
`secrets.h` as `FIREBASE_ROOT_CA`. If that is left empty the connection still
works but the certificate is **not** verified, and the firmware says so on the
serial console every time it connects.

Empty is acceptable on a bench. It is not acceptable on the campus network,
where an unverified TLS connection can be intercepted and rewritten by anyone
positioned to do so. The root certificate must be filled in before installation.

### Firmware updates

No over-the-air updates. Firmware is flashed over USB with the board in hand.

## Handling of secrets in this repository

Wifi credentials, the Firebase host, and the device account live in
`firmware/src/secrets.h`, which is gitignored and has never been committed. A
`secrets.h.example` with placeholder values is checked in so the file's shape is
documented. The web page contains no credentials of any kind.

## Summary

The worst realistic outcome is that someone steals a fifteen dollar microcontroller
and the website shows a wrong number for a while. There is no personal data on the
device, none in the database, and no inbound access to anything.

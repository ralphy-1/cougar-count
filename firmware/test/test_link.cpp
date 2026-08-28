// Host-side tests for the radio message format and duplicate detection.
//
//   c++ -std=c++17 -o /tmp/test_link firmware/test/test_link.cpp \
//       firmware/src/link.cpp -I firmware/src && /tmp/test_link

#include "link.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "  ok  " : "FAILED", what);
  if (!ok) failures++;
}

static LinkFrame crossing(int8_t dir, uint16_t boot, uint16_t seq) {
  LinkFrame f{};
  f.kind = LINK_CROSSING;
  f.dir = dir;
  f.bootId = boot;
  f.seq = seq;
  f.lanesOk = 0x03;
  f.laneCount = 2;
  return f;
}

int main() {
  // --- framing -------------------------------------------------------------
  {
    uint8_t buf[32];
    const LinkFrame sent = crossing(-1, 4242, 7);
    const size_t n = linkEncode(sent, buf, sizeof(buf));
    check(n == LINK_FRAME_BYTES, "a frame encodes to a fixed size");

    LinkFrame got{};
    check(linkDecode(buf, n, got),          "and decodes again");
    check(got.kind == LINK_CROSSING,        "kind survives");
    check(got.dir == -1,                    "direction survives, sign and all");
    check(got.bootId == 4242,               "boot id survives");
    check(got.seq == 7,                     "sequence survives");
    check(got.lanesOk == 0x03,              "lane health survives");
    check(got.laneCount == 2,               "lane count survives");
  }
  {
    uint8_t small[4];
    check(linkEncode(crossing(1, 1, 1), small, sizeof(small)) == 0,
          "encoding refuses a buffer that is too small");
  }

  // --- a radio in a public building hears all sorts of things ---------------
  {
    uint8_t buf[LINK_FRAME_BYTES];
    linkEncode(crossing(1, 1, 1), buf, sizeof(buf));
    LinkFrame got{};

    check(!linkDecode(buf, LINK_FRAME_BYTES - 1, got), "a short frame is rejected");
    check(!linkDecode(buf, LINK_FRAME_BYTES + 1, got), "an overlong frame is rejected");

    uint8_t wrong[LINK_FRAME_BYTES];
    memcpy(wrong, buf, sizeof(wrong));
    wrong[0] ^= 0xFF;
    check(!linkDecode(wrong, sizeof(wrong), got), "the wrong magic byte is rejected");

    memcpy(wrong, buf, sizeof(wrong));
    wrong[1] = LINK_VERSION + 1;
    check(!linkDecode(wrong, sizeof(wrong), got), "a future version is rejected, not guessed at");
  }
  {
    // The checks above pass for the wrong reason: changing a byte also breaks
    // the checksum, so the CRC rejects the frame before magic or version is
    // ever consulted. Forge properly checksummed frames so each check has to
    // stand on its own.
    LinkFrame got{};
    uint8_t f[LINK_FRAME_BYTES];

    linkEncode(crossing(1, 1, 1), f, sizeof(f));
    f[1] = LINK_VERSION + 1;
    uint16_t crc = linkCrc(f, 10);
    f[10] = (uint8_t)(crc & 0xFF);
    f[11] = (uint8_t)(crc >> 8);
    check(!linkDecode(f, sizeof(f), got),
          "a correctly checksummed frame from a future version is still rejected");

    linkEncode(crossing(1, 1, 1), f, sizeof(f));
    f[0] = 0x42;
    crc = linkCrc(f, 10);
    f[10] = (uint8_t)(crc & 0xFF);
    f[11] = (uint8_t)(crc >> 8);
    check(!linkDecode(f, sizeof(f), got),
          "and so is a correctly checksummed frame that is not ours at all");
  }
  {
    // Every single bit, one at a time. A corrupted direction byte that still
    // parsed would move the count the wrong way, which is worse than losing it.
    uint8_t buf[LINK_FRAME_BYTES];
    linkEncode(crossing(1, 900, 5), buf, sizeof(buf));

    bool allCaught = true;
    for (size_t byte = 0; byte < LINK_FRAME_BYTES; byte++) {
      for (uint8_t bit = 0; bit < 8; bit++) {
        uint8_t bad[LINK_FRAME_BYTES];
        memcpy(bad, buf, sizeof(bad));
        bad[byte] ^= (uint8_t)(1u << bit);

        LinkFrame got{};
        if (linkDecode(bad, sizeof(bad), got)) allCaught = false;
      }
    }
    check(allCaught, "every single-bit corruption is caught (96 of them)");
  }

  // --- duplicates ----------------------------------------------------------
  {
    // The one that matters. The exit board retries until acknowledged, so a
    // lost ACK means the same person arrives twice. Counting them both would
    // add a phantom occupant every time the radio hiccupped.
    Dedup d;
    check(d.accept(100, 1),  "a first crossing is new");
    check(!d.accept(100, 1), "the same one again is a retry, not a second person");
    check(!d.accept(100, 1), "and again");
    check(d.accept(100, 2),  "the next one is new");
  }
  {
    Dedup d;
    d.accept(100, 5);
    check(!d.accept(100, 4), "an older sequence number is stale and ignored");
    check(!d.accept(100, 1), "so is a much older one");
    check(d.accept(100, 6),  "but forward progress still works");
  }
  {
    // The other board restarted and began counting from one again. Its numbers
    // say nothing about ours, so refusing them would mean going deaf until the
    // sequence caught up -- potentially thousands of people.
    // The sequence numbers must be chosen so that ignoring the boot id would
    // actually change the answer. Going from seq 60000 to seq 1 still looks
    // like forward progress once the counter wraps, so that pairing proves
    // nothing -- 5 to 1 is unambiguously backwards.
    Dedup d;
    d.accept(100, 5);
    check(d.accept(101, 1),  "a new boot id starts a fresh sequence, even going backwards");
    check(!d.accept(101, 1), "which then dedupes normally");
  }
  {
    // Wrapping past 65535 mid-day must not look like an enormous jump backwards.
    Dedup d;
    d.accept(7, 65534);
    check(d.accept(7, 65535), "65535 follows 65534");
    check(d.accept(7, 0),     "and 0 follows 65535");
    check(d.accept(7, 1),     "and 1 follows 0");
    check(!d.accept(7, 0),    "while 0 again is still a duplicate");
  }
  {
    Dedup d;
    d.accept(1, 10);
    d.reset();
    check(d.accept(1, 10), "reset forgets, so a restarted link is not deaf");
  }

  printf("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
  return failures != 0;
}

// Host-side unit tests for NBusParser. Compile with plain g++ (no Arduino):
//   g++ -std=c++17 -I firmware test/test_parser.cpp firmware/NBusParser.cpp -o /tmp/t && /tmp/t

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "NBusParser.h"

static int g_failures = 0;

static void check_near(const char* what, float got, float want, float tol = 0.01f) {
  if (std::fabs(got - want) <= tol) {
    std::printf("  ok   %-22s = %.3f (want %.3f)\n", what, got, want);
  } else {
    std::printf("  FAIL %-22s = %.3f (want %.3f, tol %.3f)\n", what, got, want, tol);
    ++g_failures;
  }
}

static void check_int(const char* what, int got, int want) {
  if (got == want) {
    std::printf("  ok   %-22s = %d\n", what, got);
  } else {
    std::printf("  FAIL %-22s = %d (want %d)\n", what, got, want);
    ++g_failures;
  }
}

static void check_str(const char* what, const char* got, const char* want) {
  if (std::strcmp(got, want) == 0) {
    std::printf("  ok   %-22s = \"%s\"\n", what, got);
  } else {
    std::printf("  FAIL %-22s = \"%s\" (want \"%s\")\n", what, got, want);
    ++g_failures;
  }
}

static void check_true(const char* what, bool cond) {
  if (cond) {
    std::printf("  ok   %-22s\n", what);
  } else {
    std::printf("  FAIL %-22s (expected true)\n", what);
    ++g_failures;
  }
}

int main() {
  // --- Vector 1: battery V + I (discharging) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x02, 0x05, 0x32, 0x82, 0x65};
    check_true("v1 accepted", p.feedResponse(f, sizeof f));
    check_near("v1 batt_voltage", p.state().batt[0].voltage, 13.30f);
    check_near("v1 batt_current", p.state().batt[0].current, -6.13f);
  }

  // --- Vector 2: battery V + I (higher discharge) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x02, 0x05, 0x30, 0x84, 0x73};
    check_true("v2 accepted", p.feedResponse(f, sizeof f));
    check_near("v2 batt_voltage", p.state().batt[0].voltage, 13.28f);
    check_near("v2 batt_current", p.state().batt[0].current, -11.39f);
  }

  // --- Vector 3: SoC ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x0B, 0x4B, 0xFF, 0xFF, 0xFF};
    check_true("v3 accepted", p.feedResponse(f, sizeof f));
    check_int("v3 batt_soc", p.state().batt[0].soc, 75);
  }

  // --- Vector 4: cell voltages (reg 0x57 -> cells 3,4) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x57, 0x0D, 0x23, 0x0D, 0x21};
    check_true("v4 accepted", p.feedResponse(f, sizeof f));
    check_near("v4 cell_v[2]", p.state().batt[0].cell_v[2], 3.363f, 0.001f);
    check_near("v4 cell_v[3]", p.state().batt[0].cell_v[3], 3.361f, 0.001f);
  }

  // --- Vector 5: solar V + I (charging, positive) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x81, 0x06, 0xF4, 0x02, 0x05, 0x33, 0x00, 0x12};
    check_true("v5 accepted", p.feedResponse(f, sizeof f));
    check_near("v5 solar_voltage", p.state().solar.voltage, 13.31f);
    check_near("v5 solar_current", p.state().solar.current, 0.18f);
  }

  // --- Vector 6: starter-battery voltage ---
  {
    NBusParser p;
    const uint8_t f[] = {0x81, 0x06, 0xF4, 0x01, 0x04, 0xF7, 0x00, 0x00};
    check_true("v6 accepted", p.feedResponse(f, sizeof f));
    check_near("v6 starter_voltage", p.state().solar.starter_voltage, 12.71f);
  }

  // --- Vector 6b: the charger's 0x60 carries two things at once ---
  // d2 is the bus address, which decodeIdentity claims, and d1 is the charge stage, which
  // it does not. The frame has to satisfy both readers; this is the test that says so,
  // because the obvious refactor — letting decodeIdentity have 0x60 and nothing else — is
  // silent and leaves the stage permanently at its default of 0.
  {
    NBusParser p;
    const uint8_t bulk[] = {0x81, 0x06, 0xF4, 0x60, 0x42, 0x03, 0x0D, 0x00};
    check_true("v6b accepted", p.feedResponse(bulk, sizeof bulk));
    check_true("v6b stage valid", p.state().solar.stage_valid);
    check_int("v6b stage bulk", p.state().solar.stage, 3);
    check_int("v6b address still read", p.state().solar.id.address, 13);

    // Absorption, then float, then off: the stage must follow rather than latch.
    const uint8_t abs_[] = {0x81, 0x06, 0xF4, 0x60, 0x42, 0x04, 0x0D, 0x00};
    p.feedResponse(abs_, sizeof abs_);
    check_int("v6b stage absorption", p.state().solar.stage, 4);
    const uint8_t flt[] = {0x81, 0x06, 0xF4, 0x60, 0x42, 0x06, 0x0D, 0x00};
    p.feedResponse(flt, sizeof flt);
    check_int("v6b stage float", p.state().solar.stage, 6);
    const uint8_t off[] = {0x81, 0x06, 0xF4, 0x60, 0x42, 0x00, 0x0D, 0x00};
    p.feedResponse(off, sizeof off);
    check_int("v6b stage off", p.state().solar.stage, 0);
    check_true("v6b still valid at zero", p.state().solar.stage_valid);
  }

  // --- Vector 6c: a pack's 0x60 must not set a charge stage ---
  // Only the charger puts a stage in d1; on the packs that byte is a flat zero and the
  // register is identity all the way through. Reading it there would invent a stage for a
  // device that has none, so the battery path must leave stage_valid alone.
  {
    NBusParser p;
    const uint8_t pack[] = {0x85, 0x06, 0xF4, 0x60, 0x40, 0x00, 0x0B, 0x00};
    check_true("v6c accepted", p.feedResponse(pack, sizeof pack));
    check_int("v6c pack address", p.state().batt[0].id.address, 11);
    check_true("v6c no solar stage", !p.state().solar.stage_valid);
  }

  // --- Vector 7: cell voltages (reg 0x56 -> cells 1,2) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x56, 0x0D, 0x24, 0x0D, 0x22};
    check_true("v7 accepted", p.feedResponse(f, sizeof f));
    check_near("v7 cell_v[0]", p.state().batt[0].cell_v[0], 3.364f, 0.001f);
    check_near("v7 cell_v[1]", p.state().batt[0].cell_v[1], 3.362f, 0.001f);
  }

  // --- Vector 8: remaining energy Wh (reg 0x36, big-endian u16) ---
  // 0x05CF = 1487 Wh (matches the app reading). Cross-confirmed via the BLE project.
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x36, 0x05, 0xCF, 0xFF, 0xFF};
    check_true("v8 accepted", p.feedResponse(f, sizeof f));
    check_int("v8 batt_wh", p.state().batt[0].wh, 1487);
  }

  // --- Vector 9: "quality" % (reg 0x0E, d0) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x0E, 0x63, 0xFF, 0xFF, 0xFF};
    check_true("v9 accepted", p.feedResponse(f, sizeof f));
    check_int("v9 batt_quality", p.state().batt[0].quality, 99);
  }

  // --- Vector 10: nominal capacity Ah (reg 0x07) — verbatim from a live capture ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x07, 0x00, 0x00, 0x00, 0x96};
    check_true("v10 accepted", p.feedResponse(f, sizeof f));
    check_int("v10 batt_capacity_ah", p.state().batt[0].capacity_ah, 150);
  }

  // --- Vectors 11-13: frames captured verbatim off our own bus ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x02, 0x05, 0x2D, 0x83, 0x7F};
    check_true("v11 accepted", p.feedResponse(f, sizeof f));
    check_near("v11 batt_voltage", p.state().batt[0].voltage, 13.25f);
    check_near("v11 batt_current", p.state().batt[0].current, -8.95f);
  }
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x36, 0x06, 0xEB, 0xFF, 0xFF};
    check_true("v12 accepted", p.feedResponse(f, sizeof f));
    check_int("v12 batt_wh", p.state().batt[0].wh, 1771);
  }
  {
    NBusParser p;
    const uint8_t f[] = {0x81, 0x06, 0xF4, 0x1B, 0x00, 0x01, 0xFF, 0xFF};
    check_true("v13 unknown solar reg ignored", !p.feedResponse(f, sizeof f));
  }

  // --- Checksum: diagnostic frames use the LIN *classic* checksum (data only, no PID).
  // These three are observed frame+checksum pairs; the enhanced variant fails all of them.
  {
    const uint8_t f1[] = {0x85, 0x06, 0xF4, 0x02, 0x05, 0x2D, 0x83, 0x7F};
    check_int("cksum v+i", NBusParser::classicChecksum(f1, sizeof f1), 0x48);

    const uint8_t f2[] = {0x85, 0x06, 0xF4, 0x07, 0x00, 0x00, 0x00, 0x96};
    check_int("cksum capacity", NBusParser::classicChecksum(f2, sizeof f2), 0xE1);

    const uint8_t f3[] = {0x81, 0x06, 0xF4, 0x1B, 0x00, 0x01, 0xFF, 0xFF};
    check_int("cksum solar 1B", NBusParser::classicChecksum(f3, sizeof f3), 0x67);
  }

  // --- Negative cases: frames that must be rejected ---
  {
    NBusParser p;
    const uint8_t master[] = {0x85, 0x06, 0xB4, 0x02, 0x00, 0x00, 0x00, 0x00};  // read req, not resp
    check_true("read-request rejected", !p.feedResponse(master, sizeof master));

    const uint8_t unknown_nad[] = {0x80, 0x06, 0xF4, 0x02, 0x05, 0x32, 0x00, 0x00};
    check_true("unknown NAD rejected", !p.feedResponse(unknown_nad, sizeof unknown_nad));

    const uint8_t short_frame[] = {0x85, 0x06, 0xF4};
    check_true("short frame rejected", !p.feedResponse(short_frame, sizeof short_frame));
  }

  // --- Identity registers, from the post-update capture of pack 1.
  // Every field below was read off the Dometic Power app's device page first, so this
  // vector is a direct app-vs-decoder comparison rather than a self-consistency check.
  //
  // The serial numbers here — and everywhere else in this file — are FICTIONAL. The real
  // ones identify specific hardware and are deliberately not in this repository. Only the
  // value is substituted: the byte layout, the split across 0x54/0x55 and the big-endian
  // packing are exactly as captured, which is all the decoder is being tested on.
  {
    NBusParser p;
    const uint8_t prefix[] = {0x85, 0x06, 0xF4, 0x54, 0x00, 'K', 'A', 'A'};
    const uint8_t number[] = {0x85, 0x06, 0xF4, 0x55, 0x00, 0x12, 0xD6, 0x87};
    const uint8_t addr[]   = {0x85, 0x06, 0xF4, 0x60, 0x60, 0x00, 0x01, 0x00};
    const uint8_t model[]  = {0x85, 0x06, 0xF4, 0xA0, 0x00, 0x05, 0x00, 0x08};
    const uint8_t fw[]     = {0x85, 0x06, 0xF4, 0xA1, 0x05, 0x01, 0x02, 0x08};

    check_true("v14 prefix accepted", p.feedResponse(prefix, sizeof prefix, 0));
    // Half a serial is not a serial: nothing must be published until both halves are in.
    check_true("v14 serial withheld", !p.state().batt[0].id.serialValid());
    check_true("v14 number accepted", p.feedResponse(number, sizeof number, 0));
    check_str("v14 serial", p.state().batt[0].id.serial, "KAA1234567");

    check_true("v14 addr accepted", p.feedResponse(addr, sizeof addr, 0));
    check_int("v14 address", p.state().batt[0].id.address, 1);
    check_true("v14 model accepted", p.feedResponse(model, sizeof model, 0));
    check_int("v14 iad", p.state().batt[0].id.iad, 5);
    check_int("v14 model", p.state().batt[0].id.model, 8);
    check_true("v14 fw accepted", p.feedResponse(fw, sizeof fw, 0));
    check_int("v14 fw major", p.state().batt[0].id.fw_major, 5);
    check_int("v14 fw minor", p.state().batt[0].id.fw_minor, 1);
  }

  // --- Vector 15: the two packs must not splice into each other.
  // Both answer on NAD 0x85 and their frames interleave, so a single shared pair of
  // serial half-buffers would join pack 1's prefix to pack 2's number. Feed the halves
  // in the worst possible order and check each pack still assembles its own serial.
  {
    NBusParser p;
    const uint8_t pre0[] = {0x85, 0x06, 0xF4, 0x54, 0x00, 'K', 'A', 'A'};
    const uint8_t pre1[] = {0x85, 0x06, 0xF4, 0x54, 0x00, 'K', 'A', 'A'};
    const uint8_t num0[] = {0x85, 0x06, 0xF4, 0x55, 0x00, 0x12, 0xD6, 0x87};
    const uint8_t num1[] = {0x85, 0x06, 0xF4, 0x55, 0x00, 0x74, 0xCB, 0xB1};

    p.feedResponse(pre0, sizeof pre0, 0);
    p.feedResponse(pre1, sizeof pre1, 1);
    p.feedResponse(num1, sizeof num1, 1);
    p.feedResponse(num0, sizeof num0, 0);
    check_str("v15 pack0 serial", p.state().batt[0].id.serial, "KAA1234567");
    check_str("v15 pack1 serial", p.state().batt[1].id.serial, "KAA7654321");

    // Measurements must land in their own slot too.
    const uint8_t soc0[] = {0x85, 0x06, 0xF4, 0x0B, 0x4B, 0xFF, 0xFF, 0xFF};
    const uint8_t soc1[] = {0x85, 0x06, 0xF4, 0x0B, 0x3C, 0xFF, 0xFF, 0xFF};
    p.feedResponse(soc0, sizeof soc0, 0);
    p.feedResponse(soc1, sizeof soc1, 1);
    check_int("v15 pack0 soc", p.state().batt[0].soc, 75);
    check_int("v15 pack1 soc", p.state().batt[1].soc, 60);

    // Out-of-range slots must be refused rather than writing past the array.
    check_true("v15 slot 2 rejected", !p.feedResponse(soc0, sizeof soc0, 2));
    check_true("v15 slot -1 rejected", !p.feedResponse(soc0, sizeof soc0, -1));
  }

  // --- Vector 16: a slot whose bus address changes has been reassigned to a different
  // pack, which is exactly what the firmware update did (it swapped the poll order).
  // The measurements already in that slot describe the previous occupant and must go.
  {
    NBusParser p;
    const uint8_t addr1[]  = {0x85, 0x06, 0xF4, 0x60, 0x60, 0x00, 0x01, 0x00};
    const uint8_t addr11[] = {0x85, 0x06, 0xF4, 0x60, 0x40, 0x00, 0x0B, 0x00};
    const uint8_t soc[]    = {0x85, 0x06, 0xF4, 0x0B, 0x4B, 0xFF, 0xFF, 0xFF};
    const uint8_t prefix[] = {0x85, 0x06, 0xF4, 0x54, 0x00, 'K', 'A', 'A'};
    const uint8_t number[] = {0x85, 0x06, 0xF4, 0x55, 0x00, 0x12, 0xD6, 0x87};

    p.feedResponse(addr1, sizeof addr1, 0);
    p.feedResponse(soc, sizeof soc, 0);
    p.feedResponse(prefix, sizeof prefix, 0);
    p.feedResponse(number, sizeof number, 0);
    check_int("v16 soc before", p.state().batt[0].soc, 75);
    check_str("v16 serial before", p.state().batt[0].id.serial, "KAA1234567");

    p.feedResponse(addr11, sizeof addr11, 0);
    check_int("v16 address moved", p.state().batt[0].id.address, 11);
    check_true("v16 soc invalidated", !p.state().batt[0].soc_valid);
    check_true("v16 serial invalidated", !p.state().batt[0].id.serialValid());

    // Same address again is not a reassignment and must not wipe anything.
    p.feedResponse(soc, sizeof soc, 0);
    p.feedResponse(addr11, sizeof addr11, 0);
    check_true("v16 stable addr keeps", p.state().batt[0].soc_valid);
  }

  // --- Vector 17: NBusCycleTracker attributes battery frames by ordinal position.
  {
    NBusCycleTracker t;
    NBusCycleTracker::Out out[NBusCycleTracker::kMaxPerCycle + 1];
    const uint8_t d0[4] = {0x4B, 0xFF, 0xFF, 0xFF};
    const uint8_t d1[4] = {0x3C, 0xFF, 0xFF, 0xFF};
    const uint8_t ds[4] = {0x05, 0x33, 0x00, 0x12};

    // Joining mid-cycle: the run we land in the middle of is incomplete, so the first
    // 0x81 frame only marks a starting point and releases nothing.
    int n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    check_int("v17 first 0x81 solar only", n, 1);
    check_int("v17 first 0x81 slot", out[0].slot, NBusCycleTracker::kSolarSlot);

    // Eight clean cycles: the length has to be observed before it can be trusted, so
    // the early ones are dropped rather than guessed at.
    for (int c = 0; c < 8; ++c) {
      check_int("v17 batt buffered", t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]), 0);
      check_int("v17 batt buffered", t.feed(NBUS_NAD_BATTERY, 0x0B, d1, out, sizeof out / sizeof out[0]), 0);
      n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
      if (c < 7) {
        check_int("v17 early cycle dropped", n, 1);
      } else {
        check_int("v17 cycle released", n, 3);
        check_int("v17 slot 0", out[0].slot, 0);
        check_int("v17 slot 0 data", out[0].d[0], 0x4B);
        check_int("v17 slot 1", out[1].slot, 1);
        check_int("v17 slot 1 data", out[1].d[0], 0x3C);
        check_int("v17 solar last", out[2].slot, NBusCycleTracker::kSolarSlot);
      }
    }
    check_int("v17 learned length", t.expectedPerCycle(), 2);
    check_int("v17 accepted", (int)t.cyclesAccepted(), 1);
    check_int("v17 dropped", (int)t.cyclesDropped(), 7);

    // A short run means a battery frame was lost. Every position after the gap would
    // be attributed to the wrong pack, so the whole run is thrown away.
    t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
    n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    check_int("v17 short run dropped", n, 1);
    check_int("v17 dropped after short", (int)t.cyclesDropped(), 8);

    // A missing 0x81 frame merges two cycles. Nothing is lost from either, so the run is
    // attributed by position modulo the cycle length rather than thrown away — and it
    // counts as the two cycles it actually is.
    {
      const uint32_t acc = t.cyclesAccepted();
      const uint32_t drp = t.cyclesDropped();
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_BATTERY, 0x0B, d1, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_BATTERY, 0x0B, d1, out, sizeof out / sizeof out[0]);
      n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
      check_int("v17 merged run released", n, 5);
      check_int("v17 merged slot 0", out[0].slot, 0);
      check_int("v17 merged slot 1", out[1].slot, 1);
      check_int("v17 merged slot 2", out[2].slot, 0);
      check_int("v17 merged slot 3", out[3].slot, 1);
      check_int("v17 merged data 2", out[2].d[0], 0x4B);
      check_int("v17 merged data 3", out[3].d[0], 0x3C);
      check_int("v17 merged counts two", (int)(t.cyclesAccepted() - acc), 2);
      check_int("v17 merged not dropped", (int)(t.cyclesDropped() - drp), 0);
    }

    // An odd run is the case the rule exists for: a battery frame really is missing, so
    // every position after the gap would shift onto the wrong pack. Still dropped.
    t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
    t.feed(NBUS_NAD_BATTERY, 0x0B, d1, out, sizeof out / sizeof out[0]);
    t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
    n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    check_int("v17 odd run dropped", n, 1);

    // And past kMaxPerCycle the run no longer fits the buffer, so it is dropped whether
    // its length is a multiple or not.
    for (int i = 0; i < NBusCycleTracker::kMaxPerCycle + 1; ++i) {
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
    }
    n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    check_int("v17 overflow dropped", n, 1);
    check_int("v17 dropped after ovf", (int)t.cyclesDropped(), 10);

    // And it recovers: the next well-formed cycle is accepted again.
    t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
    t.feed(NBUS_NAD_BATTERY, 0x0B, d1, out, sizeof out / sizeof out[0]);
    n = t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    check_int("v17 recovered", n, 3);
    check_int("v17 accepted after ovf", (int)t.cyclesAccepted(), 4);
  }

  // --- Vector 18: a pack joining the bus must be noticed at once.
  // Taken from ring-accu2-online.txt, where the second pack came online mid-capture:
  // the cycle went from one battery response to two, and the pack answering first
  // changed from KAA1234567 to KAA7654321. Position 0 means a different pack from that
  // moment on, so the epoch must move and the stale slots must be dropped.
  {
    NBusCycleTracker t;
    NBusCycleTracker::Out out[NBusCycleTracker::kMaxPerCycle + 1];
    const uint8_t d0[4] = {0x4B, 0xFF, 0xFF, 0xFF};
    const uint8_t ds[4] = {0x05, 0x33, 0x00, 0x12};

    t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    for (int c = 0; c < 40; ++c) {  // one pack on the bus
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    }
    check_int("v18 one pack learned", t.expectedPerCycle(), 1);
    check_int("v18 epoch stable", (int)t.topologyEpoch(), 0);
    const uint32_t acc_before = t.cyclesAccepted();

    for (int c = 0; c < 400; ++c) {  // second pack joins
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    }
    check_int("v18 two packs learned", t.expectedPerCycle(), 2);
    check_int("v18 epoch moved", (int)t.topologyEpoch(), 1);
    // And it must resume accepting cycles, not stall for ever on the old length.
    check_true("v18 accepting again", t.cyclesAccepted() > acc_before + 100);
  }

  // --- Vector 19: the length histogram has to forget.
  // If it never decayed, a long-running device would keep voting for the cycle length
  // it saw during its first weeks and silently drop every cycle after a pack was added.
  // Here the old length is given a very long run, so only decay lets the new one win.
  {
    NBusCycleTracker t;
    NBusCycleTracker::Out out[NBusCycleTracker::kMaxPerCycle + 1];
    const uint8_t d0[4] = {0x4B, 0xFF, 0xFF, 0xFF};
    const uint8_t ds[4] = {0x05, 0x33, 0x00, 0x12};

    t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    for (int c = 0; c < 5000; ++c) {
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    }
    check_int("v19 long history len 1", t.expectedPerCycle(), 1);

    // A few hundred cycles of the new shape must be enough to switch over.
    for (int c = 0; c < 400; ++c) {
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_BATTERY, 0x0B, d0, out, sizeof out / sizeof out[0]);
      t.feed(NBUS_NAD_SOLAR, 0x02, ds, out, sizeof out / sizeof out[0]);
    }
    check_int("v19 relearned len 2", t.expectedPerCycle(), 2);
    check_int("v19 epoch moved", (int)t.topologyEpoch(), 1);
  }

  // --- Vector 20: forgetBatteries() clears the packs and spares the charger.
  {
    NBusParser p;
    const uint8_t soc[]  = {0x85, 0x06, 0xF4, 0x0B, 0x4B, 0xFF, 0xFF, 0xFF};
    const uint8_t sol[]  = {0x81, 0x06, 0xF4, 0x02, 0x05, 0x33, 0x00, 0x12};
    const uint8_t ssn[]  = {0x81, 0x06, 0xF4, 0x54, 0x00, 'A', 'C', 'D'};
    const uint8_t snum[] = {0x81, 0x06, 0xF4, 0x55, 0x00, 0x10, 0xF8, 0x9F};
    p.feedResponse(soc, sizeof soc, 0);
    p.feedResponse(soc, sizeof soc, 1);
    p.feedResponse(sol, sizeof sol, 0);
    p.feedResponse(ssn, sizeof ssn, 0);
    p.feedResponse(snum, sizeof snum, 0);
    check_str("v20 solar serial", p.state().solar.id.serial, "ACD1112223");

    p.forgetBatteries();
    check_true("v20 pack0 cleared", !p.state().batt[0].soc_valid);
    check_true("v20 pack1 cleared", !p.state().batt[1].soc_valid);
    check_true("v20 solar kept", p.state().solar.valid);
    check_str("v20 solar serial kept", p.state().solar.id.serial, "ACD1112223");
  }

  std::printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
  return g_failures == 0 ? 0 : 1;
}

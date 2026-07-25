// Host-side unit tests for NBusParser. Compile with plain g++ (no Arduino):
//   g++ -std=c++17 -I firmware test/test_parser.cpp firmware/NBusParser.cpp -o /tmp/t && /tmp/t

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
    check_near("v1 batt_voltage", p.state().batt_voltage, 13.30f);
    check_near("v1 batt_current", p.state().batt_current, -6.13f);
  }

  // --- Vector 2: battery V + I (higher discharge) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x02, 0x05, 0x30, 0x84, 0x73};
    check_true("v2 accepted", p.feedResponse(f, sizeof f));
    check_near("v2 batt_voltage", p.state().batt_voltage, 13.28f);
    check_near("v2 batt_current", p.state().batt_current, -11.39f);
  }

  // --- Vector 3: SoC ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x0B, 0x4B, 0xFF, 0xFF, 0xFF};
    check_true("v3 accepted", p.feedResponse(f, sizeof f));
    check_int("v3 batt_soc", p.state().batt_soc, 75);
  }

  // --- Vector 4: cell voltages (reg 0x57 -> cells 3,4) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x57, 0x0D, 0x23, 0x0D, 0x21};
    check_true("v4 accepted", p.feedResponse(f, sizeof f));
    check_near("v4 cell_v[2]", p.state().cell_v[2], 3.363f, 0.001f);
    check_near("v4 cell_v[3]", p.state().cell_v[3], 3.361f, 0.001f);
  }

  // --- Vector 5: solar V + I (charging, positive) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x81, 0x06, 0xF4, 0x02, 0x05, 0x33, 0x00, 0x12};
    check_true("v5 accepted", p.feedResponse(f, sizeof f));
    check_near("v5 solar_voltage", p.state().solar_voltage, 13.31f);
    check_near("v5 solar_current", p.state().solar_current, 0.18f);
  }

  // --- Vector 6: starter-battery voltage ---
  {
    NBusParser p;
    const uint8_t f[] = {0x81, 0x06, 0xF4, 0x01, 0x04, 0xF7, 0x00, 0x00};
    check_true("v6 accepted", p.feedResponse(f, sizeof f));
    check_near("v6 starter_voltage", p.state().starter_voltage, 12.71f);
  }

  // --- Vector 7: cell voltages (reg 0x56 -> cells 1,2) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x56, 0x0D, 0x24, 0x0D, 0x22};
    check_true("v7 accepted", p.feedResponse(f, sizeof f));
    check_near("v7 cell_v[0]", p.state().cell_v[0], 3.364f, 0.001f);
    check_near("v7 cell_v[1]", p.state().cell_v[1], 3.362f, 0.001f);
  }

  // --- Vector 8: remaining energy Wh (reg 0x36, big-endian u16) ---
  // 0x05CF = 1487 Wh (matches the app reading). Cross-confirmed via the BLE project.
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x36, 0x05, 0xCF, 0xFF, 0xFF};
    check_true("v8 accepted", p.feedResponse(f, sizeof f));
    check_int("v8 batt_wh", p.state().batt_wh, 1487);
  }

  // --- Vector 9: "quality" % (reg 0x0E, d0) ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x0E, 0x63, 0xFF, 0xFF, 0xFF};
    check_true("v9 accepted", p.feedResponse(f, sizeof f));
    check_int("v9 batt_quality", p.state().batt_quality, 99);
  }

  // --- Vector 10: nominal capacity Ah (reg 0x07, big-endian u16) — layout provisional ---
  {
    NBusParser p;
    const uint8_t f[] = {0x85, 0x06, 0xF4, 0x07, 0x00, 0x96, 0xFF, 0xFF};
    check_true("v10 accepted", p.feedResponse(f, sizeof f));
    check_int("v10 batt_capacity_ah", p.state().batt_capacity_ah, 150);
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

  std::printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
  return g_failures == 0 ? 0 : 1;
}

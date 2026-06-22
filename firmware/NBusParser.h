// NBusParser — platform-independent decoder for Büttner/Dometic NDS "N-Bus" frames.
//
// IMPORTANT: this file must NOT include any Arduino/ESP headers. It is compiled both
// into the firmware and into host-side unit tests with plain g++.

#ifndef NBUS_PARSER_H
#define NBUS_PARSER_H

#include <cstddef>
#include <cstdint>

// Node addresses (NAD) on the bus.
static constexpr uint8_t NBUS_NAD_BATTERY = 0x85;  // leisure battery (Tempra)
static constexpr uint8_t NBUS_NAD_SOLAR   = 0x81;  // solar charger (MPPT)

// Decoded snapshot of the bus. Each group carries a validity flag so consumers can
// tell fresh data from never-seen / stale registers.
struct NBusState {
  // Battery (NAD 0x85)
  float batt_voltage = 0.0f;  bool batt_voltage_valid = false;
  float batt_current = 0.0f;  bool batt_current_valid = false;  // negative = discharging
  int   batt_soc     = 0;     bool batt_soc_valid     = false;
  float cell_v[4]    = {0, 0, 0, 0};                             // cell voltages (V)
  bool  cell_valid[4]= {false, false, false, false};

  // Solar charger (NAD 0x81)
  float solar_voltage = 0.0f; bool solar_valid   = false;        // shared valid for V+I
  float solar_current = 0.0f;
  float starter_voltage = 0.0f; bool starter_valid = false;
};

// --- Free decode helpers (exposed so unit tests can hit them directly) ---

// Big-endian unsigned 16-bit from two bytes.
inline uint16_t nbus_u16(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

// Decode a centi-unit value (×0.01) where bit 15 is a sign flag (1 ⇒ negative).
// Used for battery/solar current.
inline float nbus_signed_centi(uint8_t hi, uint8_t lo) {
  uint16_t raw = nbus_u16(hi, lo);
  uint16_t mag = raw & 0x7FFF;
  float v = mag * 0.01f;
  return (raw & 0x8000) ? -v : v;
}

class NBusParser {
public:
  // Feed the 8 LIN data bytes of a slave-response frame (the bytes after the sync,
  // i.e. NAD PCI SID reg d0 d1 d2 d3). Returns true if a known register updated state.
  bool feedResponse(const uint8_t* data, size_t len);

  const NBusState& state() const { return state_; }

  // LIN "enhanced" checksum over PID + data bytes (inverted sum with carry).
  // Returns the expected checksum byte; compare against the frame's checksum.
  static uint8_t enhancedChecksum(uint8_t pid, const uint8_t* data, size_t len);

private:
  NBusState state_;

  bool decodeBattery(uint8_t reg, const uint8_t* d);
  bool decodeSolar(uint8_t reg, const uint8_t* d);
};

#endif  // NBUS_PARSER_H

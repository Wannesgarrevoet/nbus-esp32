#include "NBusParser.h"

namespace {
constexpr uint8_t PCI_SINGLE_FRAME = 0x06;  // single-frame PCI
constexpr uint8_t SID_READ_POS_RESP = 0xF4; // positive response to read (0xB4 + 0x40)
}  // namespace

bool NBusParser::feedResponse(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 8) return false;

  const uint8_t nad = data[0];
  const uint8_t pci = data[1];
  const uint8_t sid = data[2];
  const uint8_t reg = data[3];
  const uint8_t* d  = &data[4];  // d0 d1 d2 d3

  // Only handle single-frame positive read responses; ignore everything else.
  if (pci != PCI_SINGLE_FRAME || sid != SID_READ_POS_RESP) return false;

  switch (nad) {
    case NBUS_NAD_BATTERY: return decodeBattery(reg, d);
    case NBUS_NAD_SOLAR:   return decodeSolar(reg, d);
    default:               return false;
  }
}

bool NBusParser::decodeBattery(uint8_t reg, const uint8_t* d) {
  switch (reg) {
    case 0x02:  // Vh Vl Ih Il → voltage + current
      state_.batt_voltage = nbus_u16(d[0], d[1]) * 0.01f;
      state_.batt_voltage_valid = true;
      state_.batt_current = nbus_signed_centi(d[2], d[3]);
      state_.batt_current_valid = true;
      return true;

    case 0x0B:  // d0 → SoC %
      state_.batt_soc = d[0];
      state_.batt_soc_valid = true;
      return true;

    case 0x56:  // cell pair 1,2 (×0.001 V) — provisional layout
      state_.cell_v[0] = nbus_u16(d[0], d[1]) * 0.001f;
      state_.cell_v[1] = nbus_u16(d[2], d[3]) * 0.001f;
      state_.cell_valid[0] = state_.cell_valid[1] = true;
      return true;

    case 0x57:  // cell pair 3,4 (×0.001 V) — provisional layout
      state_.cell_v[2] = nbus_u16(d[0], d[1]) * 0.001f;
      state_.cell_v[3] = nbus_u16(d[2], d[3]) * 0.001f;
      state_.cell_valid[2] = state_.cell_valid[3] = true;
      return true;

    default:
      return false;  // 0x54 serial fragment etc. — ignored for now
  }
}

bool NBusParser::decodeSolar(uint8_t reg, const uint8_t* d) {
  switch (reg) {
    case 0x02:  // Vh Vl Ih Il → charger voltage + solar current
      state_.solar_voltage = nbus_u16(d[0], d[1]) * 0.01f;
      state_.solar_current = nbus_signed_centi(d[2], d[3]);
      state_.solar_valid = true;
      return true;

    case 0x01:  // Vh Vl → starter-battery voltage
      state_.starter_voltage = nbus_u16(d[0], d[1]) * 0.01f;
      state_.starter_valid = true;
      return true;

    default:
      return false;
  }
}

uint8_t NBusParser::enhancedChecksum(uint8_t pid, const uint8_t* data, size_t len) {
  uint16_t sum = pid;
  for (size_t i = 0; i < len; ++i) {
    sum += data[i];
    if (sum > 0xFF) sum -= 0xFF;  // carry wrap
  }
  return static_cast<uint8_t>(~sum);
}

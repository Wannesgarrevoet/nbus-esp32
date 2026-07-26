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

    case 0x0E:  // d0 → "quality" % (SoH-like)
      state_.batt_quality = d[0];
      state_.batt_quality_valid = true;
      return true;

    case 0x34: {
      // H1 = time to full, H2 = time to empty, both in minutes. Only the half matching
      // the current direction of travel is populated; the other reads 0xFFFF. Confirmed
      // for H2 by correlating it against remaining Wh / instantaneous power over a 300 s
      // capture: the two distributions agreed to 1 % with no fitted parameter.
      const uint16_t to_full  = nbus_u16(d[0], d[1]);
      const uint16_t to_empty = nbus_u16(d[2], d[3]);
      state_.batt_to_full_valid = (to_full != 0xFFFF);
      if (state_.batt_to_full_valid) state_.batt_to_full_min = to_full;
      state_.batt_to_empty_valid = (to_empty != 0xFFFF);
      if (state_.batt_to_empty_valid) state_.batt_to_empty_min = to_empty;
      return true;
    }

    case 0x35:  // cumulative charged / discharged energy, one count per Wh
      state_.batt_charged_wh = nbus_u16(d[0], d[1]);
      state_.batt_charged_valid = true;
      state_.batt_discharged_wh = nbus_u16(d[2], d[3]);
      state_.batt_discharged_valid = true;
      return true;

    case 0x36:  // Wh Wl → remaining energy (big-endian u16)
      state_.batt_wh = nbus_u16(d[0], d[1]);
      state_.batt_wh_valid = true;
      return true;

    case 0x07:  // nominal capacity (150 Ah) in the low half: 00 00 00 96
      state_.batt_capacity_ah = nbus_u16(d[2], d[3]);
      state_.batt_capacity_valid = true;
      return true;

    case 0x56:  // cells 1 & 2 (×0.001 V, big-endian) — cell order from BLE cross-check
      state_.cell_v[0] = nbus_u16(d[0], d[1]) * 0.001f;
      state_.cell_v[1] = nbus_u16(d[2], d[3]) * 0.001f;
      state_.cell_valid[0] = state_.cell_valid[1] = true;
      return true;

    case 0x57:  // cells 3 & 4 (×0.001 V, big-endian) — cell order from BLE cross-check
      state_.cell_v[2] = nbus_u16(d[0], d[1]) * 0.001f;
      state_.cell_v[3] = nbus_u16(d[2], d[3]) * 0.001f;
      state_.cell_valid[2] = state_.cell_valid[3] = true;
      return true;

    default:
      // 0x54 serial fragment, 0x34 (H1/H2 charge/discharge, meaning unknown), etc. —
      // ignored for now.
      return false;
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

uint8_t NBusParser::classicChecksum(const uint8_t* data, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len; ++i) {
    sum += data[i];
    if (sum > 0xFF) sum -= 0xFF;  // carry wrap
  }
  return static_cast<uint8_t>(~sum);
}

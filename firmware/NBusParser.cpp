#include "NBusParser.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint8_t PCI_SINGLE_FRAME = 0x06;  // single-frame PCI
constexpr uint8_t SID_READ_POS_RESP = 0xF4; // positive response to read (0xB4 + 0x40)
}  // namespace

bool NBusParser::feedResponse(const uint8_t* data, size_t len, int battSlot) {
  if (data == nullptr || len < 8) return false;

  const uint8_t nad = data[0];
  const uint8_t pci = data[1];
  const uint8_t sid = data[2];
  const uint8_t reg = data[3];
  const uint8_t* d  = &data[4];  // d0 d1 d2 d3

  // Only handle single-frame positive read responses; ignore everything else.
  if (pci != PCI_SINGLE_FRAME || sid != SID_READ_POS_RESP) return false;

  switch (nad) {
    case NBUS_NAD_BATTERY:
      if (battSlot < 0 || battSlot >= NBUS_MAX_BATTERIES) return false;
      return decodeBattery(reg, d, battSlot);
    case NBUS_NAD_SOLAR:
      return decodeSolar(reg, d);
    default:
      return false;
  }
}

void NBusParser::forgetBatteries() {
  for (int i = 0; i < NBUS_MAX_BATTERIES; ++i) state_.batt[i] = NBusBattery();
}

// Registers that describe the device rather than its measurements. Every decode here
// was checked against the app's device page: three devices, four fields each, all
// twelve matching.
bool NBusParser::decodeIdentity(NBusIdentity& id, uint8_t reg, const uint8_t* d,
                                bool* reordered) {
  if (reordered) *reordered = false;
  switch (reg) {
    case 0x54:  // D1..D3 → the three ASCII characters of the serial prefix
      id.prefix[0] = static_cast<char>(d[1]);
      id.prefix[1] = static_cast<char>(d[2]);
      id.prefix[2] = static_cast<char>(d[3]);
      id.prefix[3] = '\0';
      id.have_prefix = true;
      break;

    case 0x55:  // D1..D3 → the serial number as a 24-bit big-endian integer
      id.number = (static_cast<uint32_t>(d[1]) << 16) |
                  (static_cast<uint32_t>(d[2]) << 8) | d[3];
      id.have_number = true;
      break;

    case 0x60:  // D2 → bus address (the app's "Address": 1, 11, 13)
      // A changed address on a slot that already had one means the master reordered
      // its poll cycle — which happened when the packs were updated to firmware 5.1.
      // The caller has to discard that slot's readings: until the next full sweep of
      // registers arrives they belong to whichever pack used to sit here.
      if (reordered && id.address_valid && id.address != d[2]) *reordered = true;
      id.address = d[2];
      id.address_valid = true;
      return true;

    case 0xA0:  // D1 → IAD, D2D3 → model number
      id.iad = d[1];
      id.model = nbus_u16(d[2], d[3]);
      id.model_valid = true;
      return true;

    case 0xA1:  // D0.D1 → firmware version (5.1 on the packs, 5.4 on the charger)
      id.fw_major = d[0];
      id.fw_minor = d[1];
      id.fw_valid = true;
      return true;

    default:
      return false;
  }

  // Fell through from 0x54 / 0x55: publish the serial only once both halves are in.
  if (id.have_prefix && id.have_number) {
    std::snprintf(id.serial, sizeof id.serial, "%s%lu",
                  id.prefix, static_cast<unsigned long>(id.number));
  }
  return true;
}

bool NBusParser::decodeBattery(uint8_t reg, const uint8_t* d, int slot) {
  NBusBattery& b = state_.batt[slot];

  bool reordered = false;
  if (decodeIdentity(b.id, reg, d, &reordered)) {
    if (reordered) {
      // Keep the identity we have just decoded, drop every measurement: they describe
      // the pack that used to occupy this slot.
      NBusIdentity keep = b.id;
      b = NBusBattery();
      b.id = keep;
      b.id.have_prefix = b.id.have_number = false;
      b.id.serial[0] = '\0';
    }
    return true;
  }

  switch (reg) {
    case 0x02:  // Vh Vl Ih Il → voltage + current
      b.voltage = nbus_u16(d[0], d[1]) * 0.01f;
      b.voltage_valid = true;
      b.current = nbus_signed_centi(d[2], d[3]);
      b.current_valid = true;
      return true;

    case 0x0B:  // d0 → SoC %
      b.soc = d[0];
      b.soc_valid = true;
      return true;

    case 0x0E:  // d0 → "quality" % (SoH-like)
      b.quality = d[0];
      b.quality_valid = true;
      return true;

    case 0x34: {
      // H1 = time to full, H2 = time to empty, both in minutes. Only the half matching
      // the current direction of travel is populated; the other reads 0xFFFF. Confirmed
      // for H2 by correlating it against remaining Wh / instantaneous power over a 300 s
      // capture: the two distributions agreed to 1 % with no fitted parameter.
      const uint16_t to_full  = nbus_u16(d[0], d[1]);
      const uint16_t to_empty = nbus_u16(d[2], d[3]);
      b.to_full_valid = (to_full != 0xFFFF);
      if (b.to_full_valid) b.to_full_min = to_full;
      b.to_empty_valid = (to_empty != 0xFFFF);
      if (b.to_empty_valid) b.to_empty_min = to_empty;
      return true;
    }

    case 0x35:  // cumulative charged / discharged energy, one count per Wh
      b.charged_wh = nbus_u16(d[0], d[1]);
      b.charged_valid = true;
      b.discharged_wh = nbus_u16(d[2], d[3]);
      b.discharged_valid = true;
      return true;

    case 0x36:  // Wh Wl → remaining energy (big-endian u16)
      b.wh = nbus_u16(d[0], d[1]);
      b.wh_valid = true;
      return true;

    case 0x07:  // nominal capacity (150 Ah) in the low half: 00 00 00 96
      b.capacity_ah = nbus_u16(d[2], d[3]);
      b.capacity_valid = true;
      return true;

    case 0x56:  // cells 1 & 2 (×0.001 V, big-endian) — cell order from BLE cross-check
      b.cell_v[0] = nbus_u16(d[0], d[1]) * 0.001f;
      b.cell_v[1] = nbus_u16(d[2], d[3]) * 0.001f;
      b.cell_valid[0] = b.cell_valid[1] = true;
      return true;

    case 0x57:  // cells 3 & 4 (×0.001 V, big-endian) — cell order from BLE cross-check
      b.cell_v[2] = nbus_u16(d[0], d[1]) * 0.001f;
      b.cell_v[3] = nbus_u16(d[2], d[3]) * 0.001f;
      b.cell_valid[2] = b.cell_valid[3] = true;
      return true;

    default:
      return false;
  }
}

bool NBusParser::decodeSolar(uint8_t reg, const uint8_t* d) {
  NBusSolar& s = state_.solar;

  // 0x60 belongs to decodeIdentity — d2 really is the bus address — but on the charger d1
  // is not identity at all: it is the charge stage. Read it here first, so the register can
  // be both, and let decodeIdentity have the frame afterwards. The packs' 0x60 d1 is a flat
  // zero and is deliberately not read this way; only the charger puts a stage there.
  if (reg == 0x60) {
    s.stage = d[1];
    s.stage_valid = true;
  }

  if (decodeIdentity(s.id, reg, d, nullptr)) return true;

  switch (reg) {
    case 0x02:  // Vh Vl Ih Il → charger voltage + solar current
      s.voltage = nbus_u16(d[0], d[1]) * 0.01f;
      s.current = nbus_signed_centi(d[2], d[3]);
      s.valid = true;
      return true;

    case 0x01:  // Vh Vl → starter-battery voltage
      s.starter_voltage = nbus_u16(d[0], d[1]) * 0.01f;
      s.starter_valid = true;
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

// ---------------------------------------------------------------------------
// NBusCycleTracker
// ---------------------------------------------------------------------------

void NBusCycleTracker::noteLength(int len) {
  if (len <= 0 || len > kMaxPerCycle) return;
  lenHist_[len]++;
  lenTotal_++;
  // Age the histogram so it tracks the bus as it is now rather than as it once was.
  if (lenTotal_ >= kDecayAt) {
    lenTotal_ = 0;
    for (int i = 1; i <= kMaxPerCycle; ++i) {
      lenHist_[i] /= 2;
      lenTotal_ += lenHist_[i];
    }
  }
  if (lenTotal_ < static_cast<uint32_t>(kMinCycles)) return;
  // The cycle length is whichever run length dominates. Learning it rather than
  // hard-coding it is what let the split survive the update that changed the cycle
  // from two batteries every 200 ms to two every 240 ms in a different order — and
  // it is what will let it survive a third pack being added.
  int best = 1;
  for (int i = 2; i <= kMaxPerCycle; ++i) {
    if (lenHist_[i] > lenHist_[best]) best = i;
  }
  // A device joined or left. Every slot now points at a different pack than it did.
  if (expected_ != 0 && best != expected_) epoch_++;
  expected_ = best;
}

int NBusCycleTracker::feed(uint8_t nad, uint8_t reg, const uint8_t* d,
                           Out* out, int outMax) {
  if (out == nullptr || outMax <= 0) return 0;

  if (nad == NBUS_NAD_BATTERY) {
    if (nPending_ < kMaxPerCycle) {
      Out& o = pending_[nPending_];
      o.reg = reg;
      std::memcpy(o.d, d, 4);
      o.slot = -1;  // assigned when the run closes and its length is known
      nPending_++;
    } else {
      overflow_ = true;  // more merged cycles than we can hold; drop the run
    }
    return 0;
  }

  if (nad != NBUS_NAD_SOLAR) return 0;

  // An 0x81 frame closes the run that preceded it.
  int n = 0;
  if (started_ && !overflow_) {
    noteLength(nPending_);
    // A run of exactly one cycle, or of several merged by lost 0x81 frames, is
    // attributed by position modulo the cycle length. Anything else is dropped.
    if (expected_ > 0 && nPending_ > 0 && nPending_ % expected_ == 0) {
      for (int i = 0; i < nPending_ && n < outMax; ++i) {
        out[n] = pending_[i];
        out[n].slot = static_cast<int8_t>(i % expected_);
        n++;
      }
      accepted_ += static_cast<uint32_t>(nPending_ / expected_);
    } else if (nPending_ > 0) {
      dropped_++;
    }
  } else if (started_) {
    dropped_++;
  }
  nPending_ = 0;
  overflow_ = false;
  started_ = true;

  if (n < outMax) {
    Out& o = out[n++];
    o.reg = reg;
    std::memcpy(o.d, d, 4);
    o.slot = kSolarSlot;
  }
  return n;
}

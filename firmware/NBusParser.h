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

// Two Tempra packs share NAD 0x85 — the NAD identifies the device *type*, not the
// device. See NBusCycleTracker below for how they are told apart.
static constexpr int NBUS_MAX_BATTERIES = 2;

// Serial numbers are a 3-character prefix plus a number: "KAA1234567".
static constexpr int NBUS_SERIAL_LEN = 11;  // 10 characters + NUL

// Identity fields every device on the bus reports, decoded from registers that were
// confirmed against the Dometic Power app's device page (serial, IAD, model, firmware)
// and against the physical labels on the packs (serial).
struct NBusIdentity {
  char     serial[NBUS_SERIAL_LEN] = {0};  // empty until BOTH 0x54 and 0x55 have arrived
  uint8_t  address = 0;                    // reg 0x60 D2 — the app's "Address" field
  bool     address_valid = false;
  uint8_t  fw_major = 0, fw_minor = 0;     // reg 0xA1 D0.D1
  bool     fw_valid = false;
  uint8_t  iad = 0;                        // reg 0xA0 D1
  uint16_t model = 0;                      // reg 0xA0 D2D3
  bool     model_valid = false;

  bool serialValid() const { return serial[0] != '\0'; }

  // Serial arrives split over two registers, so both halves are held until they can
  // be joined. Kept here rather than in the parser so that each device accumulates
  // its own halves — with two packs interleaved on one bus, a single shared pair of
  // buffers would splice one pack's prefix onto the other's number.
  char     prefix[4] = {0};
  uint32_t number = 0;
  bool     have_prefix = false, have_number = false;
};

// One battery pack. Each group carries a validity flag so consumers can tell fresh
// data from never-seen registers.
struct NBusBattery {
  NBusIdentity id;

  float voltage = 0.0f;  bool voltage_valid = false;
  float current = 0.0f;  bool current_valid = false;  // negative = discharging
  int   soc     = 0;     bool soc_valid     = false;
  int   wh      = 0;     bool wh_valid      = false;  // remaining energy (Wh)
  int   quality = 0;     bool quality_valid = false;  // "quality" / SoH-like (%)
  int   capacity_ah = 0; bool capacity_valid = false; // nominal capacity (Ah)
  // Register 0x34 carries both runtime estimates; the one that does not apply to the
  // current direction of travel reads 0xFFFF, so each half needs its own validity flag.
  int   to_empty_min = 0; bool to_empty_valid = false;
  int   to_full_min  = 0; bool to_full_valid  = false;
  // Register 0x35: lifetime energy counters, one count per Wh.
  int   discharged_wh = 0; bool discharged_valid = false;
  int   charged_wh    = 0; bool charged_valid    = false;
  float cell_v[4]     = {0, 0, 0, 0};                  // cell voltages (V)
  bool  cell_valid[4] = {false, false, false, false};  // [0..1]=reg0x56, [2..3]=reg0x57
};

struct NBusSolar {
  NBusIdentity id;

  float voltage = 0.0f; bool valid = false;            // shared valid for V+I
  float current = 0.0f;
  float starter_voltage = 0.0f; bool starter_valid = false;

  // The charge stage as carried by 0x60 d1: 0 off, 3 bulk, 4 absorption, 6 float. The same
  // value sits in 0x26 d3, which the parser leaves to the register mirror. This copy has to
  // be decoded here instead, because 0x60 is an identity register and the mirror only ever
  // sees registers the parser rejects — mirroring it was tried first and would have left a
  // permanently unavailable entity.
  uint8_t stage = 0; bool stage_valid = false;
};

// Decoded snapshot of the whole bus.
struct NBusState {
  NBusBattery batt[NBUS_MAX_BATTERIES];
  NBusSolar   solar;
};

// --- Free decode helpers (exposed so unit tests can hit them directly) ---

// Big-endian unsigned 16-bit from two bytes.
inline uint16_t nbus_u16(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

// Decode a centi-unit value (×0.01) where bit 15 is a sign flag (1 ⇒ negative).
// Used for battery/solar current. NOT two's complement: reading it that way turns a
// 0.06 A trickle into −325 A, which is how the encoding was found in the first place.
inline float nbus_signed_centi(uint8_t hi, uint8_t lo) {
  uint16_t raw = nbus_u16(hi, lo);
  uint16_t mag = raw & 0x7FFF;
  float v = mag * 0.01f;
  return (raw & 0x8000) ? -v : v;
}

class NBusParser {
public:
  // Feed the 8 LIN data bytes of a slave-response frame (the bytes after the sync,
  // i.e. NAD PCI SID reg d0 d1 d2 d3). `battSlot` selects which pack a NAD 0x85 frame
  // belongs to and is ignored for other NADs; see NBusCycleTracker for how it is
  // derived. Returns true if a known register updated state.
  bool feedResponse(const uint8_t* data, size_t len, int battSlot = 0);

  const NBusState& state() const { return state_; }

  // Discard everything known about the battery packs. Call this when the poll cycle
  // changes shape (see NBusCycleTracker::topologyEpoch): the slots have been reshuffled,
  // so every stored value belongs to a pack that is no longer in that slot. The solar
  // charger is untouched — it is identified by its own NAD, never by position.
  void forgetBatteries();

  // LIN "classic" checksum over the data bytes only (inverted sum with carry).
  // Diagnostic frames (ID 0x3C/0x3D) always use classic, never enhanced — the PID is
  // excluded from the sum. Returns the expected checksum byte.
  static uint8_t classicChecksum(const uint8_t* data, size_t len);

private:
  NBusState state_;

  bool decodeBattery(uint8_t reg, const uint8_t* d, int slot);
  bool decodeSolar(uint8_t reg, const uint8_t* d);
  // Registers shared by every device type: serial, address, model, firmware.
  bool decodeIdentity(NBusIdentity& id, uint8_t reg, const uint8_t* d, bool* reordered);
};

// Tells the two battery packs apart.
//
// Both answer on NAD 0x85, so the NAD is useless for this. What separates them is
// position in the poll cycle: the master interrogates the devices in a fixed order,
// and the solar charger's NAD 0x81 frame marks where one cycle ends and the next
// begins. The n-th battery response after an 0x81 frame is always the same pack.
//
// An earlier attempt keyed on elapsed time instead ("within 90 ms of the 0x81 frame
// ⇒ pack two"). It looked convincing and was wrong twice over: a battery firmware
// update moved the slots and emptied one group entirely, and even after re-tuning,
// roughly 9% of frames still landed on the wrong side of the threshold whenever a
// pack answered late. Position has no such grey zone.
//
// The one failure mode left is a lost frame. If a battery frame goes missing the run
// comes up short and every position after the gap is shifted onto the wrong pack, so a
// run whose length is not a whole number of cycles is discarded rather than attributed.
// Dropping a frame costs one sample out of thousands. Misattributing one silently
// corrupts a register table, and nothing downstream would ever flag it.
//
// A *merged* run is a different case, and it is the common one. Measured over four
// two-pack captures — about 4900 runs — every single run was an even multiple of the
// two-pack cycle: 2, 4, 6 or 8, never 3, 5 or 7. The one odd run in the set was the
// truncated first run of a ring buffer. The gap timing says the same thing: inside a
// pair the frames are 60 ms apart, and where a run of 4 joins two pairs the gap is
// 189 ms — exactly the two gaps that would have straddled a missing 0x81. So what goes
// missing is the charger frame, not a battery frame, and a run of 4 is simply two
// cycles with nothing lost from either.
//
// Those runs are therefore attributed by position modulo the cycle length instead of
// being thrown away. The check that this is sound: inside the merged runs of one
// capture, 143 frames carried an identity register (0x54/0x55/0x60/0x90/0xA0/0xA1)
// whose value is fixed per pack, and all 143 landed on the pack that value belongs to,
// with none wrong. It recovers about one frame in six.
//
// What would break it is a battery frame going missing *and* an 0x81 frame going
// missing in the same run, in a pattern that leaves the length a multiple of the cycle
// anyway. That needs at least two coincident losses of a kind we have never once
// observed — the parity above says battery frames are not being lost at all — whereas
// the old rule was paying a sixth of the data every day against it.
class NBusCycleTracker {
public:
  // Deep enough to hold several merged cycles. Runs of 8 occur, and one capture had a
  // 12. Beyond this the run is treated as overflow and dropped, which is the safe side.
  static constexpr int  kMaxPerCycle = 12;  // battery responses buffered per run
  static constexpr int  kMinCycles   = 8;   // observed cycles before the length is trusted
  static constexpr int8_t kSolarSlot = -1;
  // The length histogram is halved once it reaches this many observations, so recent
  // cycles outweigh old ones. Without it the histogram remembers for ever: a pack added
  // after a week of running would be outvoted by a week of two-pack cycles, and every
  // cycle would be dropped from then on. That fails as silent staleness — sensors stop
  // updating with nothing logged — which is the worst shape a bug can take here.
  static constexpr uint32_t kDecayAt = 256;

  struct Out {
    uint8_t reg;
    uint8_t d[4];
    int8_t  slot;  // 0..NBUS_MAX_BATTERIES-1, or kSolarSlot
  };

  // Feed one checksum-valid slave response. Battery frames are buffered until their
  // cycle closes, so this returns 0 for them; the 0x81 frame that closes a cycle
  // returns the whole cycle at once, followed by the solar frame itself. Writes at
  // most `outMax` entries and returns how many.
  int feed(uint8_t nad, uint8_t reg, const uint8_t* d, Out* out, int outMax);

  int      expectedPerCycle() const { return expected_; }
  uint32_t cyclesAccepted() const { return accepted_; }
  uint32_t cyclesDropped() const  { return dropped_; }

  // Increments whenever the learned cycle length changes, i.e. whenever a device joins
  // or leaves the bus. Slot N before the change and slot N after it are not the same
  // pack — observed directly in the capture where the second pack came online: the pack
  // answering first went from KAA1234567 to KAA7654321 at that moment.
  //
  // A slot's bus address (reg 0x60) also reveals the swap, but only when a 0x60 frame
  // happens along, which is seconds later. Anything published in between would carry
  // the wrong pack's name. Consumers should watch this counter and drop every slot the
  // moment it moves.
  uint32_t topologyEpoch() const { return epoch_; }

private:
  void noteLength(int len);

  Out      pending_[kMaxPerCycle];
  int      nPending_ = 0;
  bool     overflow_ = false;      // more battery frames this cycle than we can hold
  bool     started_  = false;      // ignore the partial run we join mid-cycle
  uint16_t lenHist_[kMaxPerCycle + 1] = {0};
  uint32_t lenTotal_ = 0;
  int      expected_ = 0;
  uint32_t accepted_ = 0, dropped_ = 0;
  uint32_t epoch_ = 0;
};

#endif  // NBUS_PARSER_H

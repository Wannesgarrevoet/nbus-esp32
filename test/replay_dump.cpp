// Replays a raw bus dump through the exact code the firmware runs.
//
// The unit tests prove the parser against hand-built frames; this proves it against
// what the bus actually sent. The decisive check is at the end: each poll slot must
// carry exactly ONE serial number. A slot holding two serials means frames from the
// two packs were mixed, which is the failure the whole ordinal-position scheme exists
// to prevent — and the failure that a timing-based split produced on ~9% of frames.
//
//   g++ -std=c++17 -I firmware test/replay_dump.cpp firmware/NBusParser.cpp -o replay
//   ./replay captures/ring-post-fwupdate.txt

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "NBusParser.h"

namespace {

struct Frame {
  uint8_t nad, reg, d[4];
};

// Dump lines are "t_ms NAD PCI SID REG D0 D1 D2 D3", hex except the timestamp.
// Anything else (headers, comments, truncated tails) is skipped.
bool parseLine(const char* line, Frame& f) {
  unsigned t, nad, pci, sid, reg, d0, d1, d2, d3;
  if (std::sscanf(line, "%u %x %x %x %x %x %x %x %x",
                  &t, &nad, &pci, &sid, &reg, &d0, &d1, &d2, &d3) != 9) {
    return false;
  }
  f.nad = static_cast<uint8_t>(nad);
  f.reg = static_cast<uint8_t>(reg);
  f.d[0] = static_cast<uint8_t>(d0);
  f.d[1] = static_cast<uint8_t>(d1);
  f.d[2] = static_cast<uint8_t>(d2);
  f.d[3] = static_cast<uint8_t>(d3);
  return true;
}

void printIdentity(const char* label, const NBusIdentity& id) {
  std::printf("  %-10s serial %-12s adres %-4s fw %-6s IAD %-4s model %s\n", label,
              id.serialValid() ? id.serial : "-",
              id.address_valid ? std::to_string(id.address).c_str() : "-",
              id.fw_valid ? (std::to_string(id.fw_major) + "." +
                             std::to_string(id.fw_minor)).c_str() : "-",
              id.model_valid ? std::to_string(id.iad).c_str() : "-",
              id.model_valid ? std::to_string(id.model).c_str() : "-");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: replay <dump.txt> [dump.txt ...]\n");
    return 2;
  }

  int failures = 0;

  for (int a = 1; a < argc; ++a) {
    std::FILE* fp = std::fopen(argv[a], "r");
    if (fp == nullptr) {
      std::fprintf(stderr, "cannot open %s\n", argv[a]);
      return 2;
    }

    NBusParser parser;
    NBusCycleTracker tracker;
    NBusCycleTracker::Out out[NBusCycleTracker::kMaxPerCycle + 1];

    // Per slot: which serial numbers (reg 0x55) and which bus addresses (reg 0x60 D2)
    // were seen. Both must be single-valued if the attribution is sound.
    std::map<int, std::set<uint32_t>> serials;
    std::map<int, std::set<int>> addrs;
    std::map<int, long> frames;

    std::printf("\n=== %s\n", argv[a]);

    long lines = 0, parsed = 0, attributed = 0;
    uint32_t epoch = tracker.topologyEpoch();
    char buf[512];
    while (std::fgets(buf, sizeof buf, fp) != nullptr) {
      ++lines;
      Frame f;
      if (!parseLine(buf, f)) continue;
      ++parsed;

      int n = tracker.feed(f.nad, f.reg, f.d, out, sizeof out / sizeof out[0]);

      // A device joined or left: the slots mean something else from here on, so both
      // the decoded state and this harness's bookkeeping start over.
      if (tracker.topologyEpoch() != epoch) {
        epoch = tracker.topologyEpoch();
        parser.forgetBatteries();
        serials.clear();
        addrs.clear();
        frames.clear();
        std::printf("  -- topologie gewijzigd (epoch %u): %d accu-antwoorden per cyclus,"
                    " alle slots gewist\n", epoch, tracker.expectedPerCycle());
      }
      for (int i = 0; i < n; ++i) {
        const int slot = out[i].slot;
        ++attributed;
        frames[slot]++;
        if (out[i].reg == 0x55) {
          serials[slot].insert((static_cast<uint32_t>(out[i].d[1]) << 16) |
                               (static_cast<uint32_t>(out[i].d[2]) << 8) | out[i].d[3]);
        }
        if (out[i].reg == 0x60) addrs[slot].insert(out[i].d[2]);

        const uint8_t nad = (slot == NBusCycleTracker::kSolarSlot) ? NBUS_NAD_SOLAR
                                                                  : NBUS_NAD_BATTERY;
        const uint8_t full[8] = {nad, 0x06, 0xF4, out[i].reg,
                                 out[i].d[0], out[i].d[1], out[i].d[2], out[i].d[3]};
        parser.feedResponse(full, sizeof full,
                            slot == NBusCycleTracker::kSolarSlot ? 0 : slot);
      }
    }
    std::fclose(fp);

    const uint32_t acc = tracker.cyclesAccepted();
    const uint32_t drp = tracker.cyclesDropped();
    std::printf("  %ld regels, %ld frames, %ld toegewezen\n", lines, parsed, attributed);
    std::printf("  pollcyclus: %d accu-antwoorden; %u cycli aanvaard, %u verworpen "
                "(%.2f%%)\n",
                tracker.expectedPerCycle(), acc, drp,
                (acc + drp) ? 100.0 * drp / (acc + drp) : 0.0);

    for (int s = 0; s < NBUS_MAX_BATTERIES; ++s) {
      char label[16];
      std::snprintf(label, sizeof label, "accu %d", s + 1);
      printIdentity(label, parser.state().batt[s].id);
    }
    printIdentity("zonlader", parser.state().solar.id);

    // The cross-check. One slot, one pack.
    for (const auto& kv : serials) {
      const int slot = kv.first;
      const char* what = slot == NBusCycleTracker::kSolarSlot ? "zonlader" : "accu";
      if (kv.second.size() == 1) {
        std::printf("  OK   slot %d (%s): %ld frames, 1 serienummer (%u)\n", slot, what,
                    frames[slot], *kv.second.begin());
      } else {
        std::printf("  FAIL slot %d (%s): %zu serienummers in een slot ->",
                    slot, what, kv.second.size());
        for (uint32_t v : kv.second) std::printf(" %u", v);
        std::printf("\n");
        ++failures;
      }
      if (addrs[slot].size() > 1) {
        std::printf("  FAIL slot %d: %zu busadressen in een slot\n", slot,
                    addrs[slot].size());
        ++failures;
      }
    }
  }

  std::printf("\n%s\n", failures == 0 ? "REPLAY OK" : "REPLAY GEFAALD");
  return failures == 0 ? 0 : 1;
}

#!/usr/bin/env python3
"""Tabulate every N-Bus register seen in a raw serial capture.

The firmware's NBUS_DEBUG dump prints one "[raw N] ..." line per frame window. The
decoded "[lin] ..." lines only cover registers the parser already knows, so the raw
windows are the only place unmapped registers show up. This script rescans those
windows, keeps the frames whose LIN classic checksum holds, and reports what each
(NAD, register) pair actually carries over time.

Usage:
  python tools/analyse_raw.py <capture.log>              summary per register
  python tools/analyse_raw.py <capture.log> NAD:REG      time series for one register,
                                                         next to the battery state that
                                                         was current when it arrived
                                                         (e.g. 85:34 or 81:1B)
"""

import collections
import re
import sys

# A slave response is 0x55 (sync) 0x7D (PID 0x3D + parity), then NAD PCI SID reg d0..d3
# and the checksum. Master requests use PID 0x3C and carry no measurements.
SYNC, PID_RESPONSE, SID_RESPONSE = 0x55, 0x7D, 0xF4
FRAME_LEN = 9  # 8 data bytes + checksum

NAD_NAMES = {0x81: "solar", 0x85: "battery", 0x80: "broadcast"}

# Registers the firmware already decodes, so the report can separate "known" from
# "still to map" without duplicating the parser.
KNOWN = {
    (0x85, 0x02): "battery V + I",
    (0x85, 0x07): "nominal capacity Ah",
    (0x85, 0x0B): "state of charge %",
    (0x85, 0x0E): "quality %",
    (0x85, 0x36): "remaining energy Wh",
    (0x85, 0x54): "serial fragment",
    (0x85, 0x56): "cell voltages 1,2",
    (0x85, 0x57): "cell voltages 3,4",
    (0x81, 0x01): "starter battery V",
    (0x81, 0x02): "charge V + I",
    (0x81, 0x54): "serial fragment",
}


def classic_checksum(data):
    """LIN classic checksum: inverted sum-with-carry over the data bytes, PID excluded."""
    total = 0
    for b in data:
        total += b
        if total > 0xFF:
            total -= 0xFF
    return (~total) & 0xFF


def extract_frames(path):
    """Yield (nad, reg, (d0, d1, d2, d3)) for every checksum-valid response."""
    hex_pair = re.compile(r"\b[0-9A-F]{2}\b")
    kept = dropped = 0

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if "[raw" not in line:
                continue
            body = line.split("]", 1)[1] if "]" in line else line
            buf = [int(t, 16) for t in hex_pair.findall(body)]

            # Scan the whole window: the 64-byte forced flushes hold several frames.
            i = 0
            while i + 1 < len(buf):
                if buf[i] != SYNC or buf[i + 1] != PID_RESPONSE:
                    i += 1
                    continue
                frame = buf[i + 2 : i + 2 + FRAME_LEN]
                if len(frame) < FRAME_LEN:
                    break
                if classic_checksum(frame[:8]) != frame[8]:
                    dropped += 1
                    i += 1
                    continue
                nad, _pci, sid, reg = frame[0], frame[1], frame[2], frame[3]
                if sid != SID_RESPONSE:
                    i += 1
                    continue
                kept += 1
                yield nad, reg, tuple(frame[4:8])
                i += 2 + FRAME_LEN

    print(f"frames accepted: {kept}   checksum rejects: {dropped}", file=sys.stderr)


def u16(hi, lo):
    return (hi << 8) | lo


def signed_centi(hi, lo):
    """Centi-unit value where bit 15 is a sign flag, matching nbus_signed_centi()."""
    raw = u16(hi, lo)
    mag = (raw & 0x7FFF) * 0.01
    return -mag if raw & 0x8000 else mag


class BusState:
    """Running snapshot of the registers the firmware already decodes.

    An unknown register only becomes interpretable next to what the system was doing
    at that moment, and the bus interleaves every register into one stream, so the most
    recent known values are the best timestamp available. Decoding here mirrors
    NBusParser exactly; any divergence would invent a correlation that isn't there.
    """

    def __init__(self):
        self.volt = self.amp = self.soc = self.wh = None
        self.pv_volt = self.pv_amp = None

    def update(self, nad, reg, d):
        if nad == 0x85:
            if reg == 0x02:
                self.volt = u16(d[0], d[1]) * 0.01
                self.amp = signed_centi(d[2], d[3])
            elif reg == 0x0B:
                self.soc = d[0]
            elif reg == 0x36:
                self.wh = u16(d[0], d[1])
        elif nad == 0x81 and reg == 0x02:
            self.pv_volt = u16(d[0], d[1]) * 0.01
            self.pv_amp = signed_centi(d[2], d[3])

    def __str__(self):
        def fmt(v, spec, unit):
            return f"{v:{spec}}{unit}" if v is not None else " " * (len(unit) + 4) + "?"

        return "  ".join(
            (
                fmt(self.volt, "6.2f", "V"),
                fmt(self.amp, "6.2f", "A"),
                fmt(self.soc, "3d", "%"),
                fmt(self.wh, "5d", "Wh"),
                fmt(self.pv_volt, "6.2f", "V"),
                fmt(self.pv_amp, "5.2f", "A"),
            )
        )


def describe(values):
    """Summarise one register's payloads: how constant, and how they move."""
    n = len(values)
    distinct = len(set(values))
    first, last = values[0], values[-1]

    parts = [f"{n:5d} frames", f"{distinct:4d} distinct"]

    if distinct == 1:
        parts.append("CONSTANT")
    else:
        # Both 16-bit halves, big-endian, are the layout every mapped register uses.
        hi_vals = [u16(v[0], v[1]) for v in values]
        lo_vals = [u16(v[2], v[3]) for v in values]
        for label, vals in (("hi16", hi_vals), ("lo16", lo_vals)):
            if len(set(vals)) > 1:
                parts.append(
                    f"{label} {min(vals)}..{max(vals)} (0x{min(vals):04X}..0x{max(vals):04X})"
                )
    parts.append("first " + " ".join(f"{b:02X}" for b in first))
    parts.append("last " + " ".join(f"{b:02X}" for b in last))
    return "  ".join(parts)


def timeseries(path, nad_want, reg_want):
    """Print one register's payloads over time, next to the concurrent battery state.

    Consecutive identical payloads are collapsed into a repeat count: a register that
    only moves twice in five minutes would otherwise bury those two moments under
    hundreds of identical lines.
    """
    state = BusState()
    print(
        f"# {NAD_NAMES.get(nad_want, hex(nad_want))} 0x{reg_want:02X}\n"
        f"#    n  raw            hi16    lo16     |  batt V   batt A  SoC     Wh"
        f"    pv V    pv A"
    )

    prev = None
    count = 0
    shown = 0

    def flush(data, repeats, at):
        nonlocal shown
        raw = " ".join(f"{b:02X}" for b in data)
        hi, lo = u16(data[0], data[1]), u16(data[2], data[3])
        tag = f"x{repeats}" if repeats > 1 else "  "
        print(f"{shown:5d}  {raw}  {hi:6d}  {lo:6d} {tag} |  {at}")
        shown += 1

    for nad, reg, data in extract_frames(path):
        state.update(nad, reg, data)
        if (nad, reg) != (nad_want, reg_want):
            continue
        if data == prev:
            count += 1
            continue
        if prev is not None:
            flush(prev, count, at_state)
        prev, count, at_state = data, 1, str(state)

    if prev is not None:
        flush(prev, count, at_state)
    return 0


def main():
    if len(sys.argv) == 3:
        try:
            nad_s, reg_s = sys.argv[2].split(":")
            return timeseries(sys.argv[1], int(nad_s, 16), int(reg_s, 16))
        except ValueError:
            print("second argument must look like NAD:REG in hex, e.g. 85:34")
            return 1
    if len(sys.argv) != 2:
        print(__doc__)
        return 1

    seen = collections.defaultdict(list)
    for nad, reg, data in extract_frames(sys.argv[1]):
        seen[(nad, reg)].append(data)

    for title, wanted in (("KNOWN REGISTERS", True), ("UNMAPPED REGISTERS", False)):
        keys = [k for k in sorted(seen) if (k in KNOWN) == wanted]
        if not keys:
            continue
        print(f"\n=== {title} ===")
        for key in keys:
            nad, reg = key
            name = KNOWN.get(key, "")
            label = f"{NAD_NAMES.get(nad, hex(nad))} 0x{reg:02X}"
            print(f"{label:16s} {name:22s} {describe(seen[key])}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

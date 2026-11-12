#!/usr/bin/env python3
"""
Analyses a Saleae digital capture (CSV export) of the selector link.

Usage:
    python docs/analyze_selector_capture.py docs/digital.csv [--frames frames.csv]

Expected capture (Saleae Logic 2: export raw digital data, CSV):
    Channel 0 = selector replies (selector TX)       UART 9600 8N1
    Channel 1 = host frames (ESP32 TX)               UART 9600 8N1
    Channel 2 = SEL key contact                      0 = pressed
    Channel 3 = OK key contact                       0 = pressed
    Channel 4 = + key contact                        0 = pressed
    Channel 5 = - key contact                        0 = pressed
Change the constants below for another wiring.

What it reports:
    - the frames on both UART lines (checksum checked) and anything that does not decode
    - exact reply latency, host cadence and gaps between bytes
    - replies that nobody asked for, and the silence before the host starts
    - the (level, pulse low, pulse high) values the selector reports for each key
    - when the selector samples the keys, and how many physical presses it can see by duration
    - whether the pulse bits show a press that the level bits missed

Protocol: docs/selector-protocol.md. Only the Python standard library is used.
"""

import argparse
import bisect
import csv
import statistics
import sys
from collections import Counter, namedtuple

BAUD = 9600
BIT = 1.0 / BAUD

CH_REPLY, CH_HOST = 0, 1
KEY_CHANNELS = {"SEL": 2, "OK": 3, "+": 4, "-": 5}

# Where each key sits in the reply: (data byte index, level bit). The pulse low bit is
# one below the level bit and the pulse high bit two below (docs/selector-protocol.md).
KEY_BITS = {"-": (0, 7), "+": (0, 4), "OK": (1, 7), "SEL": (1, 4)}

START, STOP, STOP_ALT = 0x7E, 0x81, 0x51
MIN_FRAME, MAX_FRAME = 6, 64
FUNC_NAMES = {0x53: "S", 0x3F: "?", 0x42: "B", 0x4C: "L"}

Frame = namedtuple("Frame", "t_start t_end raw max_gap")


# ─── Loading ────────────────────────────────────────────────────────────────

def load(path):
    times, states = [], []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            times.append(float(row[0]))
            states.append([int(x) for x in row[1:9]])
    return times, states


class Line:
    """One digital channel as a list of edges."""

    def __init__(self, times, states, ch):
        self.t = [times[0]]
        self.v = [states[0][ch]]
        for i in range(1, len(times)):
            if states[i][ch] != states[i - 1][ch]:
                self.t.append(times[i])
                self.v.append(states[i][ch])

    def at(self, t):
        i = bisect.bisect_right(self.t, t) - 1
        return self.v[i] if i >= 0 else self.v[0]


# ─── UART and frames ────────────────────────────────────────────────────────

def decode_uart(line):
    """9600 8N1, idle high, LSB first. Returns (t_start, t_end, byte, stop_ok)."""
    out = []
    resume = -1.0
    for i in range(1, len(line.t)):
        t, v = line.t[i], line.v[i]
        if v != 0 or t < resume:
            continue
        if line.at(t + 0.5 * BIT) != 0:
            continue  # glitch, not a start bit
        value = 0
        for k in range(8):
            if line.at(t + (k + 1.5) * BIT):
                value |= 1 << k
        stop_ok = line.at(t + 9.5 * BIT) == 1
        out.append((t, t + 10 * BIT, value, stop_ok))
        resume = t + 9.5 * BIT
    return out


def checksum(raw):
    x = 0
    for b in raw[:-2]:
        x ^= b
    return x ^ 0xFF


def assemble(decoded):
    """Same rules as the firmware framer. Returns (frames, drops)."""
    frames, drops = [], Counter()
    buf = []
    for t_start, t_end, value, stop_ok in decoded:
        if not stop_ok:
            drops["uart framing error"] += 1
        buf.append((t_start, t_end, value))
        while buf:
            if buf[0][2] != START:
                buf.pop(0)
                continue
            if len(buf) < 2:
                break
            size = buf[1][2]
            if size < MIN_FRAME or size > MAX_FRAME:
                drops["bad size"] += 1
                buf.pop(0)
                continue
            if len(buf) < size:
                break
            raw = [x[2] for x in buf[:size]]
            if raw[-1] not in (STOP, STOP_ALT):
                drops["bad stop byte"] += 1
                buf.pop(0)
                continue
            if checksum(raw) != raw[-2]:
                drops["bad checksum"] += 1
                buf.pop(0)
                continue
            gaps = [buf[i + 1][0] - buf[i][1] for i in range(size - 1)]
            frames.append(Frame(buf[0][0], buf[size - 1][1], raw, max(gaps) if gaps else 0.0))
            del buf[:size]
    return frames, drops


def func_name(frame):
    return FUNC_NAMES.get(frame.raw[3], "0x%02X" % frame.raw[3])


# ─── Statistics helpers ─────────────────────────────────────────────────────

def summarize(values, unit="ms", scale=1000.0):
    if not values:
        return "n=0"
    v = sorted(x * scale for x in values)
    p95 = v[min(len(v) - 1, int(round(0.95 * (len(v) - 1))))]
    return "n=%d min=%.2f median=%.2f p95=%.2f max=%.2f %s" % (
        len(v), v[0], statistics.median(v), p95, v[-1], unit)


def section(title):
    print()
    print("=" * 78)
    print(title)
    print("=" * 78)


# ─── Analysis ───────────────────────────────────────────────────────────────

def report_frames(host, replies, host_drops, reply_drops, t_end):
    section("1. Frames on the wire")
    for name, frames, drops in (("Host (ch1)", host, host_drops), ("Selector replies (ch0)", replies, reply_drops)):
        kinds = Counter(func_name(f) + "/%d" % len(f.raw) for f in frames)
        print("%s: %d valid frames  %s" % (name, len(frames), dict(kinds)))
        print("    not decoded: %s" % (dict(drops) if drops else "none"))
        if frames:
            print("    first frame at %.3f s, last at %.3f s (capture ends at %.3f s)" % (
                frames[0].t_start, frames[-1].t_start, t_end))
    print("Bytes of one frame are sent back to back? largest gap inside a frame:")
    print("    host:    %.1f us   replies: %.1f us   (one bit = %.1f us)" % (
        max((f.max_gap for f in host), default=0) * 1e6,
        max((f.max_gap for f in replies), default=0) * 1e6, BIT * 1e6))


def pair_replies(host, replies):
    """Matches each host frame with the first reply that starts after it ends."""
    pairs, used = [], set()
    ri = 0
    for i, h in enumerate(host):
        limit = host[i + 1].t_start if i + 1 < len(host) else float("inf")
        while ri < len(replies) and replies[ri].t_start < h.t_end - 1e-6:
            ri += 1
        if ri < len(replies) and replies[ri].t_start < limit:
            pairs.append((h, replies[ri]))
            used.add(ri)
            ri += 1
        else:
            pairs.append((h, None))
    unsolicited = [r for i, r in enumerate(replies) if i not in used]
    return pairs, unsolicited


def report_timing(host, replies):
    section("2. Timing")
    pairs, unsolicited = pair_replies(host, replies)

    by_func = {}
    missing = 0
    for h, r in pairs:
        if r is None:
            missing += 1
            continue
        by_func.setdefault(func_name(h), []).append(r.t_start - h.t_end)
    print("Reply latency (end of the host frame to the start bit of the reply):")
    for f, vals in sorted(by_func.items()):
        print("    after '%s': %s" % (f, summarize(vals)))
    print("Host frames without a reply: %d of %d" % (missing, len(host)))
    for h, r in pairs:
        if r is None:
            print("    no reply to '%s' at %.4f s" % (func_name(h), h.t_start))
    print("Replies that follow no host frame (unsolicited): %d" % len(unsolicited))
    if unsolicited:
        for r in unsolicited[:5]:
            print("    at %.4f s: %s" % (r.t_start, " ".join("%02X" % b for b in r.raw)))

    starts = [h.t_start for h in host]
    cadence = [b - a for a, b in zip(starts, starts[1:])]
    print("Host cadence (start to start): %s" % summarize(cadence))
    if cadence:
        off = [c for c in cadence if abs(c - 0.1) > 0.002]
        print("    stdev %.2f ms; %d of %d intervals differ from 100 ms by more than 2 ms" % (
            statistics.pstdev(cadence) * 1000, len(off), len(cadence)))
        late = sum(1 for c in cadence if 0.105 <= c <= 0.15)
        early = sum(1 for c in cadence if 0.05 <= c <= 0.095)
        print("    intervals of 105-150 ms: %d, of 50-95 ms: %d" % (late, early))
        for a, c in zip(starts, cadence):
            if c > 0.15:
                print("    gap of %.1f ms after the host frame at %.4f s" % (c * 1000, a))

    airtime = {}
    for h, _ in pairs:
        airtime.setdefault(func_name(h), []).append(h.t_end - h.t_start)
    for f, vals in sorted(airtime.items()):
        print("Time on the wire of a host '%s' frame: %s" % (f, summarize(vals)))


def key_levels(reply):
    """(level, pulse low, pulse high) per key from an 8-byte reply."""
    out = {}
    for key, (idx, lvl) in KEY_BITS.items():
        d = reply.raw[4 + idx]
        out[key] = ((d >> lvl) & 1, (d >> (lvl - 1)) & 1, (d >> (lvl - 2)) & 1)
    return out


def report_key_encoding(replies):
    section("3. What the selector reports for each key (level, pulse low, pulse high)")
    census = {k: Counter() for k in KEY_BITS}
    for r in replies:
        if len(r.raw) != 8 or r.raw[3] not in (0x53, 0x3F):
            continue
        for key, triple in key_levels(r).items():
            census[key][triple] += 1
    for key in ("-", "+", "OK", "SEL"):
        print("  %-3s: %s" % (key, ", ".join("%s x%d" % (t, n) for t, n in sorted(census[key].items()))))
    print("  Expected if the pulse bits are a 'last edge' indicator: pressed (0, 1, 0),")
    print("  released after an edge (1, 0, 1), released with no edge seen since power-up (1, 0, 0).")
    odd = Counter()
    for key in census:
        for t, n in census[key].items():
            if t not in ((0, 1, 0), (1, 0, 1), (1, 0, 0)):
                odd[(key, t)] += n
    print("  Any other combination: %s" % (dict(odd) if odd else "none"))

    first = next((r for r in replies if len(r.raw) == 8 and r.raw[3] in (0x53, 0x3F)), None)
    if first:
        print("  First reply of the capture: Data0 %02X Data1 %02X (at %.3f s)" % (
            first.raw[4], first.raw[5], first.t_start))


def press_intervals(line, t_end):
    out, down = [], None
    for t, v in zip(line.t, line.v):
        if v == 0 and down is None:
            down = t
        elif v == 1 and down is not None:
            out.append((down, t))
            down = None
    if down is not None:
        out.append((down, t_end))
    return out


def host_gaps(host, limit=0.15):
    """Periods without host frames (for example the ESP32 was reset): (start, end)."""
    return [(a.t_end, b.t_start) for a, b in zip(host, host[1:]) if b.t_start - a.t_end > limit]


def report_keys(times, states, replies, gaps):
    section("4. Physical key presses against what the selector reports")
    t_end = times[-1]
    lines = {k: Line(times, states, ch) for k, ch in KEY_CHANNELS.items()}
    presses = {k: press_intervals(lines[k], t_end) for k in lines}
    # Presses while the host was not polling cannot be seen by any reply: leave them out
    in_gap = 0
    for k in presses:
        kept = [(a, b) for a, b in presses[k] if not any(a < g1 and b > g0 for g0, g1 in gaps)]
        in_gap += len(presses[k]) - len(kept)
        presses[k] = kept
    if gaps:
        print("  Host silent for %s: %d presses in those periods are left out below." % (
            ", ".join("%.3f-%.3f s" % g for g in gaps), in_gap))
    for k in ("-", "+", "OK", "SEL"):
        durs = [b - a for a, b in presses[k]]
        print("  %-3s: %2d presses, duration %s" % (k, len(durs), summarize(durs)))

    samples = [r for r in replies if len(r.raw) == 8 and r.raw[3] in (0x53, 0x3F)]
    reported = {k: [key_levels(r)[k][0] == 0 for r in samples] for k in lines}

    def mismatches(delta):
        n = 0
        for k, line in lines.items():
            for r, rep in zip(samples, reported[k]):
                if (line.at(r.t_start + delta) == 0) != rep:
                    n += 1
        return n

    # When does the selector look at the keys? Find the offset (relative to the start of the
    # reply) at which the physical key state best matches the reported one.
    results = [(d / 1000.0, mismatches(d / 1000.0)) for d in range(-250, 21)]
    best = min(m for _, m in results)
    plateau = [d for d, m in results if m == best]
    print()
    print("  Key sampling instant: the reported state matches the physical contact best at")
    print("  %.0f to %.0f ms relative to the start of the reply (%d mismatches out of %d)." % (
        plateau[0] * 1000, plateau[-1] * 1000, best, len(samples) * len(lines)))
    print("  Mismatches by offset (ms: count): " + ", ".join(
        "%d: %d" % (d * 1000, m) for d, m in results if round(d * 1000) % 25 == 0))
    print("  Close to the best offset (ms: count): " + ", ".join(
        "%d: %d" % (round(d * 1000), m) for d, m in results if round(d * 1000) % 5 == 0 and -45 <= d * 1000 <= 10))
    delta = (plateau[0] + plateau[-1]) / 2.0

    # Which presses contain a sampling instant?
    sample_times = [r.t_start + delta for r in samples]
    buckets = [(0, 0.04), (0.04, 0.06), (0.06, 0.08), (0.08, 0.10), (0.10, 0.15), (0.15, 9.0)]
    stats = {b: [0, 0, 0] for b in buckets}  # presses, containing a sample, seen as pressed
    missed = []
    for k in lines:
        for a, b in presses[k]:
            dur = b - a
            idx = bisect.bisect_left(sample_times, a)
            inside = [i for i in range(idx, len(sample_times)) if sample_times[i] <= b]
            seen = any(reported[k][i] for i in inside)
            bucket = next(bk for bk in buckets if bk[0] <= dur < bk[1])
            stats[bucket][0] += 1
            stats[bucket][1] += 1 if inside else 0
            stats[bucket][2] += 1 if seen else 0
            if not inside:
                nxt = [i for i in range(idx, min(idx + 2, len(samples)))]
                missed.append((k, a, dur, [key_levels(samples[i])[k] for i in nxt]))
    print()
    print("  Presses by duration: how many contain a sampling instant, how many were reported")
    print("  %-14s %8s %14s %10s" % ("duration (ms)", "presses", "has a sample", "reported"))
    total = [0, 0, 0]
    for bk in buckets:
        p, c, s = stats[bk]
        total = [total[0] + p, total[1] + c, total[2] + s]
        label = "%d-%s" % (bk[0] * 1000, "" if bk[1] > 1 else "%d" % (bk[1] * 1000))
        print("  %-14s %8d %14d %10d" % (label, p, c, s))
    print("  %-14s %8d %14d %10d" % ("all", *total))

    print()
    print("  Presses with no sampling instant inside (invisible to the host): %d" % len(missed))
    evidence = 0
    for k, a, dur, nxt in missed:
        pulse = any(t[0] == 1 and t[1] == 1 for t in nxt)
        evidence += 1 if pulse else 0
        print("    %-3s at %.3f s, %5.1f ms; next replies (level, pulse low, pulse high): %s%s" % (
            k, a, dur * 1000, nxt, "   <- pulse low set while released" if pulse else ""))
    print("  Of those, with pulse-bit evidence of the press in the next two replies: %d" % evidence)

    # Does the first 'last edge' flip of a key (pulse high 0 -> 1) ever happen without the
    # level having shown a press? That would be a press recorded only by the pulse bits.
    print()
    print("  Pulse-high flip (1,0,0) -> (1,0,1) with no pressed reply in between (a hidden press):")
    hidden = 0
    for k in ("-", "+", "OK", "SEL"):
        prev = None
        for r in samples:
            cur = key_levels(r)[k]
            if prev == (1, 0, 0) and cur == (1, 0, 1):
                hidden += 1
                print("    %-3s at %.3f s" % (k, r.t_start))
            prev = cur
    if not hidden:
        print("    none")

    # Debounce: how long must the physical state last before the selector reports it?
    print()
    print("  Reported pressed, by the physical state at the sampling instant and its age")
    print("  (time since the last edge of the contact). Shows the selector's own debounce.")
    cuts = [0, 5, 10, 15, 20, 25, 30, 40, 60, 100]
    for state, name in ((0, "physically pressed"), (1, "physically released")):
        rows = []
        for k, line in lines.items():
            for r, rep in zip(samples, reported[k]):
                t = r.t_start + delta
                i = bisect.bisect_right(line.t, t) - 1
                if i < 1 or line.v[i] != state:
                    continue
                age = (t - line.t[i]) * 1000
                rows.append((age, rep))
        out = []
        for lo, hi in zip(cuts, cuts[1:] + [1e9]):
            sel = [rep for age, rep in rows if lo <= age < hi]
            if sel:
                out.append("%d-%s ms: %d/%d" % (lo, "" if hi > 1e8 else "%d" % hi, sum(sel), len(sel)))
        print("    %s: %s" % (name, ", ".join(out)))
    print("    (each entry: samples reported as pressed / samples in that age range)")


def write_frames(path, host, replies):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["line", "t_start_s", "t_end_s", "function", "hex"])
        rows = [("host", fr) for fr in host] + [("reply", fr) for fr in replies]
        for line, fr in sorted(rows, key=lambda x: x[1].t_start):
            w.writerow([line, "%.6f" % fr.t_start, "%.6f" % fr.t_end, func_name(fr),
                        " ".join("%02X" % b for b in fr.raw)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv_file")
    ap.add_argument("--frames", help="also write every decoded frame to this CSV file")
    args = ap.parse_args()

    times, states = load(args.csv_file)
    print("%s: %d rows, %.3f s" % (args.csv_file, len(times), times[-1]))

    reply_line = Line(times, states, CH_REPLY)
    host_line = Line(times, states, CH_HOST)
    host, host_drops = assemble(decode_uart(host_line))
    replies, reply_drops = assemble(decode_uart(reply_line))

    report_frames(host, replies, host_drops, reply_drops, times[-1])
    report_timing(host, replies)
    report_key_encoding(replies)
    report_keys(times, states, replies, host_gaps(host))

    if args.frames:
        write_frames(args.frames, host, replies)
        print("\nFrames written to %s" % args.frames)


if __name__ == "__main__":
    sys.exit(main())

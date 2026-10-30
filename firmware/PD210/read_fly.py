#!/usr/bin/env python3
"""Read the PD210 v332 / PD245 v019 tuning black box (60 ms/sample).

Modes:
  --live    read the ring buffers from SRAM2 (target must stay powered)
  --flash   read the pages saved when heating stops (v012 auto-save)
  --bin F   parse a full 256 KB flash dump

Examples:
  python read_fly.py --live  --csv fly.csv
  python read_fly.py --flash --csv fly.csv
  python read_fly.py --bin PD245_full.bin --csv fly.csv

Use the pyOCD venv on this machine:
  D:\\tmp\\pyocd_env\\Scripts\\python.exe read_fly.py --flash --csv fly.csv
"""
import argparse
import csv
import struct
import sys

FLY_T = 0x10001000
FLY_D = 0x10002000
FLY_R = 0x10003000
FLY_I = 0x2000092E       # fly_i, PD210 v332
FLY_N = 2048
SAMPLE_S = 0.060
FLY_MAGIC = 0x46314C59

FLASH_BASE = 0x0803C800
FLASH_SIZE = 0x3000
PAGE_T = 0x0000
PAGE_D = 0x1000
PAGE_R = 0x2000


def u16s(buf, off, n):
    return list(struct.unpack_from("<%dH" % n, buf, off))


def read_block(target, addr, n, chunk=16):
    out = bytearray()
    for off in range(0, n, chunk):
        out += bytes(target.read_memory_block8(addr + off, min(chunk, n - off)))
    return bytes(out)


def parse_arrays(t, d, r, cnt, rotate):
    """rotate=True for the SRAM ring, False for flash (already reordered by
    flash_fly_save; the T page only holds 2044 samples, so rotating t again
    would run past its entries)."""
    if cnt <= 0:
        return []
    n = min(len(t), len(d), len(r), 2044)
    if not rotate:
        return [(i, t[i], d[i], r[i]) for i in range(n)]
    if cnt < FLY_N:
        take = min(cnt, n)
        return [(i, t[i], d[i], r[i]) for i in range(take)]
    start = cnt & (FLY_N - 1)
    rows = []
    for i in range(n):
        j = (start + i) & (FLY_N - 1)
        rows.append((i, t[j], d[j], r[j]))
    return rows


def read_live():
    from pyocd.core.helpers import ConnectHelper
    opts = {"resume_on_disconnect": True, "frequency": 500000}
    s = ConnectHelper.session_with_chosen_probe(target_override="stm32l431cctx", options=opts)
    s.open()
    try:
        cnt = s.target.read16(FLY_I)
        t = u16s(read_block(s.target, FLY_T, FLY_N * 2), 0, FLY_N)
        d = u16s(read_block(s.target, FLY_D, FLY_N * 2), 0, FLY_N)
        r = u16s(read_block(s.target, FLY_R, FLY_N * 2), 0, FLY_N)
        s.target.resume()
        return parse_arrays(t, d, r, cnt, True), cnt
    finally:
        s.close()


def read_flash_dev():
    from pyocd.core.helpers import ConnectHelper
    opts = {"resume_on_disconnect": True, "frequency": 500000}
    s = ConnectHelper.session_with_chosen_probe(target_override="stm32l431cctx", options=opts)
    s.open()
    try:
        s.target.halt()
        buf = read_block(s.target, FLASH_BASE, FLASH_SIZE)
        s.target.resume()
    finally:
        s.close()
    return buf


def parse_flash_buf(buf):
    magic, cnt = struct.unpack_from("<II", buf, PAGE_T)
    if magic != FLY_MAGIC:
        raise SystemExit("black box magic not found (0x%08X)" % magic)
    t_count = min(cnt, 2044)
    d_count = min(cnt, 2048)
    t = u16s(buf, PAGE_T + 8, t_count)
    d = u16s(buf, PAGE_D, d_count)
    r = u16s(buf, PAGE_R, d_count)
    return parse_arrays(t, d, r, cnt, False), cnt


def write_csv(rows, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["index", "time_s", "raw", "temp_c", "duty", "duty_pct"])
        for i, t, d, r in rows:
            w.writerow([i, round(i * SAMPLE_S, 3), r, t, d, round(d * 100.0 / 319.0, 1)])


def summary(rows, cnt):
    if not rows:
        print("no samples")
        return
    temps = [x[1] for x in rows]
    duties = [x[2] for x in rows]
    print("samples=%d  count=%d  span=%.1fs" % (len(rows), cnt, len(rows) * SAMPLE_S))
    print("temp  min=%d max=%d last=%d" % (min(temps), max(temps), temps[-1]))
    print("duty  max=%d (%.1f%%) last=%d" % (max(duties), max(duties) * 100.0 / 319.0, duties[-1]))
    peak = max(range(len(temps)), key=lambda i: temps[i])
    print("peak  index=%d time=%.1fs temp=%d duty=%d" % (peak, peak * SAMPLE_S, temps[peak], duties[peak]))


def plot(rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print("plot skipped:", e)
        return
    xs = [x[0] * SAMPLE_S for x in rows]
    fig, ax1 = plt.subplots()
    ax1.plot(xs, [x[1] for x in rows], "r-", label="temp C")
    ax1.set_xlabel("time (s)")
    ax1.set_ylabel("temperature (C)", color="r")
    ax2 = ax1.twinx()
    ax2.plot(xs, [x[2] * 100.0 / 319.0 for x in rows], "b-", label="duty %")
    ax2.set_ylabel("duty (%)", color="b")
    fig.tight_layout()
    fig.savefig("fly.png", dpi=140)
    print("plot: fly.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--flash", action="store_true")
    ap.add_argument("--bin")
    ap.add_argument("--csv", default="fly.csv")
    ap.add_argument("--plot", action="store_true")
    a = ap.parse_args()
    try:
        if a.bin:
            buf = open(a.bin, "rb").read()
            if len(buf) < 0x3F800:
                raise SystemExit("bin too small; need full flash dump")
            rows, cnt = parse_flash_buf(buf[0x3C800:0x3F800])
        elif a.flash:
            rows, cnt = parse_flash_buf(read_flash_dev())
        elif a.live:
            rows, cnt = read_live()
        else:
            raise SystemExit("use --live, --flash or --bin")
    except SystemExit:
        raise
    except Exception as e:
        print("read failed:", e)
        print("close PowerWriter / PowerWriter's probe lock, then retry; or use --bin with a full flash dump.")
        return 1
    write_csv(rows, a.csv)
    print("csv:", a.csv)
    summary(rows, cnt)
    if a.plot:
        plot(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())

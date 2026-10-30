#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PD210 / PD245 black-box reader (read-only).

Reads the tuning log from flash or SRAM2 through a CMSIS-DAP probe (PW Link),
then writes CSV / JSON / PNG / HTML.  It never flashes, erases or resets.
"""

from __future__ import annotations

import argparse
import base64
import csv
import datetime as _dt
import hashlib
import json
import math
import os
import re
import struct
import sys
import threading
import zipfile
import zlib
from dataclasses import dataclass, field
from statistics import mean, pstdev

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    from pyocd.core.helpers import ConnectHelper
except Exception:  # offline --csv / --bin still work without pyOCD
    ConnectHelper = None

try:
    from PIL import Image, ImageDraw, ImageFont
except Exception:
    Image = None


# ---------------------------------------------------------------------------
# Hardware layout
# ---------------------------------------------------------------------------
FLY_MAGIC = 0x46314C59          # "FLY1"
NEW_BASE = 0x0803C800
OLD_BASE = 0x0803E000
NEW_SIZE = 0x3000
OLD_SIZE = 0x1800
META_BASE = 0x0803C000
META_SIZE = 56
META_MAGIC = 0x4D455441          # "META"
META_VERSION = 1
NEW_T_OFF, NEW_D_OFF, NEW_R_OFF = 8, 0x1000, 0x2000
OLD_T_OFF, OLD_D_OFF, OLD_R_OFF = 8, 0x800, 0x1000
NEW_T_MAX, NEW_D_MAX, NEW_R_MAX = 2044, 2048, 2048
OLD_T_MAX, OLD_D_MAX, OLD_R_MAX = 1020, 1024, 1024
NEW_SAMPLE_S = 0.060
OLD_SAMPLE_S = 0.100
PWM_MAX = 319
SETTINGS_ADDR = 0x0803F800
SETTINGS_MAGICS = (0x50443234, 0x50443235)   # PD24 / PD25

PRODUCTS = {
    "pd210": {
        "name": "PD210",
        "fly_i": 0x2000092E,
        "fly_enable": 0x20000925,
        "fly_dur": 0x20000924,
    },
    "pd245": {
        "name": "PD245 V3",
        "fly_i": 0x2000092A,
        "fly_enable": 0x20000922,
        "fly_dur": 0x20000921,
    },
}

KNOWN_VERSIONS = [
    b"V335", b"V334", b"V333", b"V332", b"V331", b"V329",
    b"V023", b"V022", b"V021", b"V019", b"V018", b"V014", b"V013",
]


class ToolError(Exception):
    pass


@dataclass
class Log:
    source: str
    layout: str
    product: str | None
    version: str | None
    cnt: int
    sample_s: float
    t: list[int]
    d: list[int]
    r: list[int]
    target_c: int | None = None
    limit_pct: int | None = None
    pdo_gear: int | None = None
    running: bool | None = None
    fly_enable: int | None = None
    fly_dur: int | None = None
    meta: dict | None = None
    notes: list[str] = field(default_factory=list)

    @property
    def product_name(self) -> str:
        if self.product in PRODUCTS:
            return PRODUCTS[self.product]["name"]
        return "PD210/PD245"

    @property
    def mode_name(self) -> str:
        if self.layout == "old":
            return "旧三页"
        if self.fly_dur == 0 and self.source == "live":
            return "61S"
        if self.fly_dur == 1 and self.source == "live":
            return "123S"
        if self.cnt == 1024:
            return "61S"
        if self.cnt == 2048:
            return "123S"
        if self.cnt <= 0:
            return "空"
        if self.layout == "csv":
            return f"CSV {self.cnt}点"
        return f"{self.cnt}点"


def _u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def _u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def _read(target, addr: int, n: int) -> bytes:
    return bytes(target.read_memory_block8(addr, n))


def open_session():
    if ConnectHelper is None:
        raise ToolError("当前 Python 没有 pyocd；请用 run_gui.bat 或 D:\\tmp\\pyocd_env 运行。")
    errors = []
    for freq in (500_000, 250_000, 100_000):
        try:
            session = ConnectHelper.session_with_chosen_probe(
                target_override="stm32l431cctx",
                frequency=freq,
                options={"connect_mode": "attach"},
                blocking=False,
            )
            if session is None:
                errors.append("未找到 PW Link/CMSIS-DAP")
                continue
            session.open()
            return session
        except Exception as exc:  # noqa: BLE001
            errors.append(f"{freq//1000}kHz: {type(exc).__name__}")
    raise ToolError(
        "连接失败（" + "; ".join(errors) + "）。\n"
        "请先完全退出 PowerWriter，再给设备断电重上电/按复位；"
        "本工具不会复位或烧录。"
    )


def detect_version(target) -> str | None:
    buf = bytearray()
    try:
        for addr in range(0x08000000, 0x08018000, 0x1000):
            buf += _read(target, addr, 0x1000)
    except Exception:
        return None
    hits: list[tuple[int, str]] = []
    for raw in KNOWN_VERSIONS:
        pos = bytes(buf).find(raw)
        if pos >= 0:
            hits.append((pos, raw.decode("ascii")))
    if not hits:
        for m in re.finditer(rb"V(\d{3})", bytes(buf)):
            hits.append((m.start(), "V" + m.group(1).decode("ascii")))
    if not hits:
        return None
    hits.sort()
    return hits[-1][1]


def product_from_version(version: str | None) -> str | None:
    if not version or len(version) != 4:
        return None
    if version.startswith("V3"):
        return "pd210"
    if version.startswith("V0"):
        return "pd245"
    return None


def is_running(target) -> bool | None:
    try:
        rcc = target.read32(0x40021000)
        systick = target.read32(0xE000E010)
        return not (rcc == 0x63 and (systick & 1) == 0)
    except Exception:
        return None


def read_settings(target) -> dict | None:
    try:
        buf = _read(target, SETTINGS_ADDR, 80)
    except Exception:
        return None
    magic = _u32(buf, 0)
    if magic not in SETTINGS_MAGICS:
        return None
    target_c = struct.unpack_from("<i", buf, 4)[0]
    limit_pct = struct.unpack_from("<i", buf, 8)[0]
    gear = struct.unpack_from("<i", buf, 12)[0]
    return {"target_c": target_c, "limit_pct": limit_pct, "pdo_gear": gear}


def _meta_checksum(raw: bytes) -> int:
    s = 0xA5A5A5A5
    for w in struct.unpack_from("<13I", raw, 0):
        s = ((s << 1) | (s >> 31)) & 0xFFFFFFFF
        s ^= w
    return s


def parse_meta(raw: bytes, header_cnt: int | None = None) -> dict | None:
    if len(raw) < META_SIZE:
        return None
    magic, ver, cnt = struct.unpack_from("<IHH", raw, 0)
    if magic != META_MAGIC or ver != META_VERSION:
        return None
    if header_cnt is not None and cnt != header_cnt:
        return None
    if struct.unpack_from("<I", raw, 52)[0] != _meta_checksum(raw):
        return None
    crc64 = struct.unpack_from("<Q", raw, 8)[0]
    run_seq = struct.unpack_from("<I", raw, 16)[0]
    fw_tag = raw[20:24].decode("ascii", "replace").strip("\x00 ")
    uid = raw[24:36].hex().upper()
    target_c = struct.unpack_from("<h", raw, 36)[0]
    limit_pct = raw[38]
    pdo_gear = raw[39]
    pid_p, pid_i, pid_d = struct.unpack_from("<HHH", raw, 40)
    avg_v, avg_i, avg_p = struct.unpack_from("<HHH", raw, 46)
    return {
        "magic": magic, "version": ver, "cnt": cnt, "crc64": crc64,
        "run_seq": run_seq, "fw_tag": fw_tag, "uid": uid,
        "target_c": target_c, "limit_pct": limit_pct, "pdo_gear": pdo_gear,
        "pid_p100": pid_p, "pid_i1000": pid_i, "pid_d100": pid_d,
        "avg_vin_dv": avg_v, "avg_iout_ma": avg_i, "avg_power_w10": avg_p,
    }


def read_meta(target) -> dict | None:
    try:
        hdr = _read(target, NEW_BASE, 8)
        if _u32(hdr, 0) != FLY_MAGIC:
            return None
        cnt = _u32(hdr, 4) & 0xFFFF
        return parse_meta(_read(target, META_BASE, META_SIZE), cnt)
    except Exception:
        return None


def apply_meta(log: Log, meta: dict | None) -> None:
    if not meta:
        return
    log.meta = meta
    tag = meta.get("fw_tag") or ""
    if tag:
        log.version = tag
        p = product_from_version(tag)
        if p:
            log.product = p
    log.target_c = meta.get("target_c")
    log.limit_pct = meta.get("limit_pct")
    log.pdo_gear = meta.get("pdo_gear")


def _parse_arrays(buf: bytes, layout: str, cnt: int):
    if layout == "new":
        tmax, dmax, rmax = NEW_T_MAX, NEW_D_MAX, NEW_R_MAX
        toff, doff, roff, sample = NEW_T_OFF, NEW_D_OFF, NEW_R_OFF, NEW_SAMPLE_S
    else:
        tmax, dmax, rmax = OLD_T_MAX, OLD_D_MAX, OLD_R_MAX
        toff, doff, roff, sample = OLD_T_OFF, OLD_D_OFF, OLD_R_OFF, OLD_SAMPLE_S
    tn, dn, rn = min(cnt, tmax), min(cnt, dmax), min(cnt, rmax)
    t = list(struct.unpack_from(f"<{tn}H", buf, toff)) if tn else []
    d = list(struct.unpack_from(f"<{dn}H", buf, doff)) if dn else []
    r = list(struct.unpack_from(f"<{rn}H", buf, roff)) if rn else []
    return t, d, r, sample


def read_flash_log(target) -> Log:
    newbuf = _read(target, NEW_BASE, NEW_SIZE)
    if _u32(newbuf, 0) == FLY_MAGIC:
        cnt = _u32(newbuf, 4)
        t, d, r, sample = _parse_arrays(newbuf, "new", cnt)
        return Log("flash", "new", None, None, cnt, sample, t, d, r)
    oldbuf = _read(target, OLD_BASE, OLD_SIZE)
    if _u32(oldbuf, 0) == FLY_MAGIC:
        cnt = _u32(oldbuf, 4)
        t, d, r, sample = _parse_arrays(oldbuf, "old", cnt)
        return Log("flash", "old", None, None, cnt, sample, t, d, r)
    raise ToolError("未找到黑匣子日志（FLY1）。可能还没有保存过，或已被清空。")


def _coherence(t: list[int], d: list[int], r: list[int]) -> int:
    n = min(64, len(t), len(d), len(r))
    return sum(1 for i in range(n)
               if t[i] <= 999 and d[i] <= PWM_MAX and r[i] <= 4095)


def read_live_log(target, product: str | None, version: str | None) -> Log:
    if product not in PRODUCTS:
        product = product_from_version(version)
    if product not in PRODUCTS:
        candidates = []
        for key, info in PRODUCTS.items():
            try:
                n = target.read16(info["fly_i"])
            except Exception:
                continue
            if 0 <= n <= 2048:
                candidates.append((key, n))
        if candidates:
            product = max(candidates, key=lambda x: x[1])[0]
        else:
            product = "pd210"
    info = PRODUCTS[product]
    cnt = target.read16(info["fly_i"])
    if cnt > 2048:
        cnt = 2048
    try:
        fly_enable = target.read8(info["fly_enable"])
        fly_dur = target.read8(info["fly_dur"])
    except Exception:
        fly_enable = fly_dur = None
    running = is_running(target)
    notes: list[str] = []
    if running is False:
        notes.append("设备未运行（可能被 PowerWriter 停住），实时数据可能无效。")
    if cnt == 0:
        notes.append("实时缓冲为空：当前没有加热记录，或这轮已经落盘。")
        if fly_enable is not None:
            notes.append(f"黑匣子当前：{'开' if fly_enable else '关'}；fly_i=0。")
    tmax = min(cnt, NEW_T_MAX)
    t = list(struct.unpack(f"<{tmax}H", _read(target, 0x10001000, 2 * tmax))) if tmax else []
    dmax = min(cnt, NEW_D_MAX)
    d = list(struct.unpack(f"<{dmax}H", _read(target, 0x10002000, 2 * dmax))) if dmax else []
    rmax = min(cnt, NEW_R_MAX)
    r = list(struct.unpack(f"<{rmax}H", _read(target, 0x10003000, 2 * rmax))) if rmax else []
    if cnt and _coherence(t, d, r) < min(8, len(t)):
        notes.append("实时缓冲内容不完整或未初始化。")
    return Log("live", "new", product, version, cnt, NEW_SAMPLE_S, t, d, r,
               running=running, fly_enable=fly_enable, fly_dur=fly_dur, notes=notes)


def read_probe(source: str, product_arg: str = "auto") -> Log:
    session = open_session()
    target = session.target
    try:
        target.halt()
        version = detect_version(target)
        product = product_from_version(version) or (product_arg if product_arg != "auto" else None)
        running = is_running(target)
        settings = read_settings(target)
        fly_en = None
        fly_i = None
        if product in PRODUCTS:
            try:
                fly_en = target.read8(PRODUCTS[product]["fly_enable"])
                fly_i = target.read16(PRODUCTS[product]["fly_i"])
            except Exception:
                pass
        has_meta = False
        if source == "live":
            log = read_live_log(target, product, version)
        else:
            try:
                log = read_flash_log(target)
            except ToolError as exc:
                pname = PRODUCTS[product]["name"] if product in PRODUCTS else "未知"
                stxt = (f"target {settings['target_c']}°C / LM {settings['limit_pct']}% / gear {settings['pdo_gear']}"
                        if settings else "设置读取失败")
                ftxt = ("开" if fly_en else "关") if fly_en is not None else "未知"
                raise ToolError(
                    f"{exc}\n设备: {pname} {version or ''}；{stxt}\n"
                    f"黑匣子: {ftxt}；fly_i={fly_i if fly_i is not None else '?'}\n"
                    "如果黑匣子为空：先在 SET 第 10 项选 61S/123S，加热一轮并保存后再读。"
                ) from exc
            meta = read_meta(target)
            if meta:
                apply_meta(log, meta)
                has_meta = True
                log.notes.append(
                    f"元数据: FW {meta.get('fw_tag')} · RUN #{meta.get('run_seq')} · "
                    f"UID {meta.get('uid', '')[:8]}…"
                )
            else:
                log.notes.append("FLY1 日志本身不含固件版本/时间戳；标题版本是读取时设备上的固件。")
                log.notes.append("目标温度/LM 是读取时设置页的值；FLY1 不保存当时的设置。")
        log.running = running
        if not has_meta:
            log.version = version
            log.product = product
            if settings:
                log.target_c = settings["target_c"]
                log.limit_pct = settings["limit_pct"]
                log.pdo_gear = settings["pdo_gear"]
        try:
            if target.read32(0x40022010) & 0x10000:
                log.notes.append("读取时 Flash 正忙（BSY），本次日志可能不完整，请等保存完成后再读。")
        except Exception:
            pass
        return log
    finally:
        try:
            target.resume()
        finally:
            session.close()


def read_bin_log(path: str) -> Log:
    data = open(path, "rb").read()
    if len(data) < 0x3F800:
        raise ToolError("bin 文件太小；需要整片 256KB flash dump。")
    newbuf = data[NEW_BASE - 0x08000000: NEW_BASE - 0x08000000 + NEW_SIZE]
    if len(newbuf) >= NEW_SIZE and _u32(newbuf, 0) == FLY_MAGIC:
        cnt = _u32(newbuf, 4)
        t, d, r, sample = _parse_arrays(newbuf, "new", cnt)
        log = Log("bin", "new", None, None, cnt, sample, t, d, r)
        off = META_BASE - 0x08000000
        meta = parse_meta(data[off:off + META_SIZE], cnt & 0xFFFF)
        apply_meta(log, meta)
        return log
    oldbuf = data[OLD_BASE - 0x08000000: OLD_BASE - 0x08000000 + OLD_SIZE]
    if len(oldbuf) >= OLD_SIZE and _u32(oldbuf, 0) == FLY_MAGIC:
        cnt = _u32(oldbuf, 4)
        t, d, r, sample = _parse_arrays(oldbuf, "old", cnt)
        return Log("bin", "old", None, None, cnt, sample, t, d, r)
    raise ToolError("bin 里没有找到 FLY1 日志。")


def read_csv_log(path: str, target: int | None = None, lm: int | None = None) -> Log:
    with open(path, "r", encoding="utf-8-sig", newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        raise ToolError("CSV 为空。")
    t: list[int] = []
    d: list[int] = []
    r: list[int] = []
    times: list[float] = []
    for row in rows:
        try:
            times.append(float(row.get("time_s") or 0))
        except ValueError:
            pass
        for key, dst in (("temp_c", t), ("duty", d), ("raw", r)):
            val = (row.get(key) or "").strip()
            if val:
                dst.append(int(float(val)))
    sample = NEW_SAMPLE_S
    if len(times) >= 2:
        diffs = [b - a for a, b in zip(times, times[1:]) if b > a]
        if diffs:
            sample = sorted(diffs)[len(diffs) // 2]
    side = os.path.splitext(path)[0] + ".json"
    meta = {}
    if os.path.exists(side):
        try:
            meta = json.load(open(side, "r", encoding="utf-8"))
        except Exception:
            meta = {}
    product = meta.get("product")
    version = meta.get("version")
    if target is None:
        target = meta.get("target_c")
    if lm is None:
        lm = meta.get("limit_pct")
    cnt = int(meta.get("cnt") or max(len(t), len(d), len(r)))
    log = Log("csv", "csv", product, version, cnt, sample, t, d, r,
              target_c=target, limit_pct=lm, pdo_gear=meta.get("pdo_gear"),
              notes=["CSV 重建：固件版本/设置来自同名 JSON（若有）。"])
    fly_meta = meta.get("fly_meta")
    if isinstance(fly_meta, dict):
        apply_meta(log, fly_meta)
        log.notes.append(f"CSV 元数据: RUN #{fly_meta.get('run_seq')} · FW {fly_meta.get('fw_tag')}")
    return log


# ---------------------------------------------------------------------------
# Statistics / identity
# ---------------------------------------------------------------------------
def canonical_bytes(log: Log) -> bytes:
    buf = bytearray(struct.pack("<I", log.cnt))
    for arr in (log.t, log.d, log.r):
        if arr:
            buf += struct.pack(f"<{len(arr)}H", *arr)
    return bytes(buf)


CRC64_POLY = 0x42F0E1EBA9EA3693
CRC64_MASK = (1 << 64) - 1


def crc64_ecma(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b << 56
        for _ in range(8):
            c = ((c << 1) ^ CRC64_POLY) & CRC64_MASK if c & (1 << 63) else (c << 1) & CRC64_MASK
    return c


def _short12(v: int) -> str:
    v &= 0xFFFFFFFFFFFF
    return f"{(v >> 32) & 0xFFFF:04X}-{(v >> 16) & 0xFFFF:04X}-{v & 0xFFFF:04X}"


def _short8(crc: int) -> str:
    return f"{(crc >> 16) & 0xFFFF:04X}-{crc & 0xFFFF:04X}"


def identity(log: Log) -> dict:
    data = canonical_bytes(log)
    crc32 = zlib.crc32(data) & 0xFFFFFFFF
    crc64 = crc64_ecma(data)
    meta = log.meta
    use64 = None
    kind = "legacy-crc32"
    meta_ok = False
    if meta and meta.get("crc64") is not None:
        use64 = int(meta["crc64"]) & CRC64_MASK
        kind = "meta"
        meta_ok = (use64 == crc64)
        if not meta_ok:
            log.notes.append("元数据 CRC64 与重算不一致（日志/元数据可能不完整）；报告以重算值为准。")
            use64 = crc64
            kind = "meta-mismatch"
    elif log.source == "live":
        use64 = crc64
        kind = "live"
    short = _short12(use64) if use64 is not None else _short8(crc32)
    return {
        "md5": hashlib.md5(data).hexdigest(),
        "crc32": f"{crc32:08X}",
        "crc64": f"{crc64:016X}",
        "short_id": short,
        "kind": kind,
        "meta_ok": meta_ok,
        "algorithm": ("CRC64/ECMA-182 of LE32(cnt)+T+D+R; display = lower 48 bits"
                      if use64 is not None else
                      "CRC32/MD5 of LE32(cnt)+T+D+R (legacy FLY1)"),
    }


def analyze(log: Log) -> dict:
    n = len(log.t)
    if n == 0:
        return {"samples": 0, "duration_s": 0.0}
    duration = n * log.sample_s
    last_n = max(1, min(n, int(round(20.0 / log.sample_s))))
    lt = log.t[-last_n:]
    ld = log.d[-last_n:]
    duty_pct = [x * 100.0 / PWM_MAX for x in log.d]
    last_duty = [x * 100.0 / PWM_MAX for x in ld]
    tgt = log.target_c
    t_to_target = None
    if tgt is not None:
        for i, v in enumerate(log.t):
            if v >= tgt:
                t_to_target = i * log.sample_s
                break
    peak = max(log.t)
    peak_i = log.t.index(peak)
    return {
        "samples": n,
        "duration_s": duration,
        "start_c": log.t[0],
        "end_c": log.t[-1],
        "min_c": min(log.t),
        "max_c": peak,
        "peak_time_s": peak_i * log.sample_s,
        "mean_c": mean(log.t),
        "last20_mean_c": mean(lt),
        "last20_std_c": pstdev(lt) if len(lt) > 1 else 0.0,
        "last20_min_c": min(lt),
        "last20_max_c": max(lt),
        "time_to_target_s": t_to_target,
        "duty_max_pct": max(duty_pct) if duty_pct else 0.0,
        "duty_mean_pct": mean(duty_pct) if duty_pct else 0.0,
        "last20_duty_mean_pct": mean(last_duty) if last_duty else 0.0,
        "raw_min": min(log.r) if log.r else None,
        "raw_max": max(log.r) if log.r else None,
    }


def integrity(log: Log) -> dict:
    bad_raw = sum(1 for x in log.r if x > 4095)
    bad_duty = sum(1 for x in log.d if x > PWM_MAX)
    bad_temp = sum(1 for x in log.t if x > 999)
    tail_d = log.d[-4:] if log.d else []
    tail_r = log.r[-4:] if log.r else []
    return {
        "bad_raw": bad_raw,
        "bad_duty": bad_duty,
        "bad_temp": bad_temp,
        "tail_d": tail_d,
        "tail_r": tail_r,
        "ok": bad_raw == 0 and bad_duty == 0 and bad_temp == 0,
    }


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def _font(size: int):
    for path in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\msyhbd.ttc",
                 r"C:\Windows\Fonts\arial.ttf"):
        try:
            return ImageFont.truetype(path, size, index=0)
        except Exception:
            continue
    return ImageFont.load_default()


def render_png(log: Log, st: dict, ident: dict, path: str) -> None:
    if Image is None:
        raise ToolError("缺少 Pillow，无法生成 PNG。请用 run_gui.bat 的 Python 环境。")
    W, H = 1200, 520
    L, R, TOP, BOT = 90, 110, 112, 82
    img = Image.new("RGB", (W, H), (14, 14, 18))
    dr = ImageDraw.Draw(img)
    f14, f16, f22 = _font(14), _font(16), _font(22)

    n = len(log.t)
    dur = max(st.get("duration_s", 0.0), 0.1)
    tmin = min(0, math.floor(min(log.t) / 50.0) * 50)
    tmax = max(350, math.ceil((max(log.t) + 10) / 50.0) * 50)
    if log.target_c:
        tmax = max(tmax, math.ceil((log.target_c + 20) / 50.0) * 50)
    dmax_pct = max(30.0, math.ceil((st.get("duty_max_pct", 0) + 5) / 10.0) * 10.0)
    if log.limit_pct:
        dmax_pct = max(dmax_pct, min(100.0, log.limit_pct + 5))
    dmax_pct = min(100.0, dmax_pct)

    def X(i: int) -> float:
        return L + (W - L - R) * (i * log.sample_s) / dur

    def YT(v: float) -> float:
        return H - BOT - (H - TOP - BOT) * (v - tmin) / max(1, tmax - tmin)

    def YD(v: float) -> float:
        return H - BOT - (H - TOP - BOT) * v / dmax_pct

    for c in range(int(tmin), int(tmax) + 1, 50):
        y = YT(c)
        dr.line([(L, y), (W - R, y)], fill=(38, 38, 46))
        dr.text((18, y - 9), str(c), font=f14, fill=(150, 150, 160))
    for pct in range(0, int(dmax_pct) + 1, 10):
        y = YD(pct)
        dr.line([(W - R, y), (W - R + 5, y)], fill=(60, 60, 70))
        dr.text((W - R + 10, y - 9), f"{pct}%", font=f14, fill=(80, 190, 210))
    step = 10 if dur <= 90 else 20
    for tsec in range(0, int(dur) + 1, step):
        x = L + (W - L - R) * tsec / dur
        dr.line([(x, TOP), (x, H - BOT)], fill=(32, 32, 40))
        dr.text((x - 10, H - BOT + 10), f"{tsec}s", font=f14, fill=(150, 150, 160))

    product = log.product_name
    meta = log.meta or {}
    is_meta = bool(log.meta)
    ver = f" {log.version}" if log.version and (is_meta or log.source not in ("flash", "bin")) else ""
    dr.text((L, 16), f"{product}{ver} · 黑匣子 {log.mode_name}", font=f22, fill=(232, 232, 238))
    if is_meta:
        src = ("Flash 落盘（META）" if log.source == "flash" else
               "bin 文件（META）" if log.source == "bin" else
               "SRAM2 实时" if log.source == "live" else "CSV")
        fw_note = f" · 日志固件 {log.version} · RUN #{meta.get('run_seq')}"
    else:
        src = {"flash": "Flash 落盘（旧 FLY1）", "live": "SRAM2 实时",
               "bin": "bin 文件（旧 FLY1）", "csv": "CSV"}.get(log.source, log.source)
        fw_note = f" · 读取固件 {log.version}" if log.source in ("flash", "bin") and log.version else ""
    settings_label = "运行设置" if is_meta else "当前设置"
    dr.text((L, 50),
            f"来源 {src}{fw_note} · 点数 {n} · 时长 {dur:.1f}s · 目标 {log.target_c if log.target_c is not None else '?'}°C · "
            f"LM {log.limit_pct if log.limit_pct is not None else '?'}%（{settings_label}）",
            font=f16, fill=(170, 170, 180))

    if log.target_c:
        y = YT(log.target_c)
        dr.line([(L, y), (W - R, y)], fill=(140, 115, 45))
        dr.text((L + 8, y - 20), f"目标 {log.target_c}°C", font=f14, fill=(210, 180, 90))

    if n:
        tp = [(X(i), YT(log.t[i])) for i in range(n)]
        dp = [(X(i), YD(log.d[i] * 100.0 / PWM_MAX)) for i in range(n)]
        dr.line(dp, fill=(35, 175, 200), width=2)
        dr.line(tp, fill=(255, 70, 60), width=2)
        px, py = X(int(st["peak_time_s"] / log.sample_s)), YT(st["max_c"])
        dr.ellipse([px - 3, py - 3, px + 3, py + 3], fill=(255, 200, 90))
        dr.text((min(px + 8, W - 220), max(py - 26, TOP + 4)),
                f"峰值 {st['max_c']}°C @{st['peak_time_s']:.1f}s", font=f16, fill=(255, 190, 90))
        if st.get("last20_mean_c") is not None:
            dr.text((L, 78),
                    f"末20s {st['last20_mean_c']:.1f}±{st['last20_std_c']:.1f}°C  "
                    f"duty {st['last20_duty_mean_pct']:.1f}%",
                    font=f16, fill=(220, 220, 228))

    dr.text((L, H - 28), "数据ID " + ident["short_id"] + " · MD5 " + ident["md5"][:16] + "…",
            font=f14, fill=(150, 210, 220))
    dr.text((W - R - 290, H - 28), "功率未记录（仅 duty 代理）", font=f14, fill=(150, 150, 160))
    dr.text((W - R - 215, 80), "温度 °C", font=f14, fill=(255, 90, 80))
    dr.text((W - R - 105, 80), "占空比 %", font=f14, fill=(60, 220, 255))
    img.save(path)


HTML_HEAD = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PD 黑匣子报告</title>
<style>
:root{color-scheme:dark}
body{margin:0;background:#0e0e12;color:#e6e6ec;font:15px/1.55 "Microsoft YaHei",system-ui,sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:28px 22px 40px}
h1{font-size:24px;margin:0 0 4px}
.sub{color:#9aa0ad;margin-bottom:20px}
.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:18px 0}
.card{background:#17171d;border:1px solid #272730;border-radius:10px;padding:14px 16px}
.card .k{color:#8f96a3;font-size:13px}.card .v{font-size:22px;font-weight:600;margin-top:4px}
.chart{background:#14141a;border:1px solid #272730;border-radius:12px;padding:10px;margin:14px 0}
.chartbox{position:relative}
.chart img{width:100%;height:auto;display:block;border-radius:8px;cursor:crosshair}
#bbCursor{position:absolute;top:17.3%;bottom:15.8%;width:2px;background:#2ed0f0;box-shadow:0 0 6px #2ed0f0;pointer-events:none;display:none}
.ctl{display:flex;gap:14px;align-items:center;margin-top:10px}
#bbSlider{flex:1;accent-color:#2ed0f0}
.readout{font-family:Consolas,monospace;color:#9fe3ee;white-space:nowrap;min-width:360px}
.hint{color:#8f96a3;font-size:12px;margin-top:6px}
@media(max-width:760px){.ctl{display:block}.readout{margin-top:6px;min-width:0}}
table{width:100%;border-collapse:collapse;margin:10px 0 18px;background:#14141a;border:1px solid #272730;border-radius:10px;overflow:hidden}
th,td{padding:8px 12px;border-bottom:1px solid #23232c;text-align:left;vertical-align:top}
th{color:#9aa0ad;font-weight:500;width:190px}
code{font-family:Consolas,monospace;color:#9fe3ee;word-break:break-all}
.warn{background:#2a1f14;border:1px solid #6b4a24;color:#ffd08a;padding:10px 14px;border-radius:8px;margin:12px 0}
.ok{color:#7ee787}.bad{color:#ff7b72}
.foot{color:#777e8c;font-size:13px;margin-top:20px}
@media(max-width:760px){.cards{grid-template-columns:1fr 1fr}th{width:130px}}
</style></head><body><div class="wrap">
<!--BODY-->
</div></body></html>
"""


def render_html(log: Log, st: dict, integ: dict, ident: dict, png_path: str,
                json_path: str, csv_path: str, meta_path: str, path: str) -> None:
    b64 = base64.b64encode(open(png_path, "rb").read()).decode("ascii")
    meta = log.meta or {}
    is_meta = bool(log.meta)
    if is_meta:
        src = ("Flash 落盘（META）" if log.source == "flash" else
               "bin 文件（META）" if log.source == "bin" else
               "SRAM2 实时" if log.source == "live" else "CSV")
    else:
        src = {"flash": "Flash 落盘（旧 FLY1）", "live": "SRAM2 实时",
               "bin": "bin 文件（旧 FLY1）", "csv": "CSV"}.get(log.source, log.source)
    ver = (meta.get("fw_tag") or log.version or "未知")
    if is_meta:
        fw_disp = f"{ver}（日志写入固件）"
    elif log.source in ("flash", "bin"):
        fw_disp = (f"{log.version}（读取时固件；旧 FLY1 不含版本）"
                   if log.version else "未知（旧 FLY1 不含版本字段）")
    else:
        fw_disp = ver
    cards = [
        ("起始温度", f"{st.get('start_c', '?')}°C"),
        ("峰值", f"{st.get('max_c', '?')}°C @{st.get('peak_time_s', 0):.1f}s"),
        ("末点", f"{st.get('end_c', '?')}°C"),
        ("末20s", f"{st.get('last20_mean_c', 0):.1f} ± {st.get('last20_std_c', 0):.1f}°C"),
        ("到温时间", f"{st.get('time_to_target_s', 0):.1f}s" if st.get("time_to_target_s") is not None else "—"),
        ("末20s duty", f"{st.get('last20_duty_mean_pct', 0):.1f}%"),
    ]
    card_html = "".join(f'<div class="card"><div class="k">{k}</div><div class="v">{v}</div></div>' for k, v in cards)
    settings_label = "（运行设置）" if is_meta else "（读取时设置）"
    meta_rows = [
        ("产品 / 固件", f"{log.product_name} / {fw_disp}"),
        ("日志来源", src),
        ("日志格式", f"{log.layout}（{'60ms/点' if log.sample_s == NEW_SAMPLE_S else '100ms/点'}）" + (" · META" if is_meta else "")),
        ("点数 / cnt", f"{len(log.t)} / {log.cnt}"),
        ("时长", f"{st.get('duration_s', 0):.2f}s"),
        ("目标 / LM", f"{log.target_c if log.target_c is not None else '?'}°C / {log.limit_pct if log.limit_pct is not None else '?'}%{settings_label}"),
        ("PDO 档位", str(log.pdo_gear) if log.pdo_gear is not None else "—"),
    ]
    if is_meta:
        meta_rows += [
            ("运行序号", f"#{meta.get('run_seq')}"),
            ("设备 UID", f"<code>{meta.get('uid', '')}</code>"),
            ("运行时 PID", f"P {meta.get('pid_p100', 0) / 100.0:.2f} / I {meta.get('pid_i1000', 0) / 1000.0:.3f} / D {meta.get('pid_d100', 0) / 100.0:.2f}"),
        ]
        av = meta.get("avg_vin_dv", 0)
        ai = meta.get("avg_iout_ma", 0)
        ap = meta.get("avg_power_w10", 0)
        if av or ai or ap:
            meta_rows.append(("平均 V/I/P", f"{av / 10.0:.1f}V / {ai}mA / {ap / 10.0:.1f}W"))
    meta_rows += [
        ("数据 ID（烙铁显示）", f"<code>{ident['short_id']}</code>"),
        ("完整 CRC64", f"<code>{ident['crc64']}</code>"),
        ("MD5", f"<code>{ident['md5']}</code>"),
        ("标识算法", f"<code>{ident['algorithm']}</code>"),
        ("生成时间", _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")),
        ("文件", f"CSV: {os.path.basename(csv_path)}<br>JSON: {os.path.basename(json_path)}<br>PNG: {os.path.basename(png_path)}"),
    ]
    meta_html = "".join(f"<tr><th>{k}</th><td>{v}</td></tr>" for k, v in meta_rows)
    integ_rows = [
        ("温度范围", f"{st.get('min_c', '?')}–{st.get('max_c', '?')}°C"),
        ("raw 范围", f"{st.get('raw_min', '?')}–{st.get('raw_max', '?')}"),
        ("duty 范围", f"0–{st.get('duty_max_pct', 0):.1f}%"),
        ("异常计数", f"temp {integ['bad_temp']} / raw {integ['bad_raw']} / duty {integ['bad_duty']}"),
        ("末尾 D", f"<code>{integ['tail_d']}</code>"),
        ("末尾 R", f"<code>{integ['tail_r']}</code>"),
        ("完整性", '<span class="ok">OK</span>' if integ["ok"] else '<span class="bad">有异常</span>'),
    ]
    integ_html = "".join(f"<tr><th>{k}</th><td>{v}</td></tr>" for k, v in integ_rows)
    notes = "".join(f'<div class="warn">{n}</div>' for n in log.notes)
    show_ver = ver if (is_meta or log.source not in ("flash", "bin")) else ""
    title_ver = f" {show_ver}" if show_ver else ""
    max_i = max(0, len(log.t) - 1)
    t_js = json.dumps(log.t, separators=(",", ":"))
    d_js = json.dumps(log.d, separators=(",", ":"))
    r_js = json.dumps(log.r, separators=(",", ":"))
    chart_js = r"""<script>
(function(){
  const T=__T__, D=__D__, R=__R__, DT=__DT__, DUR=__DUR__;
  const N=T.length, W=1200, L=90, RM=110, PWM=319;
  const img=document.getElementById('bbImg'), cur=document.getElementById('bbCursor');
  const slider=document.getElementById('bbSlider'), out=document.getElementById('bbReadout');
  function show(idx, sync){
    if(N<=0) return;
    idx=Math.max(0,Math.min(N-1, idx|0));
    const t=idx*DT;
    const left=((L+(W-L-RM)*t/DUR)/W)*100;
    cur.style.display='block'; cur.style.left=left+'%';
    if(sync) slider.value=idx;
    const d=(D[idx]===undefined?0:D[idx]), r=(R[idx]===undefined?0:R[idx]);
    out.textContent=t.toFixed(2)+' s · '+T[idx]+'°C · duty '+(d*100.0/PWM).toFixed(1)+'% · raw '+r;
  }
  function fromEvent(e){
    const rect=img.getBoundingClientRect();
    const px=(e.clientX-rect.left)/rect.width*W;
    const t=((px-L)/(W-L-RM))*DUR;
    show(Math.round(t/DT), true);
  }
  img.addEventListener('mousemove', fromEvent);
  img.addEventListener('click', fromEvent);
  img.addEventListener('touchmove', function(e){ if(e.touches[0]) fromEvent(e.touches[0]); }, {passive:true});
  slider.addEventListener('input', function(){ show(+slider.value, false); });
  const m=(location.hash||'').match(/idx=(\d+)/);
  show(m ? +m[1] : 0, true);
})();
</script>""".replace("__T__", t_js).replace("__D__", d_js).replace("__R__", r_js).replace("__DT__", repr(log.sample_s)).replace("__DUR__", repr(float(st.get("duration_s", 0.0))))
    body = f"""
<h1>{log.product_name}{title_ver} · 黑匣子 {log.mode_name}</h1>
<div class="sub">{src} · {st.get('duration_s', 0):.1f}s · {len(log.t)} 点 · 数据ID {ident['short_id']}</div>
{notes}
<div class="cards">{card_html}</div>
<div class="chart">
  <div class="chartbox"><img id="bbImg" alt="temperature and duty chart" src="data:image/png;base64,{b64}"><div id="bbCursor"></div></div>
  <div class="ctl"><input id="bbSlider" type="range" min="0" max="{max_i}" value="0" step="1"><div id="bbReadout" class="readout"></div></div>
  <div class="hint">鼠标移动/拖动滑块查看该点温度、duty、raw；功率未逐点记录，duty 是功率代理。</div>
</div>
{chart_js}
<h3>运行信息</h3><table>{meta_html}</table>
<h3>完整性检查</h3><table>{integ_html}</table>
<div class="warn">黑匣子只记录温度、占空比和原始 ADC，<b>没有记录逐点 V/I/P</b>；图中的 duty 是功率代理，不是实际瓦数。</div>
<div class="foot">数据 ID 是与烙铁底栏一致的 12 位码（CRC64/ECMA 的低 48 位）；完整 CRC64 和 MD5 用于归档核对。旧 FLY1 日志没有元数据时，工具回退到 8 位 CRC32。META 页记录写入时的固件、UID、运行序号和设置。</div>
"""
    html = HTML_HEAD.replace("<!--BODY-->", body)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)


def write_csv(log: Log, path: str) -> None:
    n = max(len(log.t), len(log.d), len(log.r))
    with open(path, "w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["index", "time_s", "temp_c", "raw", "duty", "duty_pct"])
        for i in range(n):
            temp = log.t[i] if i < len(log.t) else ""
            raw = log.r[i] if i < len(log.r) else ""
            duty = log.d[i] if i < len(log.d) else ""
            pct = f"{log.d[i] * 100.0 / PWM_MAX:.1f}" if i < len(log.d) else ""
            w.writerow([i, f"{i * log.sample_s:.2f}", temp, raw, duty, pct])


def _basename(log: Log, ident: dict) -> str:
    product = log.product or "blackbox"
    ver = log.version.lower() if log.version else ""
    stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    sid = ident["short_id"].replace("-", "")
    return f"{product}_{ver + '_' if ver else ''}{log.source}_{stamp}_{sid}"


def _series_rows(log: Log, target: int = 240) -> list[list]:
    n = len(log.t)
    if n == 0:
        return []
    step = max(1, n // max(1, target))
    idx = list(range(0, n, step))
    if idx[-1] != n - 1:
        idx.append(n - 1)
    rows = []
    for i in idx:
        d = log.d[i] if i < len(log.d) else 0
        r = log.r[i] if i < len(log.r) else 0
        rows.append([round(i * log.sample_s, 3), int(log.t[i]), int(r), int(d),
                     round(d * 100.0 / PWM_MAX, 1)])
    return rows


def _pid_of(log: Log) -> dict | None:
    m = log.meta or {}
    if all(k in m for k in ("pid_p100", "pid_i1000", "pid_d100")):
        return {
            "p": m["pid_p100"] / 100.0,
            "i": m["pid_i1000"] / 1000.0,
            "d": m["pid_d100"] / 100.0,
        }
    return None


def _ai_prompt(log: Log) -> str:
    return (
        f"你是焊笔温控分析助手。请分析这份 {log.product_name} 黑匣子日志："
        "上升时间、超调、稳态误差/波动、duty 是否饱和或振荡，"
        "并结合 target/LM/PID 给出 PID 调整建议。"
        "注意：日志只记录温度、duty、原始 ADC，没有逐点 V/I/P；"
        "duty 是功率代理，不是实际瓦数；建议必须实机验证。"
    )


def _ai_markdown(log: Log, st: dict, integ: dict, ident: dict, files: dict) -> str:
    meta = log.meta or {}
    pid = _pid_of(log)
    pid_txt = (f"P {pid['p']:.2f} / I {pid['i']:.3f} / D {pid['d']:.2f}"
               if pid else "未知（旧日志/无元数据）")
    rows = _series_rows(log, 120)
    table = "\n".join(
        f"| {r[0]:.2f} | {r[1]} | {r[2]} | {r[3]} | {r[4]:.1f} |" for r in rows
    )
    return f"""# {log.product_name} {log.version or ''} 黑匣子分析包

- 数据 ID（烙铁底栏 12 位/旧日志 8 位）：**{ident['short_id']}**
- 固件：{log.version or '未知'}    运行序号：{meta.get('run_seq', '—')}    UID：{meta.get('uid', '—')}
- 来源：{log.source} / {log.layout}    模式：{log.mode_name}
- 点数：{len(log.t)} / cnt {log.cnt}    时长：{st.get('duration_s', 0):.2f} s
- 目标 / LM / PDO：{log.target_c}°C / {log.limit_pct}% / {log.pdo_gear}
- PID（运行时）：{pid_txt}
- 起始 / 峰值 / 末点：{st.get('start_c')} / {st.get('max_c')} / {st.get('end_c')} °C
- 末 20s：{st.get('last20_mean_c', 0):.1f} ± {st.get('last20_std_c', 0):.1f}°C，duty 均值 {st.get('last20_duty_mean_pct', 0):.1f}%
- 完整性：{'OK' if integ['ok'] else '有异常'}（temp {integ['bad_temp']} / raw {integ['bad_raw']} / duty {integ['bad_duty']}）
- 完整 CSV：{os.path.basename(files['csv'])}

## 给 AI 的提示

{_ai_prompt(log)}

## 关键数据（降采样；完整数据见 CSV）

| time_s | temp_c | raw | duty_counts | duty_pct |
|---:|---:|---:|---:|---:|
{table}

> 黑匣子没有记录逐点 V/I/P；duty 是功率代理。AI 给出的 PID 建议需要上板验证。
"""


def write_ai_bundle(log: Log, st: dict, integ: dict, ident: dict, files: dict) -> None:
    meta = log.meta or {}
    ai = {
        "schema": "pd-blackbox-ai-v1",
        "generated_at": _dt.datetime.now().isoformat(timespec="seconds"),
        "product": log.product_name,
        "firmware": log.version,
        "source": log.source,
        "layout": log.layout,
        "mode": log.mode_name,
        "identity": ident,
        "run": {
            "run_seq": meta.get("run_seq"),
            "uid": meta.get("uid"),
            "cnt": log.cnt,
            "samples": len(log.t),
            "sample_s": log.sample_s,
            "duration_s": st.get("duration_s"),
        },
        "settings": {
            "target_c": log.target_c,
            "limit_pct": log.limit_pct,
            "pdo_gear": log.pdo_gear,
            "pid": _pid_of(log),
        },
        "stats": st,
        "integrity": integ,
        "series_columns": ["time_s", "temp_c", "raw", "duty_counts", "duty_pct"],
        "series": _series_rows(log, 240),
        "files": files,
        "instructions": _ai_prompt(log),
        "notes": log.notes,
    }
    with open(files["ai_json"], "w", encoding="utf-8", newline="\n") as fh:
        json.dump(ai, fh, ensure_ascii=False, indent=2)
    with open(files["ai_md"], "w", encoding="utf-8", newline="\n") as fh:
        fh.write(_ai_markdown(log, st, integ, ident, files))


def write_export_zip(files: dict) -> None:
    has_ai = all(k in files and os.path.exists(files[k]) for k in ("ai_md", "ai_json"))
    readme = (
        "PD210 / PD245 black-box export\n"
        "============================\n\n"
        "- *.csv               : 完整逐点数据\n"
        "- *.json              : 元数据 / 统计 / 完整性\n"
        "- *.html              : 可交互报告（浏览器打开）\n"
        "- *.png               : 曲线图\n"
        + ("- *.ai.md / *.ai.json : AI 摘要（本次已包含）\n" if has_ai else
           "- AI 摘要未包含；在工具中按需导出后重新打包\n")
        + "\n黑匣子只记录 T/D/R；duty 是功率代理，不是实际瓦数。\n"
    )
    with zipfile.ZipFile(files["zip"], "w", zipfile.ZIP_DEFLATED) as zf:
        for key in ("csv", "json", "png", "html"):
            zf.write(files[key], os.path.basename(files[key]))
        if has_ai:
            for key in ("ai_md", "ai_json"):
                zf.write(files[key], os.path.basename(files[key]))
        zf.writestr(os.path.splitext(os.path.basename(files["zip"]))[0] + "_README.txt", readme)


def process(log: Log, outdir: str, want_ai: bool = False) -> dict:
    if not log.t:
        msg = "没有可导出的数据：实时缓冲为空，或日志没有样本。"
        if log.notes:
            msg += "\n" + "\n".join(log.notes)
        raise ToolError(msg)
    if log.product is None:
        log.product = product_from_version(log.version)
    st = analyze(log)
    integ = integrity(log)
    ident = identity(log)
    os.makedirs(outdir, exist_ok=True)
    base = _basename(log, ident)
    files = {
        "csv": os.path.join(outdir, base + ".csv"),
        "json": os.path.join(outdir, base + ".json"),
        "png": os.path.join(outdir, base + ".png"),
        "html": os.path.join(outdir, base + ".html"),
        "zip": os.path.join(outdir, base + "_bundle.zip"),
    }
    if want_ai:
        files["ai_md"] = os.path.join(outdir, base + ".ai.md")
        files["ai_json"] = os.path.join(outdir, base + ".ai.json")
    write_csv(log, files["csv"])
    render_png(log, st, ident, files["png"])
    meta = {
        "generated_at": _dt.datetime.now().isoformat(timespec="seconds"),
        "source": log.source,
        "layout": log.layout,
        "product": log.product,
        "version": log.version,
        "cnt": log.cnt,
        "samples": len(log.t),
        "sample_s": log.sample_s,
        "duration_s": st.get("duration_s"),
        "target_c": log.target_c,
        "limit_pct": log.limit_pct,
        "pdo_gear": log.pdo_gear,
        "stats": st,
        "integrity": integ,
        "identity": ident,
        "fly_meta": log.meta,
        "notes": log.notes,
        "files": files,
    }
    with open(files["json"], "w", encoding="utf-8", newline="\n") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
    render_html(log, st, integ, ident, files["png"], files["json"], files["csv"], files["json"], files["html"])
    if want_ai:
        write_ai_bundle(log, st, integ, ident, files)
    write_export_zip(files)
    return {"log": log, "stats": st, "integrity": integ, "identity": ident, "files": files}


def export_ai(result: dict) -> dict:
    files = result["files"]
    base = os.path.splitext(files["csv"])[0]
    files.setdefault("ai_md", base + ".ai.md")
    files.setdefault("ai_json", base + ".ai.json")
    write_ai_bundle(result["log"], result["stats"], result["integrity"],
                    result["identity"], files)
    try:
        with open(files["json"], encoding="utf-8") as fh:
            meta = json.load(fh)
        meta["files"] = files
        with open(files["json"], "w", encoding="utf-8", newline="\n") as fh:
            json.dump(meta, fh, ensure_ascii=False, indent=2)
    except Exception:
        pass
    write_export_zip(files)
    return {"ai_md": files["ai_md"], "ai_json": files["ai_json"], "zip": files["zip"]}


def summary_lines(result: dict) -> list[str]:
    log: Log = result["log"]
    st = result["stats"]
    ident = result["identity"]
    lines = [
        f"产品/固件: {log.product_name} / {log.version or '未知'}",
        f"来源/格式: {log.source} / {log.layout}    点数: {len(log.t)}    cnt: {log.cnt}",
        f"时长: {st.get('duration_s', 0):.2f}s    目标: {log.target_c}°C    LM: {log.limit_pct}%",
        f"起始: {st.get('start_c')}°C    峰值: {st.get('max_c')}°C @{st.get('peak_time_s', 0):.1f}s    末点: {st.get('end_c')}°C",
        f"末20s: {st.get('last20_mean_c', 0):.1f} ± {st.get('last20_std_c', 0):.1f}°C    duty: {st.get('last20_duty_mean_pct', 0):.1f}%",
        f"数据ID: {ident['short_id']}    MD5: {ident['md5']}",
    ]
    if log.meta:
        lines.insert(1, f"元数据: FW {log.meta.get('fw_tag')} · RUN #{log.meta.get('run_seq')} · UID {str(log.meta.get('uid',''))[:8]}…")
    lines += [f"{key.upper()}: {result['files'][key]}" for key in
              ("csv", "json", "png", "html", "ai_md", "ai_json", "zip")
              if key in result["files"]]
    if "ai_md" not in result["files"]:
        lines.append("AI: 未导出（需要时加 --ai，或在 GUI 点“导出 AI 摘要”）")
    return lines


def print_summary(result: dict) -> None:
    print("[OK]")
    for line in summary_lines(result):
        print("     " + line)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def run_cli(args) -> int:
    try:
        if args.csv:
            log = read_csv_log(args.csv, args.target, args.lm)
        elif args.bin:
            log = read_bin_log(args.bin)
            if args.target is not None:
                log.target_c = args.target
            if args.lm is not None:
                log.limit_pct = args.lm
        else:
            log = read_probe("live" if args.live else "flash", args.product)
            if args.target is not None:
                log.target_c = args.target
            if args.lm is not None:
                log.limit_pct = args.lm
        result = process(log, args.outdir, want_ai=args.ai)
        print_summary(result)
        return 0
    except ToolError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 2
    except Exception as exc:  # noqa: BLE001
        print(f"[ERROR] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 3


def run_gui() -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    BG = "#f3f4f6"
    RAIL = "#ffffff"
    PANEL = "#ffffff"
    CARD = "#fbfcfd"
    FIELD = "#ffffff"
    BORDER = "#dbe0e6"
    TEXT = "#1a1d23"
    MUTED = "#667085"
    DIM = "#98a2b3"
    CYAN = "#0e7490"
    ACCENT_STRONG = "#155e75"
    ACCENT_SOFT = "#e9f5f8"
    ACCENT_LINE = "#7cc7d6"
    LOG_BG = "#fafbfc"
    SEG_BG = "#eef1f4"
    GREEN = "#1a7f37"
    RED = "#c0392b"

    UI = "Microsoft YaHei UI"
    MONO = "Consolas"

    try:
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

    root = tk.Tk()
    scale = root.winfo_fpixels("1i") / 96.0

    def px(n: float) -> int:
        return max(1, int(round(n * scale)))

    root.title("PD 黑匣子读取工具（只读）")
    root.geometry(f"{px(1000)}x{px(660)}")
    root.minsize(px(900), px(600))
    root.configure(bg=BG)

    def make_icon() -> tk.PhotoImage:
        img = tk.PhotoImage(width=32, height=32)
        pts = [(5, 23), (11, 11), (17, 18), (22, 7), (27, 14)]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            steps = max(abs(x1 - x0), abs(y1 - y0))
            for i in range(steps + 1):
                x = round(x0 + (x1 - x0) * i / steps)
                y = round(y0 + (y1 - y0) * i / steps)
                img.put(CYAN, to=(x - 1, y - 1, x + 1, y + 1))
        return img

    try:
        root.iconphoto(True, make_icon())
    except Exception:
        pass
    root.update_idletasks()
    try:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        dwmapi = ctypes.windll.dwmapi
        user32.GetParent.argtypes = [wintypes.HWND]
        user32.GetParent.restype = wintypes.HWND
        dwmapi.DwmSetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD,
                                                 ctypes.c_void_p, wintypes.DWORD]
        dwmapi.DwmSetWindowAttribute.restype = ctypes.c_long
        hwnd = user32.GetParent(root.winfo_id())
        dark = ctypes.c_int(0)
        for attr in (20, 19):
            if dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(dark),
                                            ctypes.sizeof(dark)) == 0:
                break
        user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                        ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                        wintypes.UINT]
        user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                            0x0020 | 0x0002 | 0x0004 | 0x0001)
    except Exception:
        pass

    style = ttk.Style()
    try:
        style.theme_use("clam")
    except Exception:
        pass
    style.configure(".", background=BG, foreground=TEXT, fieldbackground=FIELD,
                    bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER,
                    troughcolor=FIELD, focuscolor=CYAN)
    style.configure("Title.TLabel", background=BG, foreground=TEXT,
                    font=(UI, 18, "bold"))
    style.configure("Sub.TLabel", background=BG, foreground=MUTED,
                    font=(MONO, 8))
    style.configure("Badge.TLabel", background="#e8f5ec", foreground=GREEN,
                    font=(UI, 9), padding=(px(10), px(4)))
    style.configure("TEntry", fieldbackground=FIELD, foreground=TEXT,
                    insertcolor=CYAN, bordercolor=BORDER, lightcolor=BORDER,
                    darkcolor=BORDER, padding=px(6))
    style.configure("Primary.TButton", background=CYAN, foreground="#ffffff",
                    bordercolor=CYAN, focusthickness=1, focuscolor="#67c6d8",
                    padding=(px(16), px(10)), font=(UI, 10, "bold"))
    style.map("Primary.TButton",
              background=[("active", ACCENT_STRONG), ("disabled", "#cdd8dd")],
              foreground=[("disabled", "#8695a0")])
    style.configure("Secondary.TButton", background=PANEL, foreground=TEXT,
                    bordercolor=BORDER, padding=(px(12), px(7)),
                    font=(UI, 9))
    style.map("Secondary.TButton",
              background=[("active", "#f1f4f7"), ("disabled", "#f7f8fa")],
              foreground=[("disabled", DIM)])

    src_labels = {"落盘日志": "flash", "实时缓冲": "live", "CSV 文件": "csv"}
    prod_labels = {"自动": "auto", "PD210": "pd210", "PD245": "pd245"}
    src_desc = {
        "flash": "读取最近一次已保存的日志",
        "live": "需要正在记录并加热",
        "csv": "从已有 CSV 重建报告",
    }

    header = tk.Frame(root, bg=BG)
    header.pack(fill="x", padx=px(22), pady=(px(16), px(12)))
    mark = tk.Canvas(header, width=px(34), height=px(34), bg=BG, highlightthickness=0)
    mark.create_oval(px(2), px(2), px(32), px(32), outline="#c9d2dc", width=1)
    mark.create_line(px(9), px(23), px(15), px(13), px(21), px(19), px(26), px(10),
                     fill=CYAN, width=px(2))
    mark.pack(side="left", padx=(0, px(12)))
    titles = tk.Frame(header, bg=BG)
    titles.pack(side="left")
    tk.Label(titles, text="PD 黑匣子读取工具", bg=BG, fg=TEXT,
             font=(UI, 18, "bold")).pack(anchor="w")
    ttk.Label(titles, text="READ-ONLY · PW LINK / PYOCD",
              style="Sub.TLabel").pack(anchor="w", pady=(px(1), 0))
    ttk.Label(header, text="只读", style="Badge.TLabel").pack(side="right")

    body = tk.Frame(root, bg=BG)
    body.pack(fill="both", expand=True, padx=px(22), pady=(0, px(16)))
    rail = tk.Frame(body, bg=RAIL, width=px(340), highlightbackground=BORDER,
                    highlightthickness=1)
    rail.pack(side="left", fill="y")
    rail.pack_propagate(False)
    right = tk.Frame(body, bg=BG)
    right.pack(side="left", fill="both", expand=True, padx=(px(18), 0))

    rail_inner = tk.Frame(rail, bg=RAIL, padx=px(18), pady=px(16))
    rail_inner.pack(fill="both", expand=True)
    tk.Label(rail_inner, text="读取设置", bg=RAIL, fg=TEXT,
             font=(UI, 11, "bold")).pack(anchor="w")
    tk.Label(rail_inner, text="按步骤选择，然后开始读取", bg=RAIL, fg=MUTED,
             font=(UI, 9)).pack(anchor="w", pady=(px(2), px(18)))

    def step(parent, num: str, label: str):
        row = tk.Frame(parent, bg=RAIL)
        row.pack(fill="x", pady=(0, px(7)))
        tk.Label(row, text=num, bg=RAIL, fg=CYAN, font=(MONO, 9, "bold")).pack(side="left")
        tk.Label(row, text=label, bg=RAIL, fg=TEXT, font=(UI, 10)).pack(side="left", padx=(px(8), 0))
        return row

    prod_var = tk.StringVar(value="auto")
    src_var = tk.StringVar(value="flash")
    busy = {"value": False}
    focus_src = {"key": None}

    step(rail_inner, "01", "设备型号")
    seg = tk.Frame(rail_inner, bg=SEG_BG, highlightbackground=BORDER, highlightthickness=1)
    seg.pack(fill="x")
    prod_buttons: dict[str, tk.Button] = {}
    for label, key in prod_labels.items():
        b = tk.Button(seg, text=label, relief="flat", bd=0,
                      highlightthickness=1, highlightbackground=SEG_BG,
                      highlightcolor=CYAN, bg=SEG_BG, fg=MUTED,
                      activebackground="#e3e8ee", activeforeground=CYAN,
                      disabledforeground=DIM, font=(UI, 9), pady=px(6),
                      cursor="hand2", takefocus=True,
                      command=lambda k=key: select_product(k))
        b.pack(side="left", fill="x", expand=True, padx=px(3), pady=px(3))
        prod_buttons[key] = b

    tk.Frame(rail_inner, bg=RAIL, height=px(12)).pack(fill="x")

    step(rail_inner, "02", "数据来源")
    src_cards: dict[str, tuple[tk.Frame, tk.Label, tk.Label, tk.Canvas]] = {}
    for label, key in src_labels.items():
        f = tk.Frame(rail_inner, bg=CARD, highlightbackground=BORDER,
                     highlightthickness=1, padx=px(11), pady=px(8), cursor="hand2",
                     takefocus=True)
        f.pack(fill="x", pady=(px(4), 0))
        top = tk.Frame(f, bg=CARD)
        top.pack(fill="x")
        dot = tk.Canvas(top, width=px(14), height=px(14), bg=CARD, highlightthickness=0)
        dot.pack(side="left", padx=(0, px(8)))
        title = tk.Label(top, text=label, bg=CARD, fg=TEXT, font=(UI, 10), anchor="w")
        title.pack(side="left", fill="x", expand=True)
        desc = tk.Label(f, text=src_desc[key], bg=CARD, fg=MUTED,
                        font=(UI, 8), anchor="w")
        desc.pack(fill="x", padx=(px(22), 0), pady=(px(1), 0))
        for w in (f, top, title, desc, dot):
            w.bind("<Button-1>", lambda e, k=key: select_source(k))
        f.bind("<Return>", lambda e, k=key: select_source(k))
        f.bind("<space>", lambda e, k=key: select_source(k))
        f.bind("<FocusIn>", lambda e, k=key: set_src_focus(k))
        f.bind("<FocusOut>", lambda e, k=key: set_src_focus(None))
        src_cards[key] = (f, title, desc, dot)

    tk.Frame(rail_inner, bg=RAIL, height=px(12)).pack(fill="x")

    step(rail_inner, "03", "输出目录")
    outdir = tk.StringVar(value=r"D:\tmp\pd_blackbox")
    path_row = tk.Frame(rail_inner, bg=RAIL)
    path_row.pack(fill="x")
    out_entry = ttk.Entry(path_row, textvariable=outdir)
    out_entry.pack(side="left", fill="x", expand=True, padx=(0, px(8)))
    browse_btn = ttk.Button(path_row, text="浏览", width=5, style="Secondary.TButton")
    browse_btn.pack(side="left")

    caption = tk.Label(rail_inner, text="只读 · 不复位 · 不烧录", bg=RAIL,
                       fg=MUTED, font=(UI, 8))
    caption.pack(side="bottom", pady=(px(8), 0))
    read_btn = ttk.Button(rail_inner, text="读取并生成报告", style="Primary.TButton")
    read_btn.pack(side="bottom", fill="x", pady=(px(14), 0))

    right_head = tk.Frame(right, bg=BG)
    right_head.pack(fill="x", pady=(0, px(10)))
    status_dot = tk.Canvas(right_head, width=px(10), height=px(10), bg=BG, highlightthickness=0)
    status_dot.pack(side="left", padx=(0, px(8)))
    status_dot.create_oval(px(1), px(1), px(9), px(9), fill=MUTED, outline="", tags="dot")
    status_lbl = tk.Label(right_head, text="就绪", bg=BG, fg=MUTED,
                          font=(UI, 11, "bold"))
    status_lbl.pack(side="left")
    ttk.Label(right_head, text="运行日志", style="Sub.TLabel").pack(side="right")

    log_panel = tk.Frame(right, bg=PANEL, highlightbackground=BORDER,
                         highlightthickness=1)
    log_panel.pack(fill="both", expand=True)
    log_box = tk.Frame(log_panel, bg=LOG_BG)
    log_box.pack(fill="both", expand=True, padx=px(12), pady=px(12))
    status = tk.Text(log_box, height=15, width=1, bg=LOG_BG, fg="#303844",
                     insertbackground=CYAN, selectbackground="#d7eef4",
                     selectforeground="#0b2b33", font=(MONO, 10), relief="flat",
                     borderwidth=0, highlightthickness=0, wrap="word",
                     padx=px(2), pady=px(2))
    status.pack(fill="both", expand=True)
    status.tag_configure("ok", foreground=GREEN)
    status.tag_configure("err", foreground=RED)
    status.tag_configure("path", foreground=CYAN)
    status.tag_configure("dim", foreground="#6b7280")
    status.insert("end", "只读工具 · 不复位、不烧录。\n"
                         "先完全退出 PowerWriter，再给设备断电重上电或按复位。\n"
                         "读取完成后生成 HTML 报告；需要 AI 摘要时点“导出 AI 摘要”。\n",
                  "dim")

    bottom = tk.Frame(right, bg=BG)
    bottom.pack(fill="x", pady=(px(10), 0))
    auto_open = tk.BooleanVar(value=True)
    auto_btn = tk.Checkbutton(bottom, text="生成后自动打开 HTML", variable=auto_open,
                              bg=BG, fg=MUTED, activebackground=BG, activeforeground=TEXT,
                              selectcolor=PANEL, highlightthickness=0, borderwidth=0,
                              font=(UI, 9), cursor="hand2")
    auto_btn.pack(side="left")
    last_html: dict[str, str | None] = {"path": None}
    last_result: dict = {"result": None, "ai_md": None}
    open_dir_btn = ttk.Button(bottom, text="打开输出目录", style="Secondary.TButton")
    open_dir_btn.pack(side="right")
    ai_btn = ttk.Button(bottom, text="导出 AI 摘要", style="Secondary.TButton",
                        state="disabled")
    ai_btn.pack(side="right", padx=(0, px(8)))
    html_btn = ttk.Button(bottom, text="打开最新 HTML", style="Secondary.TButton",
                          state="disabled")
    html_btn.pack(side="right", padx=(0, px(8)))

    def set_status(text: str, color: str):
        status_lbl.configure(text=text, fg=color)
        status_dot.itemconfigure("dot", fill=color)

    def refresh_products():
        for k, b in prod_buttons.items():
            if k == prod_var.get():
                b.configure(bg="#ffffff", fg=CYAN, activebackground="#ffffff",
                            highlightbackground="#ffffff")
            else:
                b.configure(bg=SEG_BG, fg=MUTED, activebackground="#e3e8ee",
                            highlightbackground=SEG_BG)

    def refresh_sources():
        cur = src_var.get()
        focused = focus_src["key"]
        for k, (f, title, desc, dot) in src_cards.items():
            selected = (k == cur)
            bg = ACCENT_SOFT if selected else CARD
            border = ACCENT_LINE if selected else ("#8fb8c4" if k == focused else BORDER)
            f.configure(bg=bg, highlightbackground=border)
            title.configure(bg=bg, fg=(ACCENT_STRONG if selected or k == focused else TEXT))
            desc.configure(bg=bg, fg=MUTED)
            dot.configure(bg=bg)
            dot.delete("all")
            dot.create_oval(px(1), px(1), px(12), px(12),
                            outline=(CYAN if selected else "#9aa6b2"), width=1)
            if selected:
                dot.create_oval(px(4), px(4), px(9), px(9), fill=CYAN, outline="")

    def select_product(key: str):
        if busy["value"]:
            return
        prod_var.set(key)
        refresh_products()

    def select_source(key: str):
        if busy["value"]:
            return
        src_var.set(key)
        refresh_sources()

    def set_src_focus(key: str | None):
        focus_src["key"] = key
        refresh_sources()

    def append(text: str, tag: str | None = None):
        status.insert("end", text + "\n", tag or ())
        status.see("end")

    def latest(ext: str) -> str | None:
        try:
            files = [os.path.join(outdir.get(), f) for f in os.listdir(outdir.get())
                     if f.lower().endswith(ext)]
            return max(files, key=os.path.getmtime) if files else None
        except Exception:
            return None

    def update_latest():
        p = latest(".html")
        last_html["path"] = p
        html_btn.configure(state="normal" if p else "disabled")

    def browse():
        p = filedialog.askdirectory(initialdir=outdir.get() or "D:/tmp")
        if p:
            outdir.set(p)
            update_latest()

    def set_busy(b: bool):
        busy["value"] = b
        read_btn.configure(state="disabled" if b else "normal")
        for btn in prod_buttons.values():
            btn.configure(state="disabled" if b else "normal")
        out_entry.configure(state="disabled" if b else "normal")
        browse_btn.configure(state="disabled" if b else "normal")

    def done(result: dict):
        set_busy(False)
        log = result["log"]
        files = result["files"]
        set_status("读取完成", GREEN)
        append("")
        append(f"读取完成：{log.product_name} {log.version or ''} · {log.mode_name} · {len(log.t)} 点", "ok")
        append("导出包：" + files["zip"], "path")
        append("HTML  ：" + files["html"], "path")
        append("CSV   ：" + files["csv"], "path")
        append("如需发给 AI/同事，点“导出 AI 摘要”。", "dim")
        last_html["path"] = files["html"]
        last_result["result"] = result
        last_result["ai_md"] = None
        html_btn.configure(state="normal")
        ai_btn.configure(text="导出 AI 摘要", state="normal")
        if auto_open.get():
            try:
                os.startfile(files["html"])
            except Exception:
                pass

    def failed(msg: str):
        set_busy(False)
        last_result["result"] = None
        last_result["ai_md"] = None
        ai_btn.configure(text="导出 AI 摘要", state="disabled")
        set_status("读取失败", RED)
        append("")
        append("[失败] " + msg, "err")
        messagebox.showerror("读取失败", msg)

    def export_ai_click():
        res = last_result["result"]
        if not res:
            return
        if last_result["ai_md"]:
            try:
                os.startfile(last_result["ai_md"])
            except Exception as exc:  # noqa: BLE001
                messagebox.showerror("打开失败", f"{type(exc).__name__}: {exc}")
            return
        try:
            info = export_ai(res)
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("导出失败", f"{type(exc).__name__}: {exc}")
            return
        last_result["ai_md"] = info["ai_md"]
        ai_btn.configure(text="打开 AI 摘要")
        set_status("AI 摘要已导出", GREEN)
        append("")
        append("AI 摘要已导出：" + info["ai_md"], "ok")
        append("AI JSON：" + info["ai_json"], "path")
        append("导出包已更新（含 AI）：" + info["zip"], "path")

    def do_read():
        src = src_var.get()
        prod = prod_var.get()
        csv_path = None
        if src == "csv":
            csv_path = filedialog.askopenfilename(title="选择黑匣子 CSV",
                                                  filetypes=[("CSV", "*.csv"), ("All", "*.*")])
            if not csv_path:
                return
        out_dir = outdir.get().strip()
        if not out_dir:
            messagebox.showwarning("提示", "请先选择输出目录")
            return
        last_result["result"] = None
        last_result["ai_md"] = None
        ai_btn.configure(text="导出 AI 摘要", state="disabled")
        set_busy(True)
        set_status("读取中…", CYAN)
        status.delete("1.0", "end")
        names = {v: k for k, v in src_labels.items()}
        pnames = {v: k for k, v in prod_labels.items()}
        detail = f"CSV={csv_path}" if csv_path else f"设备={pnames.get(prod, prod)} 来源={names.get(src, src)}"
        append(f"读取中… {detail}", "dim")
        append("")

        def worker():
            try:
                if src == "csv":
                    log = read_csv_log(csv_path)
                else:
                    log = read_probe(src, prod)
                result = process(log, out_dir)
            except Exception as exc:  # noqa: BLE001
                root.after(0, failed, f"{type(exc).__name__}: {exc}")
            else:
                root.after(0, done, result)

        threading.Thread(target=worker, daemon=True).start()

    browse_btn.configure(command=browse)
    read_btn.configure(command=do_read)
    html_btn.configure(command=lambda: os.startfile(last_html["path"]) if last_html["path"] else None)
    ai_btn.configure(command=export_ai_click)
    open_dir_btn.configure(command=lambda: os.startfile(outdir.get()) if os.path.isdir(outdir.get()) else messagebox.showwarning("提示", "输出目录还不存在"))
    refresh_products()
    refresh_sources()
    update_latest()
    root.mainloop()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="PD210/PD245 黑匣子只读读取工具")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--flash", action="store_true", help="读 Flash 落盘日志（默认）")
    src.add_argument("--live", action="store_true", help="读 SRAM2 实时缓冲")
    src.add_argument("--bin", metavar="FILE", help="解析整片 256KB flash dump")
    src.add_argument("--csv", metavar="FILE", help="从已导出的 CSV 重新生成报告")
    ap.add_argument("--product", choices=["auto", "pd210", "pd245"], default="auto")
    ap.add_argument("--outdir", default=r"D:\tmp\pd_blackbox")
    ap.add_argument("--ai", action="store_true", help="同时导出 AI 摘要（.ai.md / .ai.json）")
    ap.add_argument("--target", type=int, help="覆盖目标温度（CSV/bin 用）")
    ap.add_argument("--lm", type=int, help="覆盖功率限制（CSV/bin 用）")
    ap.add_argument("--gui", action="store_true", help="打开图形界面")
    args = ap.parse_args()
    if args.gui or len(sys.argv) == 1:
        return run_gui()
    return run_cli(args)


if __name__ == "__main__":
    sys.exit(main())

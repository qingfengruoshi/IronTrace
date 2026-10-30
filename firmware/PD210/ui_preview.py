# -*- coding: utf-8 -*-
# ui_preview.py — PD245 主界面软件渲染预览器
# 完整复刻固件 lcd.c 的绘制逻辑 (5x7 字体/段码/填充), 输出 PNG — 免烧录迭代布局
# 用法: python ui_preview.py [out.png]
from PIL import Image
import sys

W, H = 160, 40
FB = [[0] * W for _ in range(H)]

# ---- 颜色 (RGB565, 与 main.c 一致) ----
C_ST, C_LM, C_RS = 0xBC21, 0xBC21, 0x2444
C_V, C_I, C_P = 0xF800, 0xC618, 0xFC06
C_TMP, C_BG, C_DIM, C_OK = 0x07FF, 0x0000, 0x4208, 0x07E0

def px(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        FB[y][x] = c

def fill(x, y, w, h, c):
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            px(xx, yy, c)

def clear(c=C_BG):
    for y in range(H):
        for x in range(W):
            FB[y][x] = c

# ---- 5x7 字体 (ASCII 0x20-0x5A, 与 lcd.c font5x7 完全相同) ----
FONT = [
 0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00,
 0x14,0x7F,0x14,0x7F,0x14, 0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62,
 0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00, 0x00,0x1C,0x22,0x41,0x00,
 0x00,0x41,0x22,0x1C,0x00, 0x14,0x08,0x3E,0x08,0x14, 0x08,0x08,0x3E,0x08,0x08,
 0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00,
 0x20,0x10,0x08,0x04,0x02, 0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00,
 0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31, 0x18,0x14,0x12,0x7F,0x10,
 0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03,
 0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, 0x00,0x36,0x36,0x00,0x00,
 0x00,0x56,0x36,0x00,0x00, 0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14,
 0x41,0x22,0x14,0x08,0x00, 0x02,0x01,0x51,0x09,0x06, 0x32,0x49,0x79,0x41,0x3E,
 0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
 0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x01,0x01,
 0x3E,0x41,0x49,0x49,0x7A, 0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00,
 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40,
 0x7F,0x02,0x04,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
 0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46,
 0x46,0x49,0x49,0x49,0x31, 0x01,0x01,0x7F,0x01,0x01, 0x3F,0x40,0x40,0x40,0x3F,
 0x1F,0x20,0x40,0x20,0x1F, 0x7F,0x20,0x18,0x20,0x7F, 0x63,0x14,0x08,0x14,0x63,
 0x03,0x04,0x78,0x04,0x03, 0x61,0x51,0x49,0x45,0x43,
]

def char(x, y, ch, fg):
    o = ord(ch)
    if o < 0x20 or o > 0x5A: o = 0x20
    i = (o - 0x20) * 5
    for col in range(5):
        bits = FONT[i + col]
        for row in range(7):
            if bits & (1 << row):
                px(x + col, y + row, fg)

def text(x, y, s, fg):
    for ch in s:
        char(x, y, ch, fg); x += 6

def char_big(x, y, ch, fg, sc):
    o = ord(ch)
    if o < 0x20 or o > 0x5A: o = 0x20
    i = (o - 0x20) * 5
    for col in range(5):
        bits = FONT[i + col]
        for row in range(7):
            if bits & (1 << row):
                for dy in range(sc):
                    for dx in range(sc):
                        px(x + col * sc + dx, y + row * sc + dy, fg)

def text_big(x, y, s, fg, sc):
    for ch in s:
        char_big(x, y, ch, fg, sc); x += 6 * sc

SEG7TAB = [0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F]

def seg7_char(x0, y, seg, fg, dw, dh, t):
    h2 = (dh - 3 * t) // 2
    if seg & 0x01: fill(x0 + t, y, dw - 2*t, t, fg)
    if seg & 0x02: fill(x0 + dw - t, y + t, t, h2, fg)
    if seg & 0x04: fill(x0 + dw - t, y + dh//2 + t//2, t, h2, fg)
    if seg & 0x08: fill(x0 + t, y + dh - t, dw - 2*t, t, fg)
    if seg & 0x10: fill(x0, y + dh//2 + t//2, t, h2, fg)
    if seg & 0x20: fill(x0, y + t, t, h2, fg)
    if seg & 0x40: fill(x0 + t, y + dh//2 - t//2, dw - 2*t, t, fg)

def seg7(x, y, value, fg, dw, dh, t):
    if value > 999: value = 999
    for d in range(3):
        v = value//100 if d == 0 else (value//10 % 10 if d == 1 else value % 10)
        x0 = x + d * (dw + 2)
        if d < 2 and value < (100 if d == 0 else 10):
            continue    # 前导零消隐
        seg7_char(x0, y, SEG7TAB[v], fg, dw, dh, t)

# ================= 布局参数 (与 main.c 的 T_* 一一对应) =================
P = dict(
    T_X0_2=51, T_X0_3=50, T_Y=4, T_DW_2=16, T_DW_3=13, T_DH=26,
    T_TH2=3, T_TH3=2, T_CX=3, T_CY=12, T_CW=12, T_CH=18, T_CT=3,
    T_DEG_Y=6, T_DOT_DX=5, T_DOT_DY=4,
    TEST_888=1, T_DOT_ON=1,
)

def draw_main(cur=26, v10=88, i_ma=0, out_pct=0, target_c=320, limit_pct=30,
              pd_v="9V", dot_color=None):
    clear()
    text(2, 2, "ST:%3dC" % target_c, C_ST)
    text(2, 13, "LM:%3d%%" % limit_pct, C_LM)
    text(2, 24, "PD:%s V71" % pd_v, C_RS)

    t = 888 if P["TEST_888"] else int(cur + 0.5)
    if t >= 100 or t < 0: n, dw, x0 = 3, P["T_DW_3"], P["T_X0_3"]
    else:                 n, dw, x0 = 2, P["T_DW_2"], P["T_X0_2"]
    seg7(x0, P["T_Y"], t, C_TMP, dw, P["T_DH"], P["T_TH3"] if n == 3 else P["T_TH2"])

    xe = x0 + n * (dw + 2) + 1
    dy = P["T_DEG_Y"]
    px(xe+1, dy, C_TMP); px(xe+2, dy, C_TMP)
    px(xe, dy+1, C_TMP); px(xe+3, dy+1, C_TMP)
    px(xe+1, dy+2, C_TMP); px(xe+2, dy+2, C_TMP)

    cx, cy, cw, ch, ct = xe+P["T_CX"], P["T_CY"], P["T_CW"], P["T_CH"], P["T_CT"]
    h2 = (ch - 3*ct)//2
    cmode = P.get("C_ROUND", 3)
    if cmode == 3:
        # ★ v73: 真正的字母 C — 连续左轨 + 上下横 + 端部圆头像素
        fill(cx+2, cy, cw-4, ct, C_TMP)                    # 上横
        px(cx+1, cy+1, C_TMP); px(cx+cw-2, cy+1, C_TMP)    # 端圆头
        fill(cx+2, cy+ch-ct, cw-4, ct, C_TMP)              # 下横
        px(cx+1, cy+ch-ct+1, C_TMP); px(cx+cw-2, cy+ch-ct+1, C_TMP)
        fill(cx, cy+ct, ct, ch-2*ct, C_TMP)                # 连续左轨 (关键差异)
    elif cmode == 2:
        # ★ v73: 柔和端 C — 段主体内缩 2px + 端部中行补像素 (圆润且不断开)
        # 上横
        fill(cx+2, cy, cw-4, ct, C_TMP)
        px(cx+1, cy+1, C_TMP); px(cx+cw-2, cy+1, C_TMP)
        # 下横
        fill(cx+2, cy+ch-ct, cw-4, ct, C_TMP)
        px(cx+1, cy+ch-ct+1, C_TMP); px(cx+cw-2, cy+ch-ct+1, C_TMP)
        # 左上竖 (顶行補像素做圆头)
        fill(cx, cy+ct, ct, h2, C_TMP)
        px(cx+1, cy+ct-1, C_TMP)
        # 左下竖
        fill(cx, cy+ch-ct-h2, ct, h2, C_TMP)
        px(cx+1, cy+ch-ct-h2+h2, C_TMP)
    elif cmode == 1:
        fill(cx+ct+1, cy, cw-2*ct-2, ct, C_TMP)
        px(cx+ct, cy+ct//2, C_TMP); px(cx+cw-ct-1, cy+ct//2, C_TMP)
        fill(cx+ct+1, cy+ch-ct, cw-2*ct-2, ct, C_TMP)
        px(cx+ct, cy+ch-ct+ct//2, C_TMP); px(cx+cw-ct-1, cy+ch-ct+ct//2, C_TMP)
        fill(cx, cy+ct+1, ct, h2-2, C_TMP)
        px(cx+ct//2, cy+ct, C_TMP); px(cx+ct//2, cy+ct+h2-1, C_TMP)
        fill(cx, cy+ch-ct-h2+1, ct, h2-2, C_TMP)
        px(cx+ct//2, cy+ch-ct-h2, C_TMP); px(cx+ct//2, cy+ch-ct-1, C_TMP)
    else:
        # 整段 C (无圆角缺口, 与数字同渲染)
        seg7_char(cx, cy, 0x39, C_TMP, cw, ch, ct)

    if P["T_DOT_ON"]:
        dc = dot_color if dot_color else 0x001F
        for yy in range(4):
            for xx in range(4):
                px(xe+P["T_DOT_DX"]+xx, P["T_DOT_DY"]+yy, dc)

    text(110, 2,  "V:%2d.%dV" % (v10//10, v10%10), C_V)
    text(110, 13, "I:%d.%02dA" % (i_ma//1000, (i_ma%1000)//10), C_I)
    p_cw = v10 * i_ma // 100
    text(110, 24, "P:%d.%02dW" % (p_cw//100, p_cw%100), C_P)
    fill(2, 34, 130, 5, C_DIM)
    w = out_pct * 130 // 100
    if w > 0: fill(2, 34, w, 5, C_I)
    text(138, 33, "%d%%" % out_pct, C_OK)

def save(path, scale=5):
    im = Image.new('RGB', (W, H))
    p = im.load()
    for y in range(H):
        for x in range(W):
            v = FB[y][x]
            p[x, y] = (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
    im = im.resize((W * scale, H * scale), Image.NEAREST)
    im.save(path)
    print("saved:", path)

if __name__ == "__main__":
    # 允许命令行覆盖参数: python ui_preview.py out.png T_X0_2=49 T_DH=28
    out = "preview.png"
    for a in sys.argv[1:]:
        if "=" in a:
            k, v = a.split("=")
            if k in P: P[k] = int(v)
        else:
            out = a
    draw_main()
    save(out)

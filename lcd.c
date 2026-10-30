/* ============================================================================
 * lcd.c — GC9D01N 0.99" 40×160 正式驱动 (硬件 SPI2 @12MHz, 16bpp, 横屏 160×40)
 *
 * ★ 几何 (全部实测标定, 2026-10-07):
 *   窗口   CASET 0..39 × RASET 0..159  (die 列 0..39 = 屏宽, die 行 0..159 = 屏长)
 *   旋转   die 行 159 = 玻璃左端 (条纹扫描"从右往左"实测)
 *   字节序 SPI 流: 高字节在前 (低字节在前会导致颜色错乱)
 *   映射   UI(x=0..159 横, y=0..39 竖) → 显存 行 r=159-x, 列 c=39-y
 *          (v2.1 只有 r 镜像→文字左右反; 补上 c 翻转后应为正像, 角块可验证)
 * ==========================================================================*/
#include "regs.h"
#include "lcd.h"
#include "hw.h"

#define LCD_CS_LOW()  gpio_pin_clr(GPIOA, 7)
#define LCD_CS_HIGH() gpio_pin_set(GPIOA, 7)
#define LCD_RES_LOW() gpio_pin_clr(GPIOB, 0)
#define LCD_RES_HIGH() gpio_pin_set(GPIOB, 0)
#define LCD_DC_LOW()  gpio_pin_clr(GPIOB, 2)
#define LCD_DC_HIGH() gpio_pin_set(GPIOB, 2)

#define W 160
#define H 40
static uint16_t fb[W * H];        /* UI 横屏帧缓冲 */
static uint16_t fb_prev[W * H];   /* 翻页动画的上一帧快照 */
static uint8_t  ram_b[W * H * 2]; /* SPI 流: 高字节在前 */
static int lcd_rot180 = 0;                 /* 加速度计驱动的 180° 翻转 (UI 空间) */
void lcd_set_rot180(int r) { lcd_rot180 = r; }

/* ---- 屏幕方向: MADCTL 硬件镜像位 (标准 TFT 做法, 8 组合覆盖全部方向) ----
 * O0-O3 = MV=0 组合 (正常), O4-O7 = MV=1 组合 (行列交换, 备用)
 * 其中必有且仅有一个正立; 用户在 PDO 页 左+右 循环选择后固化编号 */
static uint8_t lcd_ori = 0;
static const uint8_t ori_madctl[8] = {0x00, 0x40, 0x80, 0xC0, 0x20, 0x60, 0xA0, 0xE0};
int lcd_get_orientation(void) { return lcd_ori; }
void lcd_set_orientation(int o)
{
    lcd_ori = o & 7;
    uint8_t cmd = 0x36, dat = ori_madctl[lcd_ori];
    LCD_CS_LOW();
    LCD_DC_LOW();  spi2_write(&cmd, 1);  LCD_DC_HIGH();  spi2_write(&dat, 1);
    LCD_CS_HIGH();
}

static const uint8_t gc9d01_init_tab[] = {
    0xFE, 0,
    0xEF, 0,
    0x80, 1, 0xFF, 0x81, 1, 0xFF, 0x82, 1, 0xFF, 0x83, 1, 0xFF,
    0x84, 1, 0xFF, 0x85, 1, 0xFF, 0x86, 1, 0xFF, 0x87, 1, 0xFF,
    0x88, 1, 0xFF, 0x89, 1, 0xFF, 0x8A, 1, 0xFF, 0x8B, 1, 0xFF,
    0x8C, 1, 0xFF, 0x8D, 1, 0xFF, 0x8E, 1, 0xFF, 0x8F, 1, 0xFF,
    0x3A, 1, 0x05,
    0xEC, 1, 0x11,
    0x7E, 1, 0x7A,
    0x74, 7, 0x02, 0x0E, 0x00, 0x00, 0x28, 0x00, 0x00,
    0x98, 1, 0x3E,
    0x99, 1, 0x3E,
    0xB5, 2, 0x0E, 0x0E,
    0x60, 4, 0x38, 0x09, 0x6D, 0x67,
    0x63, 5, 0x38, 0xAD, 0x6D, 0x67, 0x05,
    0x64, 6, 0x38, 0x0B, 0x70, 0xAB, 0x6D, 0x67,
    0x66, 6, 0x38, 0x0F, 0x70, 0xAF, 0x6D, 0x67,
    0x6A, 2, 0x00, 0x00,
    0x68, 7, 0x3B, 0x08, 0x04, 0x00, 0x04, 0x64, 0x67,
    0x6C, 7, 0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50,
    0x6E, 32, 0x00, 0x00, 0x00, 0x00, 0x07, 0x01, 0x13, 0x11,
           0x0B, 0x09, 0x16, 0x15, 0x1D, 0x1E, 0x00, 0x00,
           0x00, 0x00, 0x1E, 0x1D, 0x15, 0x16, 0x0A, 0x0C,
           0x12, 0x14, 0x02, 0x08, 0x00, 0x00, 0x00, 0x00,
    0xA9, 1, 0x1B, 0xA8, 1, 0x6B, 0xA8, 1, 0x6D, 0xA7, 1, 0x40,
    0xAD, 1, 0x47, 0xAF, 1, 0x73, 0xAF, 1, 0x73, 0xAC, 1, 0x44,
    0xA3, 1, 0x6C, 0xCB, 1, 0x00, 0xCD, 1, 0x22, 0xC2, 1, 0x10,
    0xC5, 1, 0x00, 0xC6, 1, 0x0E, 0xC7, 1, 0x1F, 0xC8, 1, 0x0E,
    0xBF, 1, 0x00, 0xF9, 1, 0x20, 0x9B, 1, 0x3B,
    0x93, 3, 0x33, 0x7F, 0x00,
    0x70, 6, 0x0E, 0x0F, 0x03, 0x0E, 0x0F, 0x03,
    0x71, 3, 0x0E, 0x16, 0x03,
    0x91, 2, 0x0E, 0x09,
    0xC3, 1, 0x2C, 0xC4, 1, 0x1A,
    0xF0, 6, 0x51, 0x13, 0x0C, 0x06, 0x00, 0x2F,
    0xF2, 6, 0x51, 0x13, 0x0C, 0x06, 0x00, 0x33,
    0xF1, 6, 0x3C, 0x94, 0x4F, 0x33, 0x34, 0xCF,
    0xF3, 6, 0x4D, 0x94, 0x4F, 0x33, 0x34, 0xCF,
    0x36, 1, 0x40,
    0xFF, 0xFF,
};

static void lcd_win(void)
{
    uint8_t cas[5] = {0x2A, 0x00, 0x00, 0x00, 39};    /* CASET 0..39 */
    uint8_t ras[5] = {0x2B, 0x00, 0x00, 0x00, 159};   /* RASET 0..159 */
    uint8_t ramc = 0x2C;
    LCD_CS_LOW();
    LCD_DC_LOW();  spi2_write(cas, 1);  LCD_DC_HIGH();  spi2_write(cas + 1, 4);
    LCD_DC_LOW();  spi2_write(ras, 1);  LCD_DC_HIGH();  spi2_write(ras + 1, 4);
    LCD_DC_LOW();  spi2_write(&ramc, 1); LCD_DC_HIGH();
}

void lcd_init(void)
{
    LCD_CS_HIGH();
    LCD_RES_LOW(); delay_ms(20); LCD_RES_HIGH(); delay_ms(120);
    LCD_CS_LOW();
    const uint8_t *p = gc9d01_init_tab;
    for (;;) {
        uint8_t c = *p++;
        uint8_t n = *p++;
        if (c == 0xFF && n == 0xFF) break;
        LCD_DC_LOW(); spi2_write(&c, 1); LCD_DC_HIGH();
        while (n--) { uint8_t d = *p++; spi2_write(&d, 1); }
    }
    { uint8_t slp = 0x11; LCD_DC_LOW(); spi2_write(&slp, 1); }
    delay_ms(200);
    { uint8_t don = 0x29; LCD_DC_LOW(); spi2_write(&don, 1); LCD_DC_HIGH(); }
    lcd_set_orientation(0);     /* MADCTL 方向 (8 组合之一, PDO 页 L+R 可切) */
    lcd_win();
    LCD_CS_HIGH();
    lcd_clear(0x0000);      /* 首帧清屏 (背光仍灭) */
    lcd_flush();
}

void lcd_backlight_on(void) { gpio_pin_set(GPIOB, 11); }

void lcd_clear(uint16_t c)
{
    for (int i = 0; i < W * H; i++) fb[i] = c;
}

/* ★ v92: 休眠亮度 15% — 全屏帧缓冲压暗 (r/g/b 各 ×38/256 ≈ 15%) */
void lcd_dim15(void)
{
    for (int i = 0; i < W * H; i++) {
        uint16_t v = fb[i];
        uint16_t r = ((v >> 11) & 0x1F) * 38 >> 8;
        uint16_t g = ((v >> 5) & 0x3F) * 38 >> 8;
        uint16_t b = (v & 0x1F) * 38 >> 8;
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

/* v183: 帧缓冲按百分比压暗（休眠 5% / SET 亮度 5-100%） */
void lcd_dim_pct(int pct)
{
    if (pct >= 100) return;
    if (pct < 1) pct = 1;
    uint32_t k = (uint32_t)pct * 256u / 100u;
    for (int i = 0; i < W * H; i++) {
        uint16_t v = fb[i];
        uint16_t r = (uint16_t)((((v >> 11) & 0x1F) * k) >> 8);
        uint16_t g = (uint16_t)((((v >> 5) & 0x3F) * k) >> 8);
        uint16_t b = (uint16_t)(((v & 0x1F) * k) >> 8);
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void lcd_send_full(void)
{
    LCD_CS_LOW();
    lcd_win();
    spi2_write(ram_b, W * H * 2);
    LCD_CS_HIGH();
}

void lcd_save_frame(void)
{
    for (int i = 0; i < W * H; i++) fb_prev[i] = fb[i];
}

static void boot_wait(uint32_t ticks)
{
    uint32_t t = millis();
    while (millis() - t < ticks) ;
}

/* ★ v227: 原版修改器把图直接写到固件偏移 0x18000（Flash 0x08018000） */
#define BOOT_IMG_ADDR 0x08018000u

static int boot_custom_valid(void)
{
    const uint32_t *p = (const uint32_t *)BOOT_IMG_ADDR;
    uint32_t andv = 0xFFFFFFFFu, orv = 0;
    for (int i = 0; i < (W * H * 2) / 4; i++) { uint32_t v = p[i]; andv &= v; orv |= v; }
    return !(andv == 0xFFFFFFFFu || orv == 0);   /* 全 FF=未烧录，全 00=空图 */
}

/* 启动画面单帧：边框/四角 + 自定义图或 PD210 标题 + 状态字 + 分段进度条 + 百分比 + 扫描线 */
static void boot_frame(int pct, int scan_y, int phase, int flash, int custom)
{
    const uint16_t c_bg=0x0000, c_red=0xFA2A, c_dim=0x7431, c_track=0x1925, c_white=0xEF9E;
    if (custom) {
        const uint16_t *img=(const uint16_t *)BOOT_IMG_ADDR;
        for (int i=0;i<W*H;i++) fb[i]=img[i];
        lcd_fill(6,28,124,8,c_bg);
        lcd_fill(132,19,22,9,c_bg);
        lcd_fill(8,30,120,4,c_track);
        int fw=120*pct/100; if(fw) lcd_fill(8,30,fw,4,c_red);
        char pb[5]; int n=0;
        if(pct>=100){pb[0]='1';pb[1]='0';pb[2]='0';pb[3]='%';n=4;}
        else {pb[0]=(char)('0'+pct/10);pb[1]=(char)('0'+pct%10);pb[2]='%';n=3;}
        pb[n]=0; lcd_text(153-(6*n-1),21,pb,c_white);
        return;
    }
    lcd_clear(c_bg);
    (void)phase;
    /* diagonal Apex beam */
    if (scan_y >= 0) {
        for (int y=0;y<H;y++) {
            int x = scan_y + 18 - y*36/40;
            if (x >= 1 && x < W-2) lcd_fill(x, y, 2, 1, flash?c_white:c_red);
            if (x-3 >= 1) lcd_px(x-3, y, 0x78E5);
        }
    }
    /* wordmark, revealed by the beam */
    const char *wm="PD210";
    for (int i=0;i<5;i++) {
        char ch[2]={wm[i],0};
        int lit = (scan_y < 0) || (scan_y >= 51+i*12+4);
        lcd_text_big(51+i*12, 9, ch, lit ? c_white : c_dim, 2);
    }
    lcd_fill(44,9,2,14,c_red);
    lcd_text(39,25,"APEX INTERFACE", pct>=100?c_white:c_dim);
    /* footer: BOOT [bar] 100% */
    lcd_text(4,33,"BOOT",c_dim);
    lcd_fill(33,35,94,2,c_track);
    int fw=94*pct/100;
    if (fw) lcd_fill(33,35,fw,2,c_red);
    for (int k=1;k<10;k++) lcd_fill(33+94*k/10,35,1,2,c_bg);
    if (fw>0) lcd_fill(33+fw-1,34,2,4,c_red);
    char pb[5]; int n=0;
    if(pct>=100){pb[0]='1';pb[1]='0';pb[2]='0';pb[3]='%';n=4;}
    else {pb[0]=(char)('0'+pct/10);pb[1]=(char)('0'+pct%10);pb[2]='%';n=3;}
    pb[n]=0; lcd_text(156-(6*n-1),33,pb,pct>=100?c_white:c_dim);
}

/* ★ v227: 开机动画 —— 扫描线 + 分段进度 + 完成闪烁，再滑入主界面 */
void lcd_boot_reveal(int frames)
{
    if (frames < 1) frames = 1;
    for (int i = 0; i < W * H; i++) fb_prev[i] = fb[i];   /* prev = 主界面 */
    int custom = boot_custom_valid();

    boot_frame(0, 0, 0, 0, custom);
    lcd_flush();                                          /* 背光未开，先铺第一帧 */
    lcd_backlight_on();
    boot_wait(2);
    for (int k = 1; k < 10; k++) {
        boot_frame(0, k * W / 10, k, 0, custom);          /* APEX 斜向光束 */
        lcd_flush();
        boot_wait(5);                                     /* v228: 放慢到看得清 */
    }
    for (int k = 0; k <= 15; k++) {
        int pct = k * 100 / 15;
        boot_frame(pct, -1, k, 0, custom);
        lcd_flush();
        boot_wait(5);
    }
    for (int k = 0; k < 4; k++) {
        boot_frame(100, -1, k, k & 1, custom);             /* 完成闪烁 */
        lcd_flush();
        boot_wait(8);
    }
    boot_frame(100, -1, 0, 0, custom);                     /* 稳定停留 READY */
    lcd_flush();
    boot_wait(50);                                         /* 约 300ms */

    for (int i = 0; i < W * H; i++) {                      /* 交换：prev=启动画面, fb=主界面 */
        uint16_t m = fb_prev[i], sp = fb[i];
        fb_prev[i] = sp; fb[i] = m;
    }
    for (int k = 0; k <= frames; k++) {
        uint32_t ft = millis();
        uint32_t p = (uint32_t)k * 1000u / (uint32_t)frames;
        int e = (int)(3u * p * p / 1000u - 2u * p * p * p / 1000000u);   /* smoothstep */
        lcd_wipe_flush(1, e);
        while (millis() - ft < 6u) { }                     /* v228: 滑入也放慢 */
    }
}

void lcd_flush(void)
{
    /* 恒等映射 + Y 轴翻转修正 (实测玻璃安装方向与 CASET 扫描相反):
     * die(RASET=x, CASET=y) = UI(x, 39-y); rot180 在 UI 空间内再转 180° */
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H; y++) {
            int ux = lcd_rot180 ? (W - 1 - x) : x;
            int uy = lcd_rot180 ? y : (H - 1 - y);
            uint16_t v = fb[uy * W + ux];
            int o = (x * H + y) * 2;
            ram_b[o]     = (uint8_t)(v >> 8);
            ram_b[o + 1] = (uint8_t)(v & 0xFF);
        }
    }
    lcd_send_full();
}

/* v291: 局部刷新 —— 只发 UI 矩形对应的 die 窗口，供选中块拉伸动效使用。
 * 映射与 lcd_flush 完全一致: die row=UI x (rot180 镜像), die col=UI y 的上下翻转。 */
void lcd_flush_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    int x1 = x + w - 1, y1 = y + h - 1;
    int dr0, dr1, dc0, dc1;
    if (!lcd_rot180) {
        dr0 = x; dr1 = x1;
        dc0 = H - 1 - y1; dc1 = H - 1 - y;
    } else {
        dr0 = W - 1 - x1; dr1 = W - 1 - x;
        dc0 = y; dc1 = y1;
    }

    uint8_t cas[5] = { 0x2A, 0, (uint8_t)dc0, 0, (uint8_t)dc1 };
    uint8_t ras[5] = { 0x2B, 0, (uint8_t)dr0, 0, (uint8_t)dr1 };
    uint8_t ramc = 0x2C;
    LCD_CS_LOW();
    LCD_DC_LOW(); spi2_write(cas, 1); LCD_DC_HIGH(); spi2_write(cas + 1, 4);
    LCD_DC_LOW(); spi2_write(ras, 1); LCD_DC_HIGH(); spi2_write(ras + 1, 4);
    LCD_DC_LOW(); spi2_write(&ramc, 1); LCD_DC_HIGH();

    int o = 0;
    for (int dr = dr0; dr <= dr1; dr++) {
        int ux = lcd_rot180 ? (W - 1 - dr) : dr;
        for (int dc = dc0; dc <= dc1; dc++) {
            int uy = lcd_rot180 ? dc : (H - 1 - dc);
            uint16_t v = fb[uy * W + ux];
            ram_b[o++] = (uint8_t)(v >> 8);
            ram_b[o++] = (uint8_t)(v & 0xFF);
        }
    }
    spi2_write(ram_b, o);
    LCD_CS_HIGH();
}

/* 翻页滑动：dir>0 旧页左移、新页从右入；dir<0 反向。pm=0..1000 为缓动进度 */
void lcd_slide_flush(int dir, int pm)
{
    if (pm >= 1000) { lcd_flush(); return; }
    int s = W * pm / 1000;
    for (int x = 0; x < W; x++) {
        int ux = lcd_rot180 ? (W - 1 - x) : x;
        int sx = (dir > 0) ? ux + s : ux - s;
        const uint16_t *src = fb_prev;
        if (sx < 0)        { sx += W; src = fb; }
        else if (sx >= W)  { sx -= W; src = fb; }
        for (int y = 0; y < H; y++) {
            int uy = lcd_rot180 ? y : (H - 1 - y);
            uint16_t v = src[uy * W + sx];
            int o = (x * H + y) * 2;
            ram_b[o]     = (uint8_t)(v >> 8);
            ram_b[o + 1] = (uint8_t)(v & 0xFF);
        }
    }
    lcd_send_full();
}

/* APEX 斜切擦除：新页从 dir 方向推入，红色斜边在前 */
void lcd_wipe_flush(int dir, int pm)
{
    if (pm >= 1000) { lcd_flush(); return; }
    int b = (dir > 0) ? (W - W * pm / 1000) : (W * pm / 1000);
    for (int x = 0; x < W; x++) {
        int ux = lcd_rot180 ? (W - 1 - x) : x;
        for (int y = 0; y < H; y++) {
            int uy = lcd_rot180 ? y : (H - 1 - y);
            int edge = b + (uy - 20) * 16 / 40;
            int takeNew = (dir > 0) ? (ux >= edge) : (ux <= edge);
            uint16_t v = takeNew ? fb[uy * W + ux] : fb_prev[uy * W + ux];
            int d = ux - edge; if (d < 0) d = -d;
            if (d <= 1) v = 0xFA2A;
            int o = (x * H + y) * 2;
            ram_b[o] = (uint8_t)(v >> 8);
            ram_b[o + 1] = (uint8_t)(v & 0xFF);
        }
    }
    lcd_send_full();
}

/* v322: 增量斜切擦除 —— 只发送相对上一帧新露出的斜向条带，避免整屏 12.8KB 传输 */
void lcd_wipe_band(int dir, int pm_prev, int pm_cur)
{
    if (pm_cur <= pm_prev) return;
    if (pm_cur > 1000) pm_cur = 1000;
    if (pm_prev < 0) pm_prev = 0;
    int b0 = (dir > 0) ? (W - W * pm_prev / 1000) : (W * pm_prev / 1000);
    int b1 = (dir > 0) ? (W - W * pm_cur / 1000) : (W * pm_cur / 1000);
    for (int uy = 0; uy < H; uy++) {
        int e0 = b0 + (uy - 20) * 16 / 40;
        int e1 = b1 + (uy - 20) * 16 / 40;
        int x0, x1;
        if (dir > 0) { x0 = e1; x1 = e0 - 1; }
        else         { x0 = e0 + 1; x1 = e1; }
        if (x0 < 0) x0 = 0;
        if (x1 >= W) x1 = W - 1;
        if (x1 >= x0) lcd_flush_rect(x0, uy, x1 - x0 + 1, 1);

        /* v323: 在当前斜边补 2px 红色光束，遮住新旧页交界；临时改 fb 后恢复 */
        int be = e1;
        if (be >= 0 && be < W) {
            int bw = (be + 1 < W) ? 2 : 1;
            uint16_t s0 = fb[uy * W + be];
            uint16_t s1 = (bw > 1) ? fb[uy * W + be + 1] : 0;
            fb[uy * W + be] = 0xFA2A;
            if (bw > 1) fb[uy * W + be + 1] = 0xFA2A;
            lcd_flush_rect(be, uy, bw, 1);
            fb[uy * W + be] = s0;
            if (bw > 1) fb[uy * W + be + 1] = s1;
        }
    }
}

/* v326: 增量斜切 —— 每帧只发一条竖直条带，条带内按斜边合成新旧页。
 * 相比逐行窗口，每帧只有一个 RAMWR 窗口，避免局部刷新撕裂/花屏。 */
void lcd_wipe_strip(int dir, int pm_prev, int pm_cur)
{
    if (pm_cur <= pm_prev) return;
    if (pm_cur > 1000) pm_cur = 1000;
    if (pm_prev < 0) pm_prev = 0;
    int b0 = (dir > 0) ? (W - W * pm_prev / 1000) : (W * pm_prev / 1000);
    int b1 = (dir > 0) ? (W - W * pm_cur / 1000) : (W * pm_cur / 1000);
    int x0, x1;
    /* 发送当前斜边到屏幕边缘的整个新侧矩形，内部逐像素合成旧/新页；
     * 避免逐行窗口造成撕裂，也比整屏少传一部分。 */
    if (dir > 0) { x0 = b1 - 8; x1 = W - 1; }
    else         { x0 = 0;      x1 = b1 + 7; }
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (x1 < x0) return;

    int y0 = 0, y1 = H - 1;
    int dr0, dr1, dc0, dc1;
    if (!lcd_rot180) { dr0 = x0; dr1 = x1; dc0 = H - 1 - y1; dc1 = H - 1 - y0; }
    else             { dr0 = W - 1 - x1; dr1 = W - 1 - x0; dc0 = y0; dc1 = y1; }

    uint8_t cas[5] = { 0x2A, 0, (uint8_t)dc0, 0, (uint8_t)dc1 };
    uint8_t ras[5] = { 0x2B, 0, (uint8_t)dr0, 0, (uint8_t)dr1 };
    uint8_t ramc = 0x2C;
    LCD_CS_LOW();
    LCD_DC_LOW(); spi2_write(cas, 1); LCD_DC_HIGH(); spi2_write(cas + 1, 4);
    LCD_DC_LOW(); spi2_write(ras, 1); LCD_DC_HIGH(); spi2_write(ras + 1, 4);
    LCD_DC_LOW(); spi2_write(&ramc, 1); LCD_DC_HIGH();

    int o = 0;
    for (int dr = dr0; dr <= dr1; dr++) {
        int ux = lcd_rot180 ? (W - 1 - dr) : dr;
        int edge = b1 + (0 - 20) * 16 / 40;
        for (int dc = dc0; dc <= dc1; dc++) {
            int uy = lcd_rot180 ? dc : (H - 1 - dc);
            edge = b1 + (uy - 20) * 16 / 40;
            int takeNew = (dir > 0) ? (ux >= edge) : (ux <= edge);
            uint16_t v = takeNew ? fb[uy * W + ux] : fb_prev[uy * W + ux];
            int d = ux - edge; if (d < 0) d = -d;
            if (d <= 1) v = 0xFA2A;
            ram_b[o++] = (uint8_t)(v >> 8);
            ram_b[o++] = (uint8_t)(v & 0xFF);
        }
    }
    spi2_write(ram_b, o);
    LCD_CS_HIGH();
}

static int clip_x0 = 0, clip_y0 = 0, clip_x1 = W - 1, clip_y1 = H - 1;

void lcd_set_clip(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}

void lcd_px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    if (x < clip_x0 || x > clip_x1 || y < clip_y0 || y > clip_y1) return;
    fb[y * W + x] = c;
}

/* 5-bit alpha (0..31) 混合，用于 14px 抗锯齿中文字模 */
void lcd_blend_px(int x, int y, uint16_t c, uint8_t a)
{
    if (!a) return;
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    if (x < clip_x0 || x > clip_x1 || y < clip_y0 || y > clip_y1) return;
    uint16_t bg = fb[y * W + x];
    if (a >= 31) { fb[y * W + x] = c; return; }
    uint16_t inv = (uint16_t)(32 - a);
    uint16_t r = (uint16_t)((((c >> 11) & 31) * a + ((bg >> 11) & 31) * inv) >> 5);
    uint16_t g = (uint16_t)((((c >> 5) & 63) * a + ((bg >> 5) & 63) * inv) >> 5);
    uint16_t b = (uint16_t)(((c & 31) * a + (bg & 31) * inv) >> 5);
    fb[y * W + x] = (uint16_t)((r << 11) | (g << 5) | b);
}

void lcd_fill(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            lcd_px(xx, yy, c);
}

/* ---- 5×7 字体 (ASCII 0x20-0x5A, 标准 Adafruit GLCD 表, 59 条全) ---- */
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},                                 /* @ */
    {0x7E,0x11,0x11,0x11,0x7E},                                 /* A */
    {0x7F,0x49,0x49,0x49,0x36},                                 /* B */
    {0x3E,0x41,0x41,0x41,0x22},                                 /* C */
    {0x7F,0x41,0x41,0x22,0x1C},                                 /* D */
    {0x7F,0x49,0x49,0x49,0x41},                                 /* E */
    {0x7F,0x09,0x09,0x01,0x01},                                 /* F */
    {0x3E,0x41,0x49,0x49,0x7A},                                 /* G */
    {0x7F,0x08,0x08,0x08,0x7F},                                 /* H */
    {0x00,0x41,0x7F,0x41,0x00},                                 /* I */
    {0x20,0x40,0x41,0x3F,0x01},                                 /* J */
    {0x7F,0x08,0x14,0x22,0x41},                                 /* K */
    {0x7F,0x40,0x40,0x40,0x40},                                 /* L */
    {0x7F,0x02,0x04,0x02,0x7F},                                 /* M */
    {0x7F,0x04,0x08,0x10,0x7F},                                 /* N */
    {0x3E,0x41,0x41,0x41,0x3E},                                 /* O */
    {0x7F,0x09,0x09,0x09,0x06},                                 /* P */
    {0x3E,0x41,0x51,0x21,0x5E},                                 /* Q */
    {0x7F,0x09,0x19,0x29,0x46},                                 /* R */
    {0x46,0x49,0x49,0x49,0x31},                                 /* S */
    {0x01,0x01,0x7F,0x01,0x01},                                 /* T */
    {0x3F,0x40,0x40,0x40,0x3F},                                 /* U */
    {0x1F,0x20,0x40,0x20,0x1F},                                 /* V */
    {0x7F,0x20,0x18,0x20,0x7F},                                 /* W */
    {0x63,0x14,0x08,0x14,0x63},                                 /* X */
    {0x03,0x04,0x78,0x04,0x03},                                 /* Y */
    {0x61,0x51,0x49,0x45,0x43},                                 /* Z */
};

/* 小写只补界面实际用到的 s/m/z；s/m 做成满高以与数字对齐，z 保持 x-height 供 Zzz */
static const uint8_t FONT_LC[3][5] = {
    {0x46, 0x49, 0x49, 0x49, 0x31},   /* s */
    {0x7F, 0x02, 0x7F, 0x02, 0x7F},   /* m */
    {0x44, 0x64, 0x54, 0x4C, 0x00},   /* z */
};

void lcd_char(int x, int y, char c, uint16_t fg)
{
    const uint8_t *g;
    int yo = 0;
    if (c == 's' || c == 'm') {
        g = FONT_LC[c == 's' ? 0 : 1];
    } else if (c == 'z') {
        g = FONT_LC[2];
        yo = 2;
    } else {
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x20 || c > 0x5A) c = 0x20;
        g = font5x7[c - 0x20];
    }
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row)) lcd_px(x + col, y + yo + row, fg);
    }
}

void lcd_text(int x, int y, const char *s, uint16_t fg)
{
    while (*s) { lcd_char(x, y, *s, fg); x += 6; s++; }
}

void lcd_char_big(int x, int y, char c, uint16_t fg, int sc)
{
    const uint8_t *g;
    int yo = 0;
    if (c == 's' || c == 'm') {
        g = FONT_LC[c == 's' ? 0 : 1];
    } else if (c == 'z') {
        g = FONT_LC[2];
        yo = 2;
    } else {
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x20 || c > 0x5A) c = 0x20;
        g = font5x7[c - 0x20];
    }
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row))
                for (int dy = 0; dy < sc; dy++)
                    for (int dx = 0; dx < sc; dx++)
                        lcd_px(x + col * sc + dx, y + (yo + row) * sc + dy, fg);
    }
}

void lcd_text_big(int x, int y, const char *s, uint16_t fg, int sc)
{
    while (*s) { lcd_char_big(x, y, *s, fg, sc); x += 6 * sc; s++; }
}

/* ---- 7 段码数码管数字 (设计基线主界面同款观感) ----
 * 段位: bit0=A顶 1=B右上 2=C右下 3=D底 4=E左下 5=F左上 6=G中 */
static const uint8_t seg7tab[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void lcd_seg7_char(int x0, int y, uint8_t seg, uint16_t fg, int dw, int dh, int t)
{
    int h2 = (dh - 3 * t) / 2;
    if (seg & 0x01) lcd_fill(x0 + t, y, dw - 2 * t, t, fg);                    /* A */
    if (seg & 0x02) lcd_fill(x0 + dw - t, y + t, t, h2, fg);                   /* B */
    if (seg & 0x04) lcd_fill(x0 + dw - t, y + dh / 2 + t / 2, t, h2, fg);      /* C */
    if (seg & 0x08) lcd_fill(x0 + t, y + dh - t, dw - 2 * t, t, fg);           /* D */
    if (seg & 0x10) lcd_fill(x0, y + dh / 2 + t / 2, t, h2, fg);               /* E */
    if (seg & 0x20) lcd_fill(x0, y + t, t, h2, fg);                            /* F */
    if (seg & 0x40) lcd_fill(x0 + t, y + dh / 2 - t / 2, dw - 2 * t, t, fg);   /* G */
}

void lcd_seg7_digit(int x, int y, int v, uint16_t fg, int dw, int dh, int t)
{
    if (v < 0 || v > 9) return;
    lcd_seg7_char(x, y, seg7tab[v], fg, dw, dh, t);
}

void lcd_seg7(int x, int y, int value, uint16_t fg, int dw, int dh, int t)
{
    int neg = value < 0;
    if (neg) value = -value;
    if (value > 999) value = 999;
    int h2 = (dh - 3 * t) / 2;
    for (int d = 0; d < 3; d++) {                 /* 3 位, 前导零消隐 */
        int v = (d == 0) ? value / 100 : (d == 1) ? (value / 10) % 10 : value % 10;
        int x0 = x + d * (dw + 2);
        if (d == 0 && neg) {                      /* 负号 */
            lcd_fill(x0, y + dh / 2 - t / 2, t + 2, t, fg);
            continue;
        }
        if (d < 2 && value < (d == 0 ? 100 : 10)) { x0 += 0; continue; }  /* 消隐 */
        uint8_t seg = seg7tab[v];
        lcd_seg7_char(x0, y, seg, fg, dw, dh, t);
    }
}

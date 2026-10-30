/* ============================================================================
 * flash.c — 设置持久化 (STM32L431 末页 0x0803F800, page 127)
 * PNB 在 CR bits[11:3] (RM0351 家族约定); 擦除后读回自校验, 编码错即放弃,
 * 绝不尝试其他编码 (防止误擦代码页)。
 * ==========================================================================*/
#include "regs.h"
#include "flash.h"

#define FKEYR 0x40022008u
#define FSR   0x40022010u
#define FCR   0x40022014u
#define FPG    0x00000001u
#define FPER   0x00000002u
#define FSTRT  0x00010000u
#define FBSY   0x00010000u
#define FLOCK  0x80000000u
#define F_PNB_POS 3

#define SETTINGS_ADDR 0x0803F800u
#define SETTINGS_PAGE 127
#define SETTINGS_MAGIC 0x50443234u   /* "PD24" */

static void flash_wait(void) { while (REG32(FSR) & FBSY) ; }

uint8_t flash_settings_load(settings_t *s)
{
    const settings_t *p = (const settings_t *)SETTINGS_ADDR;
    if (p->magic != SETTINGS_MAGIC) return 0;
    uint32_t sum = p->magic ^ (uint32_t)p->target_c ^ (uint32_t)p->limit_pct
                 ^ (uint32_t)p->pdo_gear;
    if (sum != p->checksum) return 0;
    if (p->target_c < 100 || p->target_c > 450) return 0;
    if (p->limit_pct < 1 || p->limit_pct > 100) return 0;
    if (p->pdo_gear > 5) return 0;
    *s = *p;
    return 1;
}

uint8_t flash_settings_write(const settings_t *s)
{
    /* ★ 擦写期间必须关中断: SysTick 打断 64 位写序列 = HardFault + 全局区污染
     * (v17 实测崩溃根因)。代价: millis() 每次保存丢失约 22ms, 可接受 */
    flash_wait();
    REG32(FKEYR) = 0x45670123u;
    REG32(FKEYR) = 0xCDEF89ABu;
    REG32(FSR) = 0x00C3FBFFu;                 /* 清历史错误位 */

    __asm volatile ("cpsid i" ::: "memory");

    /* --- 擦页 + 自校验 --- */
    uint32_t cr = REG32(FCR) & ~((0x1FFu << F_PNB_POS) | FPG);
    cr |= FPER | ((uint32_t)SETTINGS_PAGE << F_PNB_POS);
    REG32(FCR) = cr;
    REG32(FCR) = cr | FSTRT;
    flash_wait();
    REG32(FCR) = cr & ~FPER;
    {
        volatile uint8_t *p = (volatile uint8_t *)SETTINGS_ADDR;
        uint8_t ok = 1;
        for (int i = 0; i < 32; i++)
            if (p[i] != 0xFF) { ok = 0; break; }  /* 编码错, 放弃 */
        if (!ok) {
            __asm volatile ("cpsie i" ::: "memory");
            REG32(FCR) |= FLOCK;
            return 0;
        }
    }

    /* --- 64bit 双字写 x10 (HAL 同款: uint64 赋值) --- */
    uint32_t crw = (REG32(FCR) & ~(0x1FFu << F_PNB_POS)) | FPG;
    REG32(FCR) = crw;
    volatile uint64_t *dst = (volatile uint64_t *)SETTINGS_ADDR;
    const uint64_t *src = (const uint64_t *)s;
    for (int i = 0; i < 10; i++) {   /* ★ v251: 80 字节 (10×64bit) */
        dst[i] = src[i];
        flash_wait();
    }
    REG32(FCR) = crw & ~FPG;
    REG32(FCR) |= FLOCK;

    __asm volatile ("cpsie i" ::: "memory");

    /* --- 读回校验 (80 字节) --- */
    const uint32_t *w = (const uint32_t *)SETTINGS_ADDR;
    const uint32_t *x = (const uint32_t *)s;
    for (int i = 0; i < 20; i++) if (w[i] != x[i]) return 0;   /* ★ v251: 20 字 */
    return 1;
}

/* ============================================================================
 * ★ v335: 黑匣子 — 元数据页 120 + 数据页 121..126
 *   page120: fly_meta_t (56B) = magic/version/cnt/CRC64/run_seq/fw/UID/设置
 *   page121..122: [0..7]=magic+cnt, 之后 T[0..2043]
 *   page123..124: D[0..2047]
 *   page125..126: R[0..2047]
 *   设置页 127 不动；清空擦 120..126。
 * ==========================================================================*/
#define FLY_MAGIC  0x46314C59u    /* "FLY1" */
#define FLY_MAX    2044
#define FLY_RING   2048
#define FLY_META_ADDR 0x0803C000u
#define FLY_META_PAGE 120u

_Static_assert(sizeof(fly_meta_t) == 56, "fly_meta_t layout");

static uint32_t fly_meta_checksum(const fly_meta_t *m)
{
    const uint32_t *w = (const uint32_t *)m;
    uint32_t s = 0xA5A5A5A5u;
    for (int i = 0; i < 13; i++)
        s = ((s << 1) | (s >> 31)) ^ w[i];
    return s;
}

static uint64_t crc64_byte(uint64_t crc, uint8_t b)
{
    crc ^= (uint64_t)b << 56;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000000000000000ull)
            ? (crc << 1) ^ 0x42F0E1EBA9EA3693ull
            : (crc << 1);
    return crc;
}

static uint64_t crc64_u16(uint64_t crc, uint16_t v)
{
    crc = crc64_byte(crc, (uint8_t)v);
    return crc64_byte(crc, (uint8_t)(v >> 8));
}

static uint64_t crc64_u32(uint64_t crc, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        crc = crc64_byte(crc, (uint8_t)(v >> (8 * i)));
    return crc;
}

static uint64_t fly_crc64(uint16_t cnt,
                          const volatile uint16_t *t,
                          const volatile uint16_t *d,
                          const volatile uint16_t *r)
{
    uint64_t c = crc64_u32(0, (uint32_t)cnt);
    uint16_t tn = (cnt > FLY_MAX) ? FLY_MAX : cnt;
    uint16_t dn = (cnt > FLY_RING) ? FLY_RING : cnt;
    for (uint16_t i = 0; i < tn; i++) c = crc64_u16(c, t[i]);
    for (uint16_t i = 0; i < dn; i++) c = crc64_u16(c, d[i]);
    for (uint16_t i = 0; i < dn; i++) c = crc64_u16(c, r[i]);
    return c;
}

uint8_t flash_meta_load(fly_meta_t *meta)
{
    const fly_meta_t *p = (const fly_meta_t *)FLY_META_ADDR;
    if (p->magic != FLY_META_MAGIC || p->version != FLY_META_VERSION) return 0;
    if (p->checksum != fly_meta_checksum(p)) return 0;
    const volatile uint32_t *hdr = (const volatile uint32_t *)0x0803C800u;
    if (hdr[0] != FLY_MAGIC || (uint16_t)hdr[1] != p->cnt) return 0;
    *meta = *p;
    return 1;
}

uint8_t flash_fly_save(uint16_t cnt,
                       const volatile uint16_t *t,
                       const volatile uint16_t *d,
                       const volatile uint16_t *r,
                       fly_meta_t *meta)
{
    static const uint32_t PAGE[7] = { 120u, 121u, 122u, 123u, 124u, 125u, 126u };
    static const uint32_t ADDR[7] = { 0x0803C000u, 0x0803C800u, 0x0803D000u, 0x0803D800u,
                                      0x0803E000u, 0x0803E800u, 0x0803F000u };
    static const uint32_t DATA_ADDR[3] = { 0x0803C800u, 0x0803D800u, 0x0803E800u };
    const volatile uint16_t *SRC[3] = { t, d, r };

    if (!meta) return 0;

    uint32_t run_seq = 1;
    fly_meta_t prev;
    if (flash_meta_load(&prev)) {
        run_seq = prev.run_seq + 1u;
        if (run_seq == 0u) run_seq = 1u;
    }
    meta->magic = FLY_META_MAGIC;
    meta->version = FLY_META_VERSION;
    meta->cnt = cnt;
    meta->crc64 = fly_crc64(cnt, t, d, r);
    meta->run_seq = run_seq;
    meta->checksum = fly_meta_checksum(meta);

    flash_wait();
    REG32(FKEYR) = 0x45670123u;
    REG32(FKEYR) = 0xCDEF89ABu;
    REG32(FSR) = 0x00C3FBFFu;
    __asm volatile ("cpsid i" ::: "memory");

    /* --- 擦除页 120..126 --- */
    for (int k = 0; k < 7; k++) {
        uint32_t cr = REG32(FCR) & ~((0x1FFu << F_PNB_POS) | FPG);
        cr |= FPER | (PAGE[k] << F_PNB_POS);
        REG32(FCR) = cr;
        REG32(FCR) = cr | FSTRT;
        flash_wait();
        REG32(FCR) = cr & ~FPER;
        volatile uint8_t *p = (volatile uint8_t *)ADDR[k];
        for (int i = 0; i < 8; i++)
            if (p[i] != 0xFF) {
                REG32(FCR) |= FLOCK;
                __asm volatile ("cpsie i" ::: "memory");
                return 0;
            }
    }

    /* --- 数据页：T=page121, D=page123, R=page125 --- */
    for (int k = 0; k < 3; k++) {
        uint32_t crw = (REG32(FCR) & ~(0x1FFu << F_PNB_POS)) | FPG;
        REG32(FCR) = crw;
        uint32_t base = DATA_ADDR[k];
        int n = (k == 0) ? ((cnt > FLY_MAX) ? FLY_MAX : cnt)
                         : ((cnt > FLY_RING) ? FLY_RING : cnt);
        uint32_t off = 0;

        if (k == 0) {
            volatile uint64_t *h = (volatile uint64_t *)base;
            *h = (uint64_t)FLY_MAGIC | ((uint64_t)cnt << 32);
            flash_wait();
            off = 8;
        }

        volatile uint64_t *dst = (volatile uint64_t *)(base + off);
        uint32_t start = (cnt >= FLY_RING) ? (cnt & (FLY_RING - 1)) : 0u;
        for (int i = 0; i + 3 < n; i += 4) {
            uint16_t i0 = (uint16_t)((start + i) & (FLY_RING - 1));
            uint16_t i1 = (uint16_t)((start + i + 1) & (FLY_RING - 1));
            uint16_t i2 = (uint16_t)((start + i + 2) & (FLY_RING - 1));
            uint16_t i3 = (uint16_t)((start + i + 3) & (FLY_RING - 1));
            uint64_t v = (uint64_t)SRC[k][i0]
                       | ((uint64_t)SRC[k][i1] << 16)
                       | ((uint64_t)SRC[k][i2] << 32)
                       | ((uint64_t)SRC[k][i3] << 48);
            dst[i >> 2] = v;
            flash_wait();
        }
        REG32(FCR) = crw & ~FPG;
    }

    /* --- 元数据页 120 --- */
    {
        uint32_t crw = (REG32(FCR) & ~(0x1FFu << F_PNB_POS)) | FPG;
        REG32(FCR) = crw;
        volatile uint64_t *dst = (volatile uint64_t *)FLY_META_ADDR;
        const uint64_t *src = (const uint64_t *)meta;
        for (int i = 0; i < 7; i++) {
            dst[i] = src[i];
            flash_wait();
        }
        REG32(FCR) = crw & ~FPG;
    }

    REG32(FCR) |= FLOCK;
    __asm volatile ("cpsie i" ::: "memory");
    return 1;
}

/* ★ v335: 清除黑匣子 — 擦页 120..126，不动设置页 127 */
uint8_t flash_fly_clear(void)
{
    static const uint32_t PAGE[7] = { 120u, 121u, 122u, 123u, 124u, 125u, 126u };
    static const uint32_t ADDR[7] = { 0x0803C000u, 0x0803C800u, 0x0803D000u, 0x0803D800u,
                                      0x0803E000u, 0x0803E800u, 0x0803F000u };

    flash_wait();
    REG32(FKEYR) = 0x45670123u;
    REG32(FKEYR) = 0xCDEF89ABu;
    REG32(FSR) = 0x00C3FBFFu;
    __asm volatile ("cpsid i" ::: "memory");

    for (int k = 0; k < 7; k++) {
        uint32_t cr = REG32(FCR) & ~((0x1FFu << F_PNB_POS) | FPG);
        cr |= FPER | (PAGE[k] << F_PNB_POS);
        REG32(FCR) = cr;
        REG32(FCR) = cr | FSTRT;
        flash_wait();
        REG32(FCR) = cr & ~FPER;
        volatile uint8_t *p = (volatile uint8_t *)ADDR[k];
        for (int i = 0; i < 8; i++)
            if (p[i] != 0xFF) {
                REG32(FCR) |= FLOCK;
                __asm volatile ("cpsie i" ::: "memory");
                return 0;
            }
    }

    REG32(FCR) |= FLOCK;
    __asm volatile ("cpsie i" ::: "memory");
    return 1;
}

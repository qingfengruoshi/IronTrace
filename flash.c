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
 * ★ v126: 黑匣子落盘 — 掉电后仍可读（PWLink 插拔会断电，SRAM2 数据丢失）
 * 页 124/125/126 @ 0x0803E000/0x0803E800/0x0803F000 (各 2KB)
 *   page124: [0..3]=magic [4..5]=cnt [8..] = T[0..1019]
 *   page125: D[0..1023]
 *   page126: R[0..1023]
 * ==========================================================================*/
#define FLY_MAGIC  0x46314C59u    /* "FLY1" */
#define FLY_MAX    1020

uint8_t flash_fly_save(uint16_t cnt,
                       const volatile uint16_t *t,
                       const volatile uint16_t *d,
                       const volatile uint16_t *r)
{
    static const uint32_t PAGE[3] = { 124u, 125u, 126u };
    static const uint32_t ADDR[3] = { 0x0803E000u, 0x0803E800u, 0x0803F000u };
    const volatile uint16_t *SRC[3] = { t, d, r };

    flash_wait();
    REG32(FKEYR) = 0x45670123u;
    REG32(FKEYR) = 0xCDEF89ABu;
    REG32(FSR) = 0x00C3FBFFu;
    __asm volatile ("cpsid i" ::: "memory");

    /* --- 擦除 3 页 --- */
    for (int k = 0; k < 3; k++) {
        uint32_t cr = REG32(FCR) & ~((0x1FFu << F_PNB_POS) | FPG);
        cr |= FPER | (PAGE[k] << F_PNB_POS);
        REG32(FCR) = cr;
        REG32(FCR) = cr | FSTRT;
        flash_wait();
        REG32(FCR) = cr & ~FPER;
        volatile uint8_t *p = (volatile uint8_t *)ADDR[k];
        for (int i = 0; i < 8; i++)
            if (p[i] != 0xFF) {                       /* 擦除失败保护 */
                REG32(FCR) |= FLOCK;
                __asm volatile ("cpsie i" ::: "memory");
                return 0;
            }
    }

    /* --- 写入 --- */
    for (int k = 0; k < 3; k++) {
        uint32_t crw = (REG32(FCR) & ~(0x1FFu << F_PNB_POS)) | FPG;
        REG32(FCR) = crw;
        uint32_t base = ADDR[k];
        int n = (k == 0) ? FLY_MAX : 1024;
        uint32_t off = 0;

        if (k == 0) {
            /* 头: magic + cnt */
            volatile uint64_t *h = (volatile uint64_t *)base;
            *h = (uint64_t)FLY_MAGIC | ((uint64_t)cnt << 32);
            flash_wait();
            off = 8;
        }

        volatile uint64_t *dst = (volatile uint64_t *)(base + off);
        uint32_t start = (cnt >= 1024u) ? (cnt & 1023u) : 0u;   /* 未填满时从 0 起 */
        for (int i = 0; i + 3 < n; i += 4) {
            /* ★ v127: 按时间顺序重排（0 = 最旧） */
            uint16_t i0 = (uint16_t)((start + i) & 1023u);
            uint16_t i1 = (uint16_t)((start + i + 1) & 1023u);
            uint16_t i2 = (uint16_t)((start + i + 2) & 1023u);
            uint16_t i3 = (uint16_t)((start + i + 3) & 1023u);
            uint64_t v = (uint64_t)SRC[k][i0]
                       | ((uint64_t)SRC[k][i1] << 16)
                       | ((uint64_t)SRC[k][i2] << 32)
                       | ((uint64_t)SRC[k][i3] << 48);
            dst[i >> 2] = v;
            flash_wait();
        }
        REG32(FCR) = crw & ~FPG;
    }

    REG32(FCR) |= FLOCK;
    __asm volatile ("cpsie i" ::: "memory");
    return 1;
}

/* ★ v269: 清除黑匣子 — 只擦页 124/125/126，不动设置页 127 */
uint8_t flash_fly_clear(void)
{
    static const uint32_t PAGE[3] = { 124u, 125u, 126u };
    static const uint32_t ADDR[3] = { 0x0803E000u, 0x0803E800u, 0x0803F000u };

    flash_wait();
    REG32(FKEYR) = 0x45670123u;
    REG32(FKEYR) = 0xCDEF89ABu;
    REG32(FSR) = 0x00C3FBFFu;
    __asm volatile ("cpsid i" ::: "memory");

    for (int k = 0; k < 3; k++) {
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

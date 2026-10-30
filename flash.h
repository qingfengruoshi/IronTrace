/* ============================================================================
 * flash.h — 设置持久化接口
 * ==========================================================================*/
#ifndef FLASH_H
#define FLASH_H
#include <stdint.h>

typedef struct {
    uint32_t magic;      /* 0x50443234 "PD24" */
    int32_t  target_c;
    int32_t  limit_pct;
    int32_t  pdo_gear;
    uint32_t reserved;
    uint32_t checksum;   /* magic^target^limit^gear */
    /* ★ v211: 扩展字段 —— 全部 SET 菜单项 + 常用温度持久化（旧记录无 EXT1 标志→用默认值） */
    uint32_t ext_valid;  /* 0x45585431 "EXT1" */
    uint32_t misc_a;     /* bright(0..7) rot(8..9) sleepm(10..11) logi(12) bheat(13) banim(14) mth(16..23) */
    uint32_t misc_b;     /* idle(0..7) pause(8..15) sleepmin(16..23) */
    uint32_t preset01;   /* p0(0..15) p1(16..31) */
    uint32_t preset2;    /* p2(0..15) */
    uint32_t ext_chk;    /* ext_valid^misc_a^misc_b^preset01^preset2 */
    /* ★ v248: PID 参数（定点整数；pid_tag 高 16 位 = "PI"，低 16 位校验） */
    uint32_t pid_p100;   /* Kp × 100 */
    uint32_t pid_i1000;  /* Ki × 1000 */
    uint32_t pid_d100;   /* Kd × 100 */
    uint32_t pid_tag;
    /* ★ v251: COL 自定义调色板（六色打包为 3 字 + tag） */
    uint32_t col01;      /* ST(0..15) | LM(16..31) */
    uint32_t col23;      /* RS(0..15) | V(16..31) */
    uint32_t col45;      /* I(0..15) | P(16..31) */
    uint32_t col_tag;    /* 0x434F0000 | checksum */
} settings_t;

uint8_t flash_settings_load(settings_t *s);              /* 1=有效 */
uint8_t flash_settings_write(const settings_t *s);       /* 1=成功 (含自校验) */
/* ★ v126: 黑匣子落盘 — 3 页 124/125/126 (0x0803E000..)，6KB */
uint8_t flash_fly_save(uint16_t cnt,
                       const volatile uint16_t *t,
                       const volatile uint16_t *d,
                       const volatile uint16_t *r);
uint8_t flash_fly_clear(void);                            /* v269: 擦除 3 页黑匣子数据 */
#endif

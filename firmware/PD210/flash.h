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

/* ★ v335: 黑匣子元数据页 120 (0x0803C000..0x0803C7FF) */
#define FLY_META_MAGIC   0x4D455441u   /* "META" */
#define FLY_META_VERSION 1u
typedef struct {
    uint32_t magic;        /* 0  FLY_META_MAGIC */
    uint16_t version;      /* 4  FLY_META_VERSION */
    uint16_t cnt;          /* 6  log count */
    uint64_t crc64;        /* 8  CRC64/ECMA of LE32(cnt)+T+D+R */
    uint32_t run_seq;      /* 16 save sequence */
    char     fw_tag[4];    /* 20 "V335" / "V023" */
    uint8_t  uid[12];      /* 24 STM32 96-bit UID */
    int16_t  target_c;     /* 36 run target */
    uint8_t  limit_pct;    /* 38 run LM */
    uint8_t  pdo_gear;     /* 39 run PDO gear */
    uint16_t pid_p100;     /* 40 PID P x100 */
    uint16_t pid_i1000;    /* 42 PID I x1000 */
    uint16_t pid_d100;     /* 44 PID D x100 */
    uint16_t avg_vin_dv;   /* 46 reserved: average V x10 */
    uint16_t avg_iout_ma;  /* 48 reserved: average I mA */
    uint16_t avg_power_w10;/* 50 reserved: average P x10 */
    uint32_t checksum;     /* 52 rotating-XOR of words 0..12 */
} fly_meta_t;

/* ★ v335: 黑匣子落盘 — 元数据页 120 + 数据页 121..126 */
uint8_t flash_fly_save(uint16_t cnt,
                       const volatile uint16_t *t,
                       const volatile uint16_t *d,
                       const volatile uint16_t *r,
                       fly_meta_t *meta);
uint8_t flash_meta_load(fly_meta_t *meta);                /* 1=有效 (元数据+日志头匹配) */
uint8_t flash_fly_clear(void);                            /* v335: 擦除页 120..126 */
#endif

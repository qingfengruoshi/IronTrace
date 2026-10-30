/* ============================================================================
 * mini_lib.c — 裸机最小 C 库替代（无 stdlib 依赖）
 * ==========================================================================*/
#include <stdint.h>

void *memset(void *dst, int c, unsigned int n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int mini_snprintf(char *buf, unsigned int size, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    char *p = buf;
    char *end = buf + size - 1;
    for (const char *f = fmt; *f && p < end; f++) {
        if (*f != '%') { *p++ = *f; continue; }
        f++;
        /* 宽度/补零: %02X %3d 等 */
        int zeropad = 0, width = 0;
        if (*f == '0') { zeropad = 1; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        char tmp[12];
        int i = 0, neg = 0;
        if (*f == 'd') {
            int v = __builtin_va_arg(args, int);
            uint32_t u = (v < 0) ? (neg = 1, (uint32_t)(-(int64_t)v)) : (uint32_t)v;
            do { tmp[i++] = '0' + (u % 10); u /= 10; } while (u);
        } else if (*f == 'x' || *f == 'X') {
            uint32_t v = __builtin_va_arg(args, uint32_t);
            const char *hex = (*f == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            do { tmp[i++] = hex[v & 0xF]; v >>= 4; } while (v);
        } else if (*f == 's') {
            const char *s = __builtin_va_arg(args, const char *);
            while (*s && p < end) *p++ = *s++;
            continue;
        } else if (*f == '%') {
            if (p < end) *p++ = '%';
            continue;
        } else if (*f == 0) {
            break;
        } else {
            if (p < end) *p++ = *f;
            continue;
        }
        if (neg && p < end) *p++ = '-';
        {   /* ★ v21 修复: 补零/补空格次数 = width - 已有位数; 旧代码条件里 i 不变
             * 导致循环填满整个缓冲区 (LM/V 空白、I/P 溢出的总根因) */
            int npad = width - i;
            while (npad-- > 0 && p < end) *p++ = zeropad ? '0' : ' ';
        }
        while (i-- && p < end) *p++ = tmp[i];
    }
    *p = 0;
    __builtin_va_end(args);
    return (int)(p - buf);
}

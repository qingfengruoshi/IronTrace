/* ============================================================================
 * lcd.h — GC9D01 显示接口
 * ==========================================================================*/
#ifndef LCD_H
#define LCD_H
#include <stdint.h>
void lcd_init(void);
void lcd_clear(uint16_t c);
void lcd_flush(void);
void lcd_flush_rect(int x, int y, int w, int h);
void lcd_save_frame(void);
void lcd_slide_flush(int dir, int pm);
void lcd_wipe_flush(int dir, int pm);
void lcd_wipe_band(int dir, int pm_prev, int pm_cur);
void lcd_wipe_strip(int dir, int pm_prev, int pm_cur);
void lcd_boot_reveal(int frames);
void lcd_dim15(void);
void lcd_dim_pct(int pct);
void lcd_px(int x, int y, uint16_t c);
void lcd_blend_px(int x, int y, uint16_t c, uint8_t a);
void lcd_fill(int x, int y, int w, int h, uint16_t c);
void lcd_set_clip(int x0, int y0, int x1, int y1);
void lcd_seg7_digit(int x, int y, int v, uint16_t fg, int dw, int dh, int t);
void lcd_char(int x, int y, char c, uint16_t fg);
void lcd_text(int x, int y, const char *s, uint16_t fg);
void lcd_char_big(int x, int y, char c, uint16_t fg, int sc);
void lcd_text_big(int x, int y, const char *s, uint16_t fg, int sc);

void lcd_set_rot180(int r);

void lcd_set_orientation(int o);   /* 0-7: MADCTL 硬件方向组合 */
int lcd_get_orientation(void);

void lcd_backlight_on(void);

void lcd_seg7_char(int x0, int y, uint8_t seg, uint16_t fg, int dw, int dh, int t);
void lcd_seg7(int x, int y, int value, uint16_t fg, int dw, int dh, int t);

void lcd_set_geo(int g);
int lcd_get_geo(void);

#endif

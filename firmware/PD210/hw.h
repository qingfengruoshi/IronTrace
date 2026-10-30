/* ============================================================================
 * hw.h — 硬件接口声明
 * ==========================================================================*/
#ifndef HW_H
#define HW_H
#include <stdint.h>
#include "regs.h"

void clock_init(void);
void backlight_early_off(void);
void gpio_init(void);
void gpio_pin_otyper(GPIO_TypeDef *g, int pin, int od);
void adc_init(void);
uint16_t adc_temp(void);
uint16_t adc_iout(void);
uint16_t adc_vin(void);
void pwm_init(void);
void pwm_set(uint16_t duty);
int btn_left(void);
int btn_centre(void);
int btn_right(void);
int hall_active(void);
uint8_t husb238_read(uint8_t reg);
uint32_t millis(void);
void delay_ms(uint32_t ms);
void SysTick_Handler(void);
void spi2_init(void);
void spi2_write(const uint8_t *b, int n);

void husb238_write(uint8_t reg, uint8_t val);
uint8_t husb238_pdo_avail(uint8_t gear);      /* 0-5 = 5/9/12/15/18/20V 广播支持 */
uint16_t husb238_pdo_ma(uint8_t gear);        /* 该挡广播电流 mA */
void husb238_select_request(uint8_t gear);    /* 正确协议: 0x08 选挡 + 0x09 GO */
extern uint8_t husb_sel_rb;                   /* 应用后 0x08 回读 (诊断) */

int accel_init(void);
uint8_t accel_read_xyz(int8_t *gx, int8_t *gy, int8_t *gz);   /* v249: 0=I2C 读取失败 */
uint8_t accel_read_mg(int16_t *x, int16_t *y, int16_t *z);   /* v253: 12bit mg, 设计基线休眠判据 */

extern uint8_t accel_who;

extern uint8_t husb_ack;
void i2c_scan(void);

extern uint8_t accel_addr;
extern uint8_t i2c_ack[16];

#endif

void husb238_write_diag(uint8_t reg, uint8_t val);
uint8_t husb238_write_style(int style, uint8_t val);
extern uint8_t husb_diag_addr, husb_diag_reg, husb_diag_data;

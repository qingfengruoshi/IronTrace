/* ============================================================================
 * hw.c — 硬件初始化与底层操作
 *
 * 引脚分配（SCH_PD245焊笔V3_Sch_2024-08-30.pdf，逐引脚放大图核对 2026-10-06）：
 *   PA3  = HEAT      (TIM2_CH4 PWM, AF1; 原理图 HEAT=pin13=PA3，PA0/PA1/PA2 接 GND)
 *   PA5  = ADC_TEMP  (热电偶运放输出, ADC1_IN5)
 *   PA6  = ADC_IOUT  (INA199 电流采样, ADC1_IN6)
 *   PB1  = ADC_VIN   (VBUS 分压 200k:10k, ADC1_IN16)
 *   PA7  = LCD_CS    PB0 = LCD_RES  PB2 = LCD_DC   (GPIO 推挽)
 *   PB10 = SPI2_SCK  PB15 = SPI2_MOSI (AF5 硬件 SPI, 12MHz)
 *   PB11 = LCD_BL    (背光)
 *   PB5  = SW_LEFT   PB4 = SW_CENTRE  PB3 = SW_RIGHT (按下为低)
 *   PB6  = I2C_SCL   PB9 = I2C_SDA  (HUSB238 @0x08 / SC7A20, 软件 I2C)
 *   PA15 = HALL      PA13/PA14 = SWD
 * ==========================================================================*/
#include "regs.h"
#include "hw.h"

/* ---------------- SysTick 6ms 时基 ---------------- */
static volatile uint32_t g_ticks;
void SysTick_Handler(void) { g_ticks++; }
uint32_t millis(void) { return g_ticks; }
#define MS_PER_TICK 6u
void delay_ms(uint32_t ms)
{
    uint32_t ticks = (ms + MS_PER_TICK - 1u) / MS_PER_TICK;   /* v227: 参数是毫秒，tick=6ms */
    if (!ticks) ticks = 1;
    uint32_t t = millis();
    while (millis() - t < ticks) ;
}

/* ---------------- 时钟: MSI 4MHz → PLL 24MHz (flash 1WS) ---------------- */
void clock_init(void)
{
    /* ★ v218: 之前一直停在 MSI 默认 4MHz —— CPU 慢 6 倍、SPI 只有 2MHz，
     *   整屏 12.8KB 要传 51ms，动画怎么调都卡。改用 PLL：
     *   MSI 4MHz /PLLM1 ×PLLN24 = 96MHz VCO /PLLR4 = 24MHz
     *   SysTick 仍是 6ms/tick (RVR=144000-1)，v202 以来所有 tick 换算不变 */
    /* PWR 时钟 + Range 1：24MHz/1WS 稳定工作 (PWR_CR1.VOS=01) */
    RCC->APB1ENR1 |= (1u << 28);                           /* PWREN */
    (void)RCC->APB1ENR1;
    {
        volatile uint32_t *pwr_cr1 = (volatile uint32_t *)0x40007000u;
        volatile uint32_t *pwr_sr2 = (volatile uint32_t *)0x40007014u;
        *pwr_cr1 = (*pwr_cr1 & ~(3u << 9)) | (1u << 9);
        for (volatile uint32_t i = 0; i < 40000u && (*pwr_sr2 & (1u << 10)); i++) { }
    }
    REG32(0x40022000) = (REG32(0x40022000) & ~7u) | 1u     /* LATENCY=1 */
                      | (1u << 8) | (1u << 9);             /* PRFTEN + ICEN */
    /* ★ v124: 使能 SRAM2 时钟 — 否则 CPU 访问 0x10000000 区静默失败（黑匣子数据写不进去） */
    RCC->AHB1ENR |= (1u << 9);                             /* SRAM2EN */

    /* PLLCFGR: PLLSRC=MSI(01), PLLM=/1, PLLN=24, PLLREN=1, PLLR=/4 */
    RCC->PLLCFGR = (1u << 0) | (0u << 4) | (24u << 8)
                 | (1u << 24) | (1u << 25);
    RCC->CR |= (1u << 24);                                 /* PLLON */
    {
        uint32_t n = 0;
        while (!(RCC->CR & (1u << 25)) && ++n < 200000u) { }   /* PLLRDY */
        if (!(RCC->CR & (1u << 25))) {                     /* 锁定失败 → 留在 4MHz */
            RCC->CR &= ~(1u << 24);
            SYST_RVR = 24000u - 1u;
            SYST_CSR = 7u;
            return;
        }
    }
    RCC->CFGR = (RCC->CFGR & ~3u) | 3u;                    /* SW=PLL */
    {
        uint32_t n = 0;
        while (((RCC->CFGR >> 2) & 3u) != 3u && ++n < 100000u) { }
        if (((RCC->CFGR >> 2) & 3u) != 3u) {               /* 切换失败 → 留在 MSI */
            RCC->CR &= ~(1u << 24);
            SYST_RVR = 24000u - 1u;
            SYST_CSR = 7u;
            return;
        }
    }
    SYST_RVR = 144000u - 1u;                               /* 24MHz × 6ms */
    SYST_CSR = 7u;
}

/* ---------------- GPIO ---------------- */
/* v228: 上电第一时间把背光脚拉低，避免 GPIO 尚未配置时闪一下 */
void backlight_early_off(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 22)) | (1u << 22);   /* PB11 输出 */
    gpio_pin_clr(GPIOB, 11);                                     /* 低 = 背光灭 */
}

void gpio_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;

    /* PA3: TIM2_CH4 AF1 (HEAT PWM) */
    gpio_pin_mode(GPIOA, 3, GPIO_MODE_AF);
    gpio_pin_af(GPIOA, 3, 1);

    /* PA5/PA6, PB1: 模拟 (ADC) */
    gpio_pin_mode(GPIOA, 5, GPIO_MODE_ANALOG);
    gpio_pin_mode(GPIOA, 6, GPIO_MODE_ANALOG);
    gpio_pin_mode(GPIOB, 1, GPIO_MODE_ANALOG);

    /* LCD 控制线: 输出推挽; SCK/MOSI = AF5 (SPI2) */
    gpio_pin_mode(GPIOA, 7, GPIO_MODE_OUTPUT);
    gpio_pin_mode(GPIOB, 0, GPIO_MODE_OUTPUT);
    gpio_pin_mode(GPIOB, 2, GPIO_MODE_OUTPUT);
    gpio_pin_mode(GPIOB, 10, GPIO_MODE_AF);
    gpio_pin_af(GPIOB, 10, 5);
    gpio_pin_mode(GPIOB, 15, GPIO_MODE_AF);
    gpio_pin_af(GPIOB, 15, 5);
    gpio_pin_mode(GPIOB, 11, GPIO_MODE_OUTPUT);
    /* 高速驱动 (LCD SCK/MOSI) */
    GPIOB->OSPEEDR |= (3u << 20) | (3u << 30);

    /* 按键: 输入上拉 */
    gpio_pin_mode(GPIOB, 3, GPIO_MODE_INPUT);
    gpio_pin_mode(GPIOB, 4, GPIO_MODE_INPUT);
    gpio_pin_mode(GPIOB, 5, GPIO_MODE_INPUT);
    gpio_pin_pupd(GPIOB, 3, GPIO_PUPDR_PU);
    gpio_pin_pupd(GPIOB, 4, GPIO_PUPDR_PU);
    gpio_pin_pupd(GPIOB, 5, GPIO_PUPDR_PU);

    /* 软件 I2C: 开漏 + 上拉 */
    gpio_pin_mode(GPIOB, 6, GPIO_MODE_OUTPUT);
    gpio_pin_otyper(GPIOB, 6, 1);
    gpio_pin_mode(GPIOB, 9, GPIO_MODE_OUTPUT);
    gpio_pin_otyper(GPIOB, 9, 1);
    gpio_pin_pupd(GPIOB, 6, GPIO_PUPDR_PU);
    gpio_pin_pupd(GPIOB, 9, GPIO_PUPDR_PU);

    /* HALL: 输入 + 上拉（磁吸低电平有效；未装霍尔时保持无效） */
    gpio_pin_mode(GPIOA, 15, GPIO_MODE_INPUT);
    gpio_pin_pupd(GPIOA, 15, GPIO_PUPDR_PU);

    /* 背光: 初始化期间保持熄灭 (规避上电花屏), lcd_init 完成后点亮 */
    gpio_pin_clr(GPIOB, 11);
}

void gpio_pin_otyper(GPIO_TypeDef *g, int pin, int od)
{
    g->OTYPER = (g->OTYPER & ~(1u << pin)) | ((uint32_t)od << pin);
}

/* ---------------- SPI2 (LCD, 12MHz mode0) ---------------- */
void spi2_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_SPI2EN;
    SPI2->CR1 = 0;
    SPI2->CR1 = (1u << 2)          /* MSTR */
              | (1u << 9)          /* SSM 软件 NSS */
              | (1u << 8)          /* SSI=1 */
              | (0u << 3);         /* 波特率 = PCLK/2 = 12MHz */
    SPI2->CR2 = 0;                 /* 8bit */
    SPI2->CR1 |= (1u << 6);        /* SPE */
}

void spi2_write(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        while (!(SPI2->SR & 2)) ;                  /* TXE */
        *(volatile uint8_t *)&SPI2->DR = b[i];
    }
    while (SPI2->SR & 0x80) ;                      /* BSY=0 */
    (void)SPI2->SR;
    (void)SPI2->DR;                                /* 清 OVR */
}

/* ---------------- ADC ---------------- */
void adc_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC1EN;

    /* ★ 内核时钟必须用同步 HCLK/2 (CCR.CCKMODE): ADCSEL 异步 mux 四个源
     *   实测全部喂不动校准。此行丢失 = ADCAL 永远不完成 → 卡死 (踩过两次!) */
    ADC12_COMMON->CCR = (ADC12_COMMON->CCR & ~0x30000u)
                      | ADC_CCR_CCKMODE_HCLK_DIV2;

    ADC1->CR |= ADC_CR_DEEPPWD;
    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC1->CR |= ADC_CR_ADVREGEN;
    delay_ms(1);

    ADC1->CR |= ADC_CR_ADCAL;
    while (ADC1->CR & ADC_CR_ADCAL) ;

    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while (!(ADC1->ISR & ADC_ISR_ADRDY)) ;
    /* 设计基线 ADC 使用默认 2.5 拍采样；此处保留 v002 配置，避免改变 V 读数 */
    ADC1->SMPR2 |= (0x3u << 0) | (0x3u << 3) | (0x3u << 18);
}

static uint16_t adc_read_ch(int ch)
{
    ADC1->SQR1 = (uint32_t)ch << 6;
    ADC1->ISR = ADC_ISR_EOC;
    ADC1->CR |= ADC_CR_ADSTART;
    while (!(ADC1->ISR & ADC_ISR_EOC)) ;
    return (uint16_t)(ADC1->DR & 0xFFFF);
}

uint16_t adc_temp(void)  { return adc_read_ch(10); }   /* PA5 = ADC1_IN10 */
uint16_t adc_iout(void)  { return adc_read_ch(11); }   /* PA6 = ADC1_IN11 */
uint16_t adc_vin(void)   { return adc_read_ch(16); }   /* PB1 = ADC1_IN16 */

/* ---------------- HEAT PWM (TIM2_CH4 @PA3) ---------------- */
void pwm_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
    /* PD245 设计基线加热 PWM 为 25kHz；24MHz/3/320 = 25kHz */
    TIM2->PSC = 2;                            /* 24MHz/3 = 8MHz */
    TIM2->ARR = 320 - 1;                      /* /320 = 25kHz */
    TIM2->CCMR2 = (6u << 12) | (1u << 11);    /* CH4 PWM1 + OC4PE */
    TIM2->CCER |= (1u << 12);                 /* CC4E */
    TIM2->CCR4 = 0;
    TIM2->EGR |= (1u << 0);
    TIM2->CR1 |= 1;
}

void pwm_set(uint16_t duty) { TIM2->CCR4 = duty; }   /* duty 0..319 对应 0..100% */

/* ---------------- 按键 (按丝印: PB5=左 PB3=右; 不随旋转反转, 待旋转验证后再启用) ---------------- */
int btn_left(void)   { return !gpio_pin_read(GPIOB, 5); }
int btn_centre(void) { return !gpio_pin_read(GPIOB, 4); }
int btn_right(void)  { return !gpio_pin_read(GPIOB, 3); }
int hall_active(void){ return !gpio_pin_read(GPIOA, 15); }   /* 磁吸低电平有效 */

/* ---------------- 软件 I2C (PB6=SCL, PB9=SDA) ---------------- */
#define I2C_SCL_HIGH() gpio_pin_set(GPIOB, 6)
#define I2C_SCL_LOW()  gpio_pin_clr(GPIOB, 6)
#define I2C_SDA_HIGH() gpio_pin_set(GPIOB, 9)
#define I2C_SDA_LOW()  gpio_pin_clr(GPIOB, 9)
#define I2C_SDA_READ() gpio_pin_read(GPIOB, 9)
/* ★ v228: 24MHz 下 40 次只有 ~0.7µs，SCL 会冲到 HUSB238 的 400kHz 以上；×6 恢复到 ~150kHz */
#define I2C_DELAY()    { volatile int d = 240; while (d--) ; }

static void i2c_start(void)
{
    I2C_SDA_HIGH(); I2C_SCL_HIGH(); I2C_DELAY();
    I2C_SDA_LOW(); I2C_DELAY();
    I2C_SCL_LOW(); I2C_DELAY();
}
static void i2c_stop(void)
{
    I2C_SDA_LOW(); I2C_DELAY();
    I2C_SCL_HIGH(); I2C_DELAY();
    I2C_SDA_HIGH(); I2C_DELAY();
}
static uint8_t i2c_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) I2C_SDA_HIGH(); else I2C_SDA_LOW();
        b <<= 1; I2C_DELAY();
        I2C_SCL_HIGH(); I2C_DELAY();
        I2C_SCL_LOW(); I2C_DELAY();
    }
    I2C_SDA_HIGH(); I2C_DELAY();
    I2C_SCL_HIGH(); I2C_DELAY();
    uint8_t nack = I2C_SDA_READ();
    I2C_SCL_LOW(); I2C_DELAY();
    return nack;
}
static uint8_t i2c_read_byte(uint8_t ack)
{
    uint8_t b = 0;
    I2C_SDA_HIGH();
    for (int i = 0; i < 8; i++) {
        I2C_DELAY();
        I2C_SCL_HIGH(); I2C_DELAY();
        b = (b << 1) | I2C_SDA_READ();
        I2C_SCL_LOW(); I2C_DELAY();
    }
    if (ack) I2C_SDA_LOW(); else I2C_SDA_HIGH();
    I2C_DELAY();
    I2C_SCL_HIGH(); I2C_DELAY();
    I2C_SCL_LOW();
    I2C_SDA_HIGH(); I2C_DELAY();
    return b;
}

uint8_t husb238_read(uint8_t reg)
{
    if (!husb_ack) return 0;
    i2c_start();
    i2c_write_byte(0x08 << 1);
    i2c_write_byte(reg);
    i2c_start();
    i2c_write_byte((0x08 << 1) | 1);
    uint8_t v = i2c_read_byte(0);
    i2c_stop();
    return v;
}

uint8_t husb_diag_addr = 0, husb_diag_reg = 0, husb_diag_data = 0;
void husb238_write_diag(uint8_t reg, uint8_t val)
{
    i2c_start();
    husb_diag_addr = i2c_write_byte(0x08 << 1) ? 0 : 1;   /* 1=ACK */
    husb_diag_reg  = i2c_write_byte(reg) ? 0 : 1;
    husb_diag_data = i2c_write_byte(val) ? 0 : 1;
    i2c_stop();
}

void husb238_write(uint8_t reg, uint8_t val)
{
    /* ★ v49: 重试 10 次 (对齐设计基线 reg_write 的 timeout=10 语义) —
     * HUSB238 在 PD 协商期间 NACK 一切写入, 设计基线靠重试穿透;
     * 我们的写是单次尝试失败即弃 → 挡位请求从未落地 → 9V 写死 */
    for (int attempt = 0; attempt < 10; attempt++) {
        i2c_start();
        uint8_t n1 = i2c_write_byte(0x08 << 1);
        uint8_t n2 = i2c_write_byte(reg);
        uint8_t n3 = i2c_write_byte(val);
        i2c_stop();
        if (!n1 && !n2 && !n3) return;             /* 全部 ACK ✓ */
        { volatile int d = 2000; while (d--) ; }   /* NACK → 延迟重试 */
    }
}

/* ★ v50: 三种写法风格测试 — 让芯片自己告诉我们哪种写法被接受
 * style 0 = 1 字节写 (0x08=val)
 * style 1 = 10 字节连写 (0x08=val, 0x09=GO 1, 0x0A=GO 0, 余 0)
 * style 2 = 分两次 (0x08=val; 0x09=1; 0x09=0)
 * 返回 = 0x08 回读值 */
uint8_t husb238_write_style(int style, uint8_t val)
{
    uint8_t buf[10];
    if (style == 1) {
        buf[0] = val; buf[1] = 0x01; buf[2] = 0x00;
        for (int i = 3; i < 10; i++) buf[i] = 0;
        i2c_start();
        i2c_write_byte(0x08 << 1);
        i2c_write_byte(0x08);                      /* 起始寄存器 0x08, 自动递增到 0x11 */
        for (int i = 0; i < 10; i++) i2c_write_byte(buf[i]);
        i2c_stop();
    } else if (style == 2) {
        husb238_write(0x08, val);
        husb238_write(0x09, 0x01);
        husb238_write(0x09, 0x00);
    } else {
        husb238_write(0x08, val);
    }
    return husb238_read(0x08);
}

/* ---------------- HUSB238 PD 诱骗 (寄存器协议: Adafruit HUSB238 驱动核对) ----
 * 0x02-0x07 = SRC_PDO 5/9/12/15/18/20V: bit7=充电器广播支持, bits3:0=电流档
 * 0x08 SRC_PDO bits7:4 = 档位选择 (选择码非顺序: 5V=1,9V=2,12V=3,15V=8,18V=9,20V=10)
 * 0x09 GO_COMMAND bit0 = 1 触发 PD 请求 (重新协商)                     */
static const uint8_t pdo_sel_code[6] = {0x1, 0x2, 0x3, 0x8, 0x9, 0xA};

uint8_t husb238_pdo_avail(uint8_t gear)
{
    if (!husb_ack || gear > 5) return 0;
    return (husb238_read(0x02 + gear) >> 7) & 1;
}

uint16_t husb238_pdo_ma(uint8_t gear)
{
    static const uint16_t ma_tab[16] = {
        500, 700, 1000, 1250, 1500, 1750, 2000, 2250,
        2500, 2750, 3000, 3250, 3500, 4000, 4500, 5000
    };
    if (!husb_ack || gear > 5) return 0;
    return ma_tab[husb238_read(0x02 + gear) & 0x0F];
}

uint8_t husb_sel_rb = 0;   /* v21: 应用后回读 0x08 选择寄存器 (诊断) */

void husb238_select_request(uint8_t gear)
{
    /* ★ v34: 逐字对齐设计基线 0x0800720C 的选择流程 (早期核对 早期实测数据):
     * ① 0x08 整字节写预移位码 (低 4 位清零, 不做读改写!)
     * ② 0x09 写 0x01 (GO 置位, 其余位全清)
     * ③ 0x09 写 0x00 (GO 立即清零 — 置位→消费→清除) */
    static const uint8_t codes[6] = {0x10, 0x20, 0x30, 0x80, 0x90, 0xA0};
    if (!husb_ack || gear > 5) return;
    husb238_write(0x08, codes[gear]);
    husb238_write(0x09, 0x01);
    husb238_write(0x09, 0x00);
    husb_sel_rb = husb238_read(0x08);                  /* 回读验证选择是否写入 */
}

/* ---------------- SC7A20 加速度计 (LIS2DH 兼容) ---------------- */
uint8_t accel_addr = 0;
uint8_t accel_who = 0;   /* WHO_AM_I (0=未找到) */
uint8_t husb_ack = 0;    /* HUSB238(0x08) 总线应答 */

/* 全总线扫描: 返回某地址是否 ACK (位图 i2c_ack[addr/8] 的 addr%8 位) */
uint8_t i2c_ack[16];
void i2c_scan(void)
{
    for (int i = 0; i < 16; i++) i2c_ack[i] = 0;   /* ★ v229: 每次重扫都重建应答表 */
    /* ★ v228: 总线恢复 —— 若 SDA 被从机拉死，补 9 个 SCL 再 STOP */
    I2C_SDA_HIGH();
    for (int i = 0; i < 9; i++) { I2C_SCL_LOW(); I2C_DELAY(); I2C_SCL_HIGH(); I2C_DELAY(); }
    i2c_stop();
    for (int a = 1; a < 128; a++) {
        i2c_start();
        uint8_t nack = i2c_write_byte(a << 1);
        i2c_stop();
        if (!nack) i2c_ack[a / 8] |= (1 << (a % 8));
    }
}

static uint8_t i2c_rd_reg(uint8_t dev, uint8_t reg)
{
    i2c_start();
    if (i2c_write_byte(dev << 1)) { i2c_stop(); return 0xFF; }
    i2c_write_byte(reg);
    i2c_start();
    i2c_write_byte((dev << 1) | 1);
    uint8_t v = i2c_read_byte(0);
    i2c_stop();
    return v;
}

int accel_init(void)
{
    /* ★ v167: 必须校验 WHO_AM_I —— PD 芯片(HUSB238)也在总线上且地址靠前，
     *   旧逻辑取首个 ACK 地址 → 抓到 PD 芯片 → 加速度读数恒 0（调试器实测）
     *   策略：优先匹配 WHO_AM_I=0x33(SC7A20)；找不到再退回首个 ACK 地址 */
    uint8_t cand = 0;
    for (uint8_t a = 0x18; a <= 0x1F; a++) {
        if (!(i2c_ack[a / 8] & (1 << (a % 8)))) continue;
        uint8_t who = i2c_rd_reg(a, 0x0F);          /* WHO_AM_I (SC7A20=0x33) */
        accel_who = who;
        if (who == 0x33 || who == 0x11) { cand = a; break; }   /* 0x33=LIS2DH12, 0x11=SC7A20(实测) */
        if (!cand) cand = a;                        /* 兜底候选 */
    }
    if (!cand) { accel_addr = 0; return 0; }
    accel_addr = cand;
    i2c_start();
    i2c_write_byte(accel_addr << 1);
    i2c_write_byte(0x20);                   /* CTRL_REG1 */
    i2c_write_byte(0x37);                   /* ★ v255: 25Hz（设计基线休眠采样率）, XYZ 使能 */
    i2c_stop();
    i2c_start();
    i2c_write_byte(accel_addr << 1);
    i2c_write_byte(0x23);                   /* CTRL_REG4 */
    i2c_write_byte(0x08);                   /* HR=1: 12bit, 1mg/LSB */
    i2c_stop();
    return 1;
}

uint8_t accel_read_xyz(int8_t *gx, int8_t *gy, int8_t *gz)
{
    if (!accel_addr) { *gx = *gy = *gz = 0; return 0; }
    /* ★ v58: 单次 6 字节连续读 (自动递增, 0x28|0x80) — 替代 3 次独立事务, 主循环减负 ~10ms */
    uint8_t b[6];
    i2c_start();
    if (i2c_write_byte(accel_addr << 1)) { i2c_stop(); *gx = *gy = *gz = 0; return 0; }
    i2c_write_byte(0x28 | 0x80);                    /* OUT_X_L + 自动递增 */
    i2c_start();
    if (i2c_write_byte((accel_addr << 1) | 1)) { i2c_stop(); *gx = *gy = *gz = 0; return 0; }
    for (int i = 0; i < 6; i++) b[i] = i2c_read_byte(i < 5);   /* 最后一字节 NACK */
    i2c_stop();
    *gx = (int8_t)b[1];                             /* 只取高 8 位 (原逻辑) */
    *gy = (int8_t)b[3];
    *gz = (int8_t)b[5];
    return 1;
}

/* ★ v253: 设计基线同款 12bit 读数（raw>>4，1mg/LSB），供休眠运动判据使用 */
uint8_t accel_read_mg(int16_t *x, int16_t *y, int16_t *z)
{
    if (!accel_addr) { *x = *y = *z = 0; return 0; }
    /* ★ v256: 设计基线同款数据就绪同步，避免读到更新中的混合样本造成假运动 */
    uint8_t st = i2c_rd_reg(accel_addr, 0x27);
    if (!(st & 0x08)) { *x = *y = *z = 0; return 0; }
    uint8_t b[6];
    i2c_start();
    if (i2c_write_byte(accel_addr << 1)) { i2c_stop(); *x = *y = *z = 0; return 0; }
    i2c_write_byte(0x28 | 0x80);
    i2c_start();
    if (i2c_write_byte((accel_addr << 1) | 1)) { i2c_stop(); *x = *y = *z = 0; return 0; }
    for (int i = 0; i < 6; i++) b[i] = i2c_read_byte(i < 5);
    i2c_stop();
    *x = (int16_t)(((int16_t)((b[1] << 8) | b[0])) >> 4);
    *y = (int16_t)(((int16_t)((b[3] << 8) | b[2])) >> 4);
    *z = (int16_t)(((int16_t)((b[5] << 8) | b[4])) >> 4);
    return 1;
}

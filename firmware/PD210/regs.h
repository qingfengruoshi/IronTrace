/* ============================================================================
 * regs.h — STM32L431CCT6 最小寄存器定义（只含本工程用到的外设）
 * 依据 RM0351 (STM32L4x1 Reference Manual)。使用前请按手册复核位定义。
 * 侧重点：正确性优先于完整性。
 * ==========================================================================*/
#ifndef REGS_H
#define REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* ---- RCC (0x40021000) ---- */
typedef struct {
    volatile uint32_t CR;          /* 0x00 */
    volatile uint32_t ICSCR;       /* 0x04 */
    volatile uint32_t CFGR;        /* 0x08 */
    volatile uint32_t PLLCFGR;     /* 0x0C */
    volatile uint32_t PLLSAI1CFGR; /* 0x10 */
    volatile uint32_t PLLSAI2CFGR; /* 0x14 */
    volatile uint32_t CIER;        /* 0x18 */
    volatile uint32_t CIFR;        /* 0x1C */
    volatile uint32_t CICR;        /* 0x20 */
    uint32_t _r1;                  /* 0x24 */
    volatile uint32_t AHB1RSTR;    /* 0x28 */
    volatile uint32_t AHB2RSTR;    /* 0x2C */
    volatile uint32_t AHB3RSTR;    /* 0x30 */
    uint32_t _r2;                  /* 0x34 */
    volatile uint32_t APB1RSTR1;   /* 0x38 */
    volatile uint32_t APB1RSTR2;   /* 0x3C */
    volatile uint32_t APB2RSTR;    /* 0x40 */
    uint32_t _r3;                  /* 0x44 */
    volatile uint32_t AHB1ENR;     /* 0x48 */
    volatile uint32_t AHB2ENR;     /* 0x4C */
    volatile uint32_t AHB3ENR;     /* 0x50 */
    uint32_t _r4;                  /* 0x54 */
    volatile uint32_t APB1ENR1;    /* 0x58 */
    volatile uint32_t APB1ENR2;    /* 0x5C */
    volatile uint32_t APB2ENR;     /* 0x60 */
} RCC_TypeDef;
#define RCC ((RCC_TypeDef *)0x40021000u)

/* RCC 位定义（本工程用到的） */
#define RCC_CR_MSION        (1u << 0)
#define RCC_CR_MSIRDY       (1u << 1)
#define RCC_AHB2ENR_GPIOAEN (1u << 0)
#define RCC_AHB2ENR_GPIOBEN (1u << 1)
#define RCC_AHB2ENR_ADC1EN  (1u << 13)  /* ★ ADC 总线钟 = bit13 (bit16 实测无效, 调试器验证) */
#define RCC_APB1ENR1_TIM2EN (1u << 0)
#define RCC_APB1ENR1_I2C1EN (1u << 21)
#define RCC_APB2ENR_SYSCFGEN (1u << 0)

/* ---- GPIO (GPIOA 0x48000000, GPIOB 0x48000400, 每个 1KB) ---- */
typedef struct {
    volatile uint32_t MODER;   /* 0x00 */
    volatile uint32_t OTYPER;  /* 0x04 */
    volatile uint32_t OSPEEDR; /* 0x08 */
    volatile uint32_t PUPDR;   /* 0x0C */
    volatile uint32_t IDR;     /* 0x10 */
    volatile uint32_t ODR;     /* 0x14 */
    volatile uint32_t BSRR;    /* 0x18 */
    volatile uint32_t LCKR;    /* 0x1C */
    volatile uint32_t AFRL;    /* 0x20 */
    volatile uint32_t AFRH;    /* 0x24 */
} GPIO_TypeDef;
#define GPIOA ((GPIO_TypeDef *)0x48000000u)
#define GPIOB ((GPIO_TypeDef *)0x48000400u)

#define GPIO_MODE_INPUT 0u
#define GPIO_MODE_OUTPUT 1u
#define GPIO_MODE_AF 2u
#define GPIO_MODE_ANALOG 3u
#define GPIO_PUPDR_NONE 0u
#define GPIO_PUPDR_PU 1u
#define GPIO_PUPDR_PD 2u

static inline void gpio_pin_mode(GPIO_TypeDef *g, int pin, int mode)
{
    g->MODER = (g->MODER & ~(3u << (2 * pin))) | ((uint32_t)mode << (2 * pin));
}
static inline void gpio_pin_pupd(GPIO_TypeDef *g, int pin, int p)
{
    g->PUPDR = (g->PUPDR & ~(3u << (2 * pin))) | ((uint32_t)p << (2 * pin));
}
static inline void gpio_pin_af(GPIO_TypeDef *g, int pin, int af)
{
    if (pin < 8) g->AFRL = (g->AFRL & ~(0xFu << (4 * pin))) | ((uint32_t)af << (4 * pin));
    else g->AFRH = (g->AFRH & ~(0xFu << (4 * (pin - 8)))) | ((uint32_t)af << (4 * (pin - 8)));
}
static inline void gpio_pin_set(GPIO_TypeDef *g, int pin) { g->BSRR = 1u << pin; }
static inline void gpio_pin_clr(GPIO_TypeDef *g, int pin) { g->BSRR = 1u << (16 + pin); }
static inline int gpio_pin_read(GPIO_TypeDef *g, int pin) { return (g->IDR >> pin) & 1u; }

/* ---- TIM2 (0x40000000) —— HEAT PWM ---- */
typedef struct {
    volatile uint32_t CR1;   /* 0x00 */
    volatile uint32_t CR2;   /* 0x04 */
    volatile uint32_t SMCR;  /* 0x08 */
    volatile uint32_t DIER;  /* 0x0C */
    volatile uint32_t SR;    /* 0x10 */
    volatile uint32_t EGR;   /* 0x14 */
    volatile uint32_t CCMR1; /* 0x18 */
    volatile uint32_t CCMR2; /* 0x1C */
    volatile uint32_t CCER;  /* 0x20 */
    volatile uint32_t CNT;   /* 0x24 */
    volatile uint32_t PSC;   /* 0x28 */
    volatile uint32_t ARR;   /* 0x2C */
    uint32_t _r1;            /* 0x30 */
    volatile uint32_t CCR1;  /* 0x34 */
    volatile uint32_t CCR2;  /* 0x38 */
    volatile uint32_t CCR3;  /* 0x3C */
    volatile uint32_t CCR4;  /* 0x40 */
} TIM2_TypeDef;
#define TIM2 ((TIM2_TypeDef *)0x40000000u)

/* ---- ADC1 (0x50040000, AHB2 总线; 偏移依 RM0351, 经调试器实测该地址可读) ---- */
typedef struct {
    volatile uint32_t ISR;   /* 0x00 */
    volatile uint32_t IER;   /* 0x04 */
    volatile uint32_t CR;    /* 0x08 */
    volatile uint32_t CFGR;  /* 0x0C */
    volatile uint32_t CFGR2; /* 0x10 */
    volatile uint32_t SMPR1; /* 0x14 */
    volatile uint32_t SMPR2; /* 0x18 */
    uint32_t _r1;            /* 0x1C */
    volatile uint32_t TR1;   /* 0x20 */
    volatile uint32_t TR2;   /* 0x24 */
    volatile uint32_t TR3;   /* 0x28 */
    uint32_t _r2;            /* 0x2C */
    volatile uint32_t SQR1;  /* 0x30 */
    volatile uint32_t SQR2;  /* 0x34 */
    volatile uint32_t SQR3;  /* 0x38 */
    volatile uint32_t SQR4;  /* 0x3C */
    volatile uint32_t DR;    /* 0x40 */
} ADC_TypeDef;
#define ADC1 ((ADC_TypeDef *)0x50040000u)

/* ADC12 common (0x50040300): CCR 的 CCKMODE[17:16] 选内核时钟 */
typedef struct {
    volatile uint32_t RESERVED[2];  /* 0x00 CSR(读), 0x04 CDR */
    volatile uint32_t CCR;          /* 0x08: [17:16] CCKMODE */
} ADC_COMMON_TypeDef;
#define ADC12_COMMON ((ADC_COMMON_TypeDef *)0x50040300u)
#define ADC_CCR_CCKMODE_HCLK_DIV2 (1u << 16)

#define ADC_CR_ADCAL   (1u << 31)
#define ADC_CR_ADVREGEN (1u << 28)
#define ADC_CR_DEEPPWD (1u << 29)
#define ADC_CR_ADEN    (1u << 0)
#define ADC_CR_ADSTART (1u << 2)
#define ADC_ISR_EOC    (1u << 2)
#define ADC_ISR_ADRDY  (1u << 0)

/* ---- SysTick ---- */
#define SYST_CSR  REG32(0xE000E010u)
#define SYST_RVR  REG32(0xE000E014u)
#define SYST_CVR  REG32(0xE000E018u)

/* ---- FLASH (0x40022000) —— OTP 读取 / 写保护后续用 ---- */
#define FLASH_BASE 0x40022000u

/* ---- 内存映射常量 ---- */
#define OTP_BASE   0x1FFF7000u   /* OTP 区（93 字节） */
#define UID_BASE   0x1FFF7590u   /* 96 位 UID */
#define CCM_BASE   0x10000000u   /* 16KB CCM SRAM（原固件 KEY/缓冲区在此） */

/* ---- SPI2 (0x40003800, APB1) —— LCD 高速刷屏 ---- */
typedef struct {
    volatile uint32_t CR1;   /* 0x00 */
    volatile uint32_t CR2;   /* 0x04 */
    volatile uint32_t SR;    /* 0x08 */
    volatile uint32_t DR;    /* 0x0C */
} SPI_TypeDef;
#define SPI2 ((SPI_TypeDef *)0x40003800u)
#define RCC_APB1ENR1_SPI2EN (1u << 14)
#define RCC_CR_MSIRGSEL     (1u << 4)

#endif /* REGS_H */

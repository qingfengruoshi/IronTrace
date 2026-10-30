/* ============================================================================
 * startup.c — 向量表 + Reset_Handler
 *
 * ★★ 向量表必须放这里（.isr_vector 段 + used 属性），链接脚本用 KEEP(*(.isr_vector))
 *    保留。之前直接写在 .ld 的 LONG() 里：--gc-sections 把 Default_Handler /
 *    SysTick_Handler 的 section 当垃圾回收，符号解析成 0，向量表 2..15 项全变成
 *    0x00000001 —— SysTick 一触发就跳 0 地址跑飞（2026-10-06 调试器实锤）。
 * ==========================================================================*/
#include <stdint.h>
#include "regs.h"

extern uint32_t _sdata, _edata, _sidata, _sbss, _ebss;
extern uint32_t _estack;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void);

typedef void (*isr_t)(void);

__attribute__((section(".isr_vector"), used))
const isr_t vector_table[48] = {
    (isr_t)&_estack,          /* 0: 初始 SP = 0x20010000 */
    Reset_Handler,            /* 1: Reset */
    Default_Handler,          /* 2: NMI */
    Default_Handler,          /* 3: HardFault */
    Default_Handler,          /* 4: MemManage */
    Default_Handler,          /* 5: BusFault */
    Default_Handler,          /* 6: UsageFault */
    0, 0, 0, 0,               /* 7-10: reserved */
    Default_Handler,          /* 11: SVCall */
    Default_Handler,          /* 12: DebugMonitor */
    0,                        /* 13: reserved */
    Default_Handler,          /* 14: PendSV */
    SysTick_Handler,          /* 15: SysTick */
    /* 16-47: IRQ0-31（本工程未启用外设中断，统一兜底） */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
};

void Reset_Handler(void)
{
    /* ★ v231: PB11=LCD_BL 外部有 4.7k 上拉，开机先拉低，
     *   避免 .data/.bss 初始化期间背光 MOSFET 被上拉点亮闪一下 */
    RCC->AHB2ENR |= (1u << 1);                                  /* GPIOBEN */
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 22)) | (1u << 22);   /* PB11 输出 */
    gpio_pin_clr(GPIOB, 11);

    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    dst = &_sbss;
    while (dst < &_ebss) *dst++ = 0;
    main();
    for (;;) ;
}

void Default_Handler(void) { for (;;) ; }

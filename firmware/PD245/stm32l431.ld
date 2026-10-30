/* ============================================================================
 * stm32l431.ld — 向量表在 startup.c 里定义（.isr_vector 段），这里用 KEEP 锁死。
 * ★ 不要再把向量表写在脚本 LONG() 里：--gc-sections 会回收 Default_Handler /
 *   SysTick_Handler，符号解析成 0（2026-10-06 实锤）。
 * ==========================================================================*/
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
    /* STM32L431CCT6: SRAM1 = 48K @0x20000000; SRAM2 = 16K @0x10000000,
     * 并在 0x2000C000..0x2000FFFF 有别名。黑匣子 T/D/R 占
     * 0x10001000..0x10003FFF，栈必须留在 SRAM1 内 (顶 0x2000C000)；
     * 否则 123S 写满时 R 会覆盖 0x2000FFxx 的栈顶 (v019 潜在卡死)。 */
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 48K
    CCM   (rwx) : ORIGIN = 0x10000000, LENGTH = 16K
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    .text 0x08000000 : {
        KEEP(*(.isr_vector))      /* 向量表：48 项 × 4B，GC 不可触碰 */
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > SRAM AT> FLASH
    .bss  (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > SRAM
}

# Contributors

| Contributor | Role |
|---|---|
| **Qoder Agent** | AI coding/design assistant: UI concepts, firmware drawing/motion implementation and documentation. The underlying model is not part of this project's public documentation. |
| **qingfengruoshi** | Maintainer of this firmware line; v281+ board bring-up, feature work and APEX UI direction |

## Built with

- C11, bare-metal STM32L431CCT6, no HAL/RTOS/UI framework
- arm-none-eabi-gcc 10.3.1, `-O2`, 24 MHz, 6 ms SysTick
- GC9D01N 160×40 RGB565 software framebuffer
- Hand-built 5×7 / 16×16 / 7-segment drawing, APEX/VECTOR motion

No AI runtime is included in the firmware. AI assistance was used offline for
design and code generation/review only.

## License

Code is licensed under GPL-3.0-or-later; see `LICENSE`.
Font notices are in `LICENSING.md` and `NOTICE`.

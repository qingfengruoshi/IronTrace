# PD210 / PD245 黑匣子读取工具（只读）

通过 PW Link（CMSIS-DAP）+ pyOCD 读取黑匣子，生成 CSV / JSON / PNG / 自包含 HTML 和 ZIP 导出包；AI 摘要按需导出。
**不复位、不烧录、不擦除。**

## 环境

- Windows 10 / 11，Python 3.10+（含 Tkinter）
- PW Link 或其他 CMSIS-DAP 调试器（读设备时需要）
- Python 包：

```bat
pip install -r requirements.txt
```

`run_gui.bat` 会优先使用环境变量 `PD_PY` 指向的 Python；未设置时回退到 `D:\tmp\pyocd_env`，再回退到系统 `python`。

## 使用

1. 完全退出 **PowerWriter**，给焊笔断电重上电或按复位。
2. 双击 `run_gui.bat`。
3. 选择产品（自动 / PD210 / PD245）和来源：
   - **落盘日志**：读 Flash 页 120 元数据 + 121–126 数据；
   - **实时缓冲**：读 SRAM2 当前记录（加热中可用，`fly_i=0` 表示还没有记录）；
   - **CSV 文件**：从已导出的 CSV 重新生成报告。
4. 点“读取并生成报告”。默认输出到 `D:\tmp\pd_blackbox`。
5. 需要发给 AI 或同事时，点“导出 AI 摘要”；它会写入 `.ai.md` / `.ai.json` 并更新同名 `_bundle.zip`。

命令行也可用：

```bat
D:\tmp\pyocd_env\Scripts\python.exe blackbox_reader.py --flash
D:\tmp\pyocd_env\Scripts\python.exe blackbox_reader.py --flash --ai
D:\tmp\pyocd_env\Scripts\python.exe blackbox_reader.py --live
D:\tmp\pyocd_env\Scripts\python.exe blackbox_reader.py --csv D:\tmp\fly.csv --target 300 --lm 25
D:\tmp\pyocd_env\Scripts\python.exe blackbox_reader.py --bin full_256k.bin
```

## 输出

默认读取生成同名文件：

- `.csv`：`index,time_s,temp_c,raw,duty,duty_pct`；
- `.json`：产品、日志写入固件、cnt、运行设置、统计、完整性、数据 ID、文件路径；
- `.png`：温度/占空比曲线 + 峰值/末 20 s/目标/LM/数据 ID；
- `.html`：摘要卡片 + 曲线 + 完整性检查 + 元数据 + 数据 ID，可离线打开；曲线支持鼠标悬停/滑块指针，显示该点的温度、duty、raw（功率未逐点记录，duty 是功率代理）；
- `_bundle.zip`：把 CSV/JSON/PNG/HTML 打包，适合整包发送。

点“导出 AI 摘要”（或 CLI 加 `--ai`）后才生成，并加入同名 ZIP：

- `.ai.md`：可直接发给 AI/同事的摘要 + 分析提示；
- `.ai.json`：机器可读的结构化数据（元数据、设置、统计、完整性、降采样曲线）。

## 数据标识

v335 / v023 起，日志带 **page 120 元数据**，工具对 **cnt + T + D + R** 计算：

```text
CRC64/ECMA-182( LE32(cnt) || T(16-bit LE) || D(16-bit LE) || R(16-bit LE) )
```

- 烙铁底栏显示 **CRC64 的低 48 位**，格式 `XXXX-XXXX-XXXX`（12 位十六进制）；
- 工具报告里的“数据 ID（烙铁显示）”是同一个 12 位码；
- 同时给完整 CRC64（16 位）和 MD5，用于归档核对；
- 旧 `FLY1` 日志没有元数据时，工具回退到 **8 位 CRC32**（`XXXX-XXXX`），并标注 legacy。

元数据页 120 记录：cnt、CRC64、运行序号、日志写入固件（如 V335/V023、V336/V024）、设备 UID、运行时的 target/LM/PDO/PID；预留平均 V/I/P 字段。运行序号只是方便人看，真正防重复的是 CRC64/UID。

## 注意

- 黑匣子只记录 **T/D/R**，没有逐点 V/I/P；HTML 指针显示 duty（功率代理），不是实际瓦数。元数据里的平均 V/I/P 字段目前预留，固件暂未累加。
- 有元数据时，目标/LM/PID 是**运行时设置**；旧 FLY1 日志没有元数据时，才是读取时设置。
- 12 位码在断电重启后不变（同一份日志）；关闭黑匣子、清空日志、整片擦除或新保存才会隐藏/更换。
- 读落盘日志请在加热停止、保存完成后再读；读实时缓冲请保持设备运行。
- 如果连接失败：先关 PowerWriter；再断电重上电/按复位。退出 PowerWriter 后目标可能仍停在烧录算法里，需要复位才会重新运行。
- AI 包不包含逐点 V/I/P；AI 只能基于温度/duty/raw 给建议，PID 调整必须实机验证。
- 只读工具不会修改 Flash、设置或黑匣子内容。

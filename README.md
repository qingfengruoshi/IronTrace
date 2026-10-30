# IronTrace

PD210 / PD245 V3 固件与黑匣子读取工具。

同一仓库维护两款焊笔固件：

| 产品 | 烙铁头 | 源码目录 | 当前版本 |
|---|---|---|---|
| PD210 | C210 | `firmware/PD210` | v336 |
| PD245 V3 | JBC245 / C245 | `firmware/PD245` | v024 |

主控均为 STM32L431CCT6，160×40 RGB565 屏，三键操作；UI、菜单、设置、PDO、休眠等主体代码共用。

> 仓库只保留源码、文档和工具；`build/`、bin、预览图、调试脚本和日志由 `.gitignore` 排除。

**完整操作说明见 [MANUAL_CN.md](MANUAL_CN.md)**（按键、SET/PID/CAL、黑匣子开关与清空、保护、V/I/P/RS 说明、默认值）。

> 尚未实机验证的项目（真实焊接负载、低电压 PDO 等）统一列在文末[“非阻塞尾巴”](#非阻塞尾巴不影响现有使用)，不影响现有功能。

## 目录结构

- `firmware/PD210/`：PD210（C210）v336 源码；
- `firmware/PD245/`：PD245 V3（JBC245）v024 源码；
- `tools/blackbox_reader/`：只读黑匣子读取工具；
- `README.md` / `MANUAL_CN.md`：仓库说明和完整操作说明。

两个固件目录独立构建、互不依赖。发布时 tag 打在 `master` 的发布提交上：

```bash
git tag -a pd210-v336 -m "PD210 v336"
git tag -a pd245-v024 -m "PD245 V3 v024"
```

Release 附件按型号区分；两个 tag 可以指向同一个 master 提交。

## 构建

工具链不在 PATH 时先加入：

```bash
export PATH="/d/Storage/Code_Storage/Bin/toolchain/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH"
```

**构建在对应固件目录里做**：

```bash
cd firmware/PD210
make clean && make      # 输出 build/pd245_fw.bin（文件名沿用历史，内容是 PD210）

cd firmware/PD245
make clean && make      # 输出 build/pd245_fw.bin（内容是 PD245）
```

> 从 GitHub clone 的用户：`git clone <仓库地址> IronTrace`，进入 `firmware/PD210` 或 `firmware/PD245` 后 `make`。新工作区第一次构建建议先 `make clean`。

烧录地址：`0x08000000`。

## 发布文件

| Release（tag） | 固件附件 | 工具附件 |
|---|---|---|
| PD210 v336（`pd210-v336`） | `pd210_v336.bin`（全版）、`pd210_v336_compact.bin`（简洁版） | `BlackBoxReader_<日期>.zip` |
| PD245 V3 v024（`pd245-v024`） | `pd245_v024.bin`（全版）、`pd245_v024_compact.bin`（简洁版） | 同一个工具包 |

- **全版** `pdNNN_vNNN.bin`：补齐到 `0x1B200`（111,104 B）；`0x18000` 起为 160×40 RGB565 启动图窗口。
- **简洁版** `pdNNN_vNNN_compact.bin`：代码 + 0xFF 补齐到 8 字节；不覆盖 `0x08018000` 启动图区。
- **读取工具包** `BlackBoxReader_<日期>.zip`：包含 `blackbox_reader.py`、`run_gui.bat`、`requirements.txt`、`README.md`；两个 Release 可共用同一个包。

tag 本身不能挂附件，需要基于 tag 创建 GitHub Release 后上传；同时让 tag 指向 master 发布提交。仓库不提交 `build/` 和 bin 产物。发布时可再附一份 `SHA256SUMS.txt`，方便核对下载文件。

## 黑匣子读取工具

`tools/blackbox_reader/` 是只读 PC 工具（PW Link + pyOCD）：

- `run_gui.bat` 打开 GUI，可选落盘日志/实时缓冲/CSV；
- 生成 CSV / JSON / PNG / 自包含 HTML，带目标、LM、峰值、末 20s、完整性检查；
- 数据标识为 **12 位码**（`CRC64/ECMA` 的低 48 位），与烙铁底栏显示一致；同时显示完整 CRC64 和 MD5，便于归档核对；
- 默认生成 `_bundle.zip`（CSV / JSON / PNG / HTML）；需要发给 AI/同事时，在 GUI 点“导出 AI 摘要”（或 CLI 加 `--ai`），才会生成 `.ai.md` / `.ai.json` 并更新 ZIP；
- 不复位、不烧录；使用前先完全退出 PowerWriter，目标无 ACK 时断电重上电。

详细说明见 [tools/blackbox_reader/README.md](tools/blackbox_reader/README.md)。

## 许可证

固件代码采用 **GPL-3.0-or-later**，见 [LICENSE](LICENSE)。字体许可和第三方声明见 [LICENSING.md](LICENSING.md)、[NOTICE](NOTICE)。

## 下载源码

```bash
git clone <仓库地址> IronTrace
cd IronTrace

# PD210 源码在 firmware/PD210
# PD245 源码在 firmware/PD245
```

GitHub 网页可直接 `Code → Download ZIP`。

## 非阻塞尾巴（不影响现有使用）

以下项目**尚未实测或未完全实测**，先明确标注；不影响现有功能和定稿，有条件时再补：

- **真实焊接负载未实测**：目前只有空烧/保温数据；焊点回温、恢复和连续焊接尚未验证。
- **低电压 PDO 档 / LM>25 未重测**：三点验证是在 PDO 5 档（20V）、LM25 下完成的；5/9/12/15/18V 档位未重测。LM35 只在 400°C 冷启动尾巴里短暂用过，稳态 duty 7.6%，没有触发积分钳位；真正 >25% 稳态高负载未验证。低电压下若 duty 顶到 LM 上限，需要升 LM 并同步提高积分钳位，再重测 400°C。
- **实时缓冲（SRAM2）读取未做实机加热验证**：落盘日志（Flash）和 CSV 路径已实测；实时缓冲要求黑匣子正在记录且正在加热。整机只有一个 Type-C 口，没有双向测试条件，这一项不再阻塞定稿；工具本身只读，实时分支与已实机验证的 Flash 读取共用同一套解析和报告流程。

详细背景和复测条件见 [MANUAL_CN.md](MANUAL_CN.md) 第 11 节“待验证 / 已知限制”。

## 当前状态（2026-10-30）

### PD245 V3（`firmware/PD245` v024）

- v024：翻页和开机收尾改为黑底 APEX 斜切，删除 `fb_prev` 整帧快照，bss 从 40.8 KB 降到 28.1 KB，栈余量约 20.4 KB；开机 READY 收尾等主循环稳定后再启动，避免初始化阶段掉帧。控温、休眠和黑匣子逻辑不变；新日志的元数据 FW 标记为 V024。**12 位码断电重启、关闭黑匣子恢复功率条已确认。**
- v022：把栈从 SRAM2 别名区移回 SRAM1，修复 123S 写满时 R 缓冲区覆盖栈顶导致界面/按键卡死；同时黑匣子开启期间禁止休眠（不改休眠设置，关闭黑匣子后恢复）。v019 的 61S/PID/温度逻辑不变。
- v023：新增 Flash page 120 元数据（CRC64、运行序号、写入固件、UID、运行时设置）；保存完成后主界面底栏居中显示 12 位数据码，常驻到关闭黑匣子或清空；再次加热恢复青色进度条，保存后换新码；关闭黑匣子恢复原来的功率百分比条。**已实测 61S：`B211-3525-66D0`（RUN #1，target 300/LM35，末20s 300.0±0.6°C，完整性 OK）。**
- 温度表按设计基线 245：raw `10/475/1115/1785/2450/2775` → `25/100/200/300/400/450°C`；raw >3100 无效，≥4000 NoIron，3101–3999 加热超范围退出。
- 25 kHz PWM；默认目标 300°C、LM25%、PID `2.00 / 0.010 / 1.00`。
- I/P/RS 只做显示：INA199 平均电流；V 用 PD210 分压标定；RS 为一次性冷态锁存；不参与 PID/保护。
- 黑匣子：60 ms/点；SET 第 10 项 关 / 61S / 123S / 清空；61S=1024 点≈61.4 s，123S=2048 点≈122.9 s；到 FULL 自动停热并落盘，FULL 停留约 1 s；固定 6 个数据页/12 KB + 元数据页 120，反复使用覆盖不累积；**开启期间设备不休眠，关闭后恢复休眠设置**；**保存后底栏居中显示 12 位数据码，常驻到关闭或清空**。
- 黑匣子存储：实时缓冲 SRAM2 `0x10001000–0x10003FFF`（12 KB）；元数据 Flash 页 120 `0x0803C000–0x0803C7FF`；数据页 121–126 `0x0803C800–0x0803F7FF`；设置页 127 独立。详见 [MANUAL_CN.md](MANUAL_CN.md) §4.4。
- v014 修 TEMP 采样（PWM 关断窗）；v015 修显示 199/299 卡顿；v017 修 SET 黑匣子“清空”提示；v018 设置改完松手立即落盘；v019 黑匣子增加 61S/123S 时长、FULL 自动停热、2048 点/6 页布局（SET 标签缩为“黑匣子”给 123S 让位）。
- 200/300/400 三点实测：`200.0 ± 0.4` / `300.0 ± 0.8`（11.3 s 到温，峰值 302）/ `400.2 ± 1.2`（12.2 s 到温，峰值 403）；稳态 duty 3.0 / 4.8 / 7.7%。**同一套 PID 三点通过，PID 定稿。**

### PD210（`firmware/PD210` v336）

- v336：与 PD245 v024 同步，翻页和开机收尾改为黑底 APEX 斜切，删除 `fb_prev` 整帧快照，bss 从 40.8 KB 降到 28.1 KB，栈余量约 20.4 KB；开机 READY 收尾等主循环稳定后再启动。控温、休眠和黑匣子逻辑不变；新日志的元数据 FW 标记为 V336。**12 位码断电重启、关闭黑匣子恢复功率条已确认。**
- v329 UI 基线 + v330 主界面黑匣子录制进度条；v331 设置改完松手立即落盘；v332 黑匣子对齐 PD245 v019：60 ms/点、SET 第 10 项 关 / 61S / 123S / 清空、到 FULL 自动停热并落盘、固定 6 页/12 KB。
- v334 已把栈移回 SRAM1，并在黑匣子开启期间禁止休眠；123S 已实测通过（2048 点 / 122.6 s）。
- v335：新增 Flash page 120 元数据（CRC64、运行序号、写入固件、UID、运行时设置）；保存后底栏居中显示 12 位数据码，常驻到关闭黑匣子或清空；再次加热恢复进度条，保存后换新码；关闭黑匣子恢复功率百分比条。**已实测 61S：`9985-C2D0-5C84`（RUN #1，target 300/LM15，末20s 301.3±2.4°C，完整性 OK）。**
- 操作逻辑与 PD245 共用；差异见 [MANUAL_CN.md](MANUAL_CN.md) 第 9 节。

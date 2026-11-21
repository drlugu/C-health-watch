# C-health-watch · ESP32-S3 智能健康手环

基于 **ESP32-S3** 的运动与久坐提醒终端，课程实训项目。使用 **C++17 / ESP-IDF 5.3.5 / FreeRTOS**，
硬件平台为 **正点原子 DNESP32S3** 开发板（16 MB Flash / 8 MB PSRAM）。

> 目标：白天做「运动监测 + 久坐提醒」闭环，夜间可选做「打鼾 / 睡眠分期 / 呼吸事件」，
> 数据可同步到 Web / 小程序端查看。当前进度见文末「课程进度」。

---

## 硬件

| 项目 | 型号 / 规格 |
|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8（双核 240 MHz，16 MB Flash，8 MB PSRAM） |
| 六轴 IMU | MPU6050（I²C 0x68） |
| PPG | MAX30102（I²C 0x57，选做） |
| 温度 | TMP117（I²C 0x48，选做） |
| 显示 | 4.3 英寸 800×480 触摸屏 |
| 其它 | 按键（BOOT / GPIO0）、震动马达、LED0（GPIO1，低电平点亮） |

## 目录结构

```
.
├── main/                 # 应用入口 app_main 与业务代码
│   └── main.c            # 当前：板级自检 + LED 闪烁 + 呼吸灯
├── components/BSP/       # 板级驱动（LED / KEY 等）
├── docs/                 # 文档与实测证据
│   ├── lesson03.md
│   └── evidence/         # 串口日志等验收证据
├── partitions-16MiB.csv  # 分区表
├── sdkconfig             # 工程配置（target=esp32s3）
└── CMakeLists.txt
```

## 构建与烧录

需要 ESP-IDF **v5.3.5**（工具链路径按本机实际安装为准）。

```bash
idf.py set-target esp32s3      # 仅首次
idf.py build
idf.py -p COM3 flash monitor   # COM3 换成设备管理器里的实际端口
```

> Windows 上建议用 **ESP-IDF 命令行**（`export.bat` 注入环境）或 VS Code 的 ESP-IDF 扩展；
> 不要在 Git Bash / MSYS2 里直接跑 `idf.py`。

## 已完成功能

| 课次 | 内容 | 状态 |
|---|---|---|
| 第 02 课 | 系统架构、可运行工程、Git 基线 | ✅ |
| 第 03 课 | LED 点灯（GPIO1 低电平点亮）、1 Hz 闪烁、板级自检、呼吸灯（LEDC 5 kHz） | ✅ |
| 第 04 课 | 按键输入与消抖 | ⬜ |
| 第 05 课 | I²C 协议与通信 | ⬜ |
| 第 06–07 课 | 传感器驱动、IMU 数据处理 | ⬜ |
| 第 08–09 课 | 计步算法、久坐状态机与提醒闭环 | ⬜ |
| 第 10 课 | RGB 显示屏与 LVGL | ⬜ |

### 第 03 课要点

- **板级自检**：芯片 / 主频 / Flash / PSRAM / MAC / 复位原因 / LED 回读 / 按键电平，实测 **5/5 通过**；
- **1 Hz 闪烁**：500 ms 亮 / 500 ms 灭，串口打印 `LED blink start`；
- **呼吸灯**：LEDC 5 kHz、12 位分辨率（4096 级）、10 ms 刷新（250 级/周期）、
  正弦曲线 `0.5 − 0.5cos(2πt/T)`，周期实测 **2500 ms**，每周期打印 `breath cycle = n`；
- **模式切换**：短按 BOOT 循环「1 Hz 闪烁 → 常亮 → 呼吸灯」。

## 版本与标签

| Tag | 说明 |
|---|---|
| `v0.1-baseline` | 可编译、可烧录的工程基线（第 02 课交付） |
| `v0.2-board-selfcheck` | 板级自检 + 1 Hz 闪烁 + 呼吸灯（第 03 课交付） |
| `v0.3-breath-smooth` | 呼吸灯平滑度与周期修正（12 位 / 10 ms / 正弦） |

功能在 `feature/*` 分支开发，验证通过后合并到 `main`；`main` 始终保持可编译可烧录。

## 已知问题

- 串口端口在 `sdkconfig`/`.vscode` 中为占位值，换机器后需改成实际 COM 口；
- 屏幕、传感器等外设尚未接入，相关代码在后续课次补充。

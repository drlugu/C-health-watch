# 开发环境配置清单（换机专用）

> 目的：在另一台电脑上复现本项目的开发环境。按顺序照做即可。
> 记录时间：2026-11-21　对应工程：`C-health-watch`（ESP-IDF 5.3.5 / ESP32-S3）

---

## 一、版本总览（唯一权威组合）

| 项 | 版本 / 值 |
|---|---|
| ESP-IDF | **v5.3.5** |
| 目标芯片 | **esp32s3** |
| 工具链 | xtensa-esp-elf **esp-13.2.0_20250707** |
| Python（IDF 专用） | **3.11.2**（虚拟环境） |
| CMake | 3.30.2 |
| Ninja | 1.12.1 |
| OpenOCD | v0.12.0-esp32-20251215 |
| ccache | 4.12.1 |
| idf-exe | 1.0.3 |
| Git | 2.44.0（IDF 自带）/ 系统 Git 2.56.0 |
| 开发板 | 正点原子 DNESP32S3（ESP32-S3-WROOM-1 N16R8） |
| 串口 | CH340 → **COM3**（换机器会变） |

> ⚠️ `D:\Espressif\tools` 里还残留着旧版（cmake 3.24.0、ninja 1.10.2、xtensa 12.2.0 等，来自早期的 5.1.2 安装包）。
> **不要用那些**，以 export.bat 实际导出的为准（即上表）。

---

## 二、安装位置（目录结构）

```
D:\Espressif\
├── frameworks\esp-idf-v5.3.5\      ← IDF 源码（IDF_PATH）
├── tools\                          ← 工具链（IDF_TOOLS_PATH 指向 D:\Espressif，不是这里！）
│   ├── xtensa-esp-elf\esp-13.2.0_20250707\
│   ├── cmake\3.30.2\  ninja\1.12.1\  openocd-esp32\v0.12.0-esp32-20251215\
│   ├── idf-python\3.11.2\  idf-git\2.44.0\  ccache\4.12.1\  idf-exe\1.0.3\
│   └── esp-clang\  riscv32-esp-elf\  esp32ulp-elf\  dfu-util\  esp-rom-elfs\
└── python_env\idf5.3_py3.11_env\    ← Python 虚拟环境（IDF_PYTHON_ENV_PATH）
```

**装到 D 盘的原因**：C 盘根目录需要管理员权限，D 盘直接可写，且空间充足。

---

## 三、环境变量（4 个必设 + PATH 顺序）

| 变量 | 值 | 说明 |
|---|---|---|
| `IDF_PATH` | `D:\Espressif\frameworks\esp-idf-v5.3.5` | IDF 源码根 |
| `IDF_TOOLS_PATH` | `D:\Espressif` | **不含 `\tools`**，IDF 会自己在下面找 `tools\` |
| `IDF_PYTHON_ENV_PATH` | `D:\Espressif\python_env\idf5.3_py3.11_env` | 虚拟环境 |
| `PYTHONUTF8` | `1` | 避免中文路径下 GBK 解码报错 |

**PATH 前置顺序**（必须放在 `export.bat` 之前）：

```
%IDF_TOOLS_PATH%\tools\idf-python\3.11.2
%IDF_TOOLS_PATH%\tools\idf-git\2.44.0\cmd
```

> `export.bat` 依赖 PATH 里已有 IDF 自带 Python，否则会报
> `tool xtensa-esp-elf has no installed versions`。

---

## 四、启动脚本（可直接复制成 .bat）

⚠️ **必须存为 GBK 编码 + CRLF 换行**，否则 cmd 会解析错乱。

```bat
@echo off
set MSYSTEM=
set MSYSTEM_PREFIX=
set MINGW_PREFIX=
set IDF_TOOLS_PATH=D:\Espressif
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.3.5
set IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.3_py3.11_env
set "IDF_GIT_DIR=D:\Espressif\tools\idf-git\2.44.0\cmd"
set "IDF_PY_DIR=D:\Espressif\tools\idf-python\3.11.2"
set PATH=%IDF_PY_DIR%;%IDF_GIT_DIR%;%PATH%
set PYTHONUTF8=1
call "%IDF_PATH%\export.bat"
cd /d <工程目录>
cmd /k
```

`set MSYSTEM=` 这几行是为了清掉 Git Bash / MSYS2 注入的变量——否则 `idf_tools.py` 会拒绝执行
（`ERROR: MSys/Mingw is not supported`）。

---

## 五、VS Code 配置

### 5.1 扩展（装这几个就够）

| 扩展 | 作用 | 必需 |
|---|---|---|
| `espressif.esp-idf-extension` | 编译/烧录/串口、头文件跳转 | 必需 |
| `ms-vscode.cpptools` | C/C++ 语言支持 | 必需 |
| `ms-vscode.cmake-tools` | CMake 工程 | 建议 |
| `ms-python.python` | Python 脚本 | 可选 |

安装命令：

```bash
code --install-extension espressif.esp-idf-extension
code --install-extension ms-vscode.cpptools
code --install-extension ms-vscode.cmake-tools
```

### 5.2 `settings.json` 关键项

用户级（`%APPDATA%\Code\User\settings.json`）或工作区 `.vscode/settings.json`：

```jsonc
{
  "idf.espIdfPath": "D:/Espressif/frameworks/esp-idf-v5.3.5",
  "idf.espIdfPathWin": "D:\\Espressif\\frameworks\\esp-idf-v5.3.5",
  "idf.toolsPath": "D:/Espressif",
  "idf.toolsPathWin": "D:\\Espressif",
  "idf.pythonInstallPath": "D:\\Espressif\\python_env\\idf5.3_py3.11_env\\Scripts\\python.exe",
  "idf.adapterTargetName": "esp32s3",
  "idf.flashBaudRate": "460800",
  "idf.monitorBaudRate": "115200",
  "idf.openOcdConfigs": ["board/esp32s3-builtin.cfg"]
}
```

### 5.3 `c_cpp_properties.json`（IntelliSense）

```jsonc
{
  "configurations": [{
    "name": "ESP-IDF",
    "compilerPath": "${config:idf.toolsPathWin}\\tools\\xtensa-esp-elf\\esp-13.2.0_20250707\\xtensa-esp-elf\\bin\\xtensa-esp32s3-elf-gcc.exe",
    "compileCommands": "${config:idf.buildPath}/compile_commands.json",
    "includePath": [
      "${config:idf.espIdfPath}/components/**",
      "${config:idf.espIdfPathWin}/components/**",
      "${workspaceFolder}/**"
    ]
  }],
  "version": 4
}
```

> 如果换 IDF 版本，`compilerPath` 里的版本号要跟着改。

---

## 六、工程配置（sdkconfig 关键项）

| 配置 | 值 | 说明 |
|---|---|---|
| `CONFIG_IDF_TARGET` | `esp32s3` | 目标芯片 |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `16MB` | 板载 Flash |
| `CONFIG_ESPTOOLPY_FLASHMODE` | `dio` | 下载模式 |
| `CONFIG_SPIRAM` | `y` | 启用 PSRAM |
| `CONFIG_SPIRAM_MODE_OCT` | `y` | 八线 PSRAM（N16R8 是 OCT） |
| `CONFIG_SPIRAM_SPEED` | `80` | 80 MHz |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | `240` | 主频 |
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms 节拍（`pdMS_TO_TICKS` 精确） |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | 用自定义分区表 |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `partitions-16MiB.csv` | 见下 |
| `CONFIG_LOG_DEFAULT_LEVEL` | `3` | INFO |

**分区表 `partitions-16MiB.csv`**：

```csv
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,0x6000,,
phy_init,data,phy,0xf000,0x1000,,
factory,app,factory,0x10000,0x1F0000,,
vfs,data,fat,0x200000,0xA00000,
storage,data,spiffs,0xc00000,0x400000,
```

---

## 七、常用命令

```bash
idf.py set-target esp32s3            # 首次配置芯片
idf.py build                         # 编译
idf.py -p COM3 flash                 # 烧录（COM3 换成实际端口）
idf.py -p COM3 monitor               # 串口（退出 Ctrl+]）
idf.py -p COM3 flash monitor         # 烧录并监视
idf.py fullclean                     # 彻底清理（换机器后建议先跑一次）
```

---

## 八、换机步骤（照这个顺序）

1. 拷贝整个 `D:\Espressif` 目录到新机器同路径（省去重装，约 3 GB）；
   - 若重装：用离线包 `esp-idf-tools-setup-offline-5.3.5.exe`，装到 `D:\Espressif`
2. 确认 `D:\Espressif\python_env\idf5.3_py3.11_env` 存在；没有就执行
   `python idf_tools.py install-python-env`（需联网）
3. 建启动脚本（见第四节），存为 **GBK + CRLF**
4. 双击启动脚本，执行 `idf.py --version` 应输出 `ESP-IDF v5.3.5`
5. `git clone https://github.com/drlugu/C-health-watch.git`
6. 打开工程 → **删除旧 `build/`**（缓存里有上一台机器的绝对路径）
7. `idf.py set-target esp32s3` → `idf.py build`
8. 装 CH340 驱动，设备管理器确认 COM 号，把 `.vscode/settings.json` 里 `idf.portWin` 改掉
9. VS Code 里装 ESP-IDF 扩展，指向 `D:\Espressif`

---

## 九、踩过的坑（复现时必看）

| 现象 | 原因 | 解决 |
|---|---|---|
| `MSys/Mingw is not supported` | 从 Git Bash 启动了安装器/`idf.py` | 启动脚本里 `set MSYSTEM=`，只用 cmd |
| `tool xxx has no installed versions` | `IDF_TOOLS_PATH` 写成了 `D:\Espressif\tools` | 改成 `D:\Espressif`（不含 `\tools`） |
| `UnicodeDecodeError: 'gbk' codec` | 工程路径含中文 | 工程放纯英文路径；或设 `PYTHONUTF8=1` |
| ccache `Illegal byte sequence` | 中文/括号路径下 ccache 4.12 崩溃 | 工程移到英文路径（`D:\work`） |
| `.bat` 里中文乱码、命令被截断 | 文件存成了 UTF-8 | 存为 **GBK + CRLF**，不要用 `chcp 65001` |
| 编译报缓存路径错（如 `B:/...`） | `build/` 里留了别的机器的 CMakeCache | 删 `build/` 重编 |
| VS Code 满屏红波浪线 | `c_cpp_properties.json` 的 compilerPath 版本号不对 | 按 5.3 节改成实际版本 |
| 按键"第一次按没反应" | BSP `key_scan` 阻塞去抖且先清标志 | 已改为 10 ms 非阻塞采样（工程内） |

---

## 十、仓库与版本

- 仓库：https://github.com/drlugu/C-health-watch
- 标签：`v0.1-baseline`（基线）→ `v0.2-board-selfcheck`（第03课）→ `v0.3-breath-smooth`（呼吸灯修正）
- 分支策略：`main` 始终可编译可烧录；功能走 `feature/*`，验证后合并

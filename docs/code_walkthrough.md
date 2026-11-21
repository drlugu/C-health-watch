# 完整源码讲解（第 03 课 · LED 点灯与板级自检）

> 对应代码：`main/main.c`（332 行）+ `components/BSP/LED`（41 行）+ `components/BSP/KEY`（73 行）
> 对应版本：`v0.3-breath-smooth`　对应提交：`d128754`（或之后）
> 文中行号与当前文件一致，可直接在 VS Code 里对照跳转。

---

## 一、源码总览

| 文件 | 行数 | 职责 | 谁调用它 |
|---|---|---|---|
| `main/main.c` | 332 | 业务主线：自检 → 主循环 → 模式切换 | `app_main()` 由 IDF 启动 |
| `components/BSP/LED/led.h` | 51 | LED 引脚号 + 两个操作宏 | `main.c` |
| `components/BSP/LED/led.c` | 41 | `led_init()`：配 GPIO1 | `main.c` |
| `components/BSP/KEY/key.h` | 51 | 按键引脚号 + `BOOT` 宏 + `key_scan` 声明 | `main.c`（仅用宏与 `key_init`） |
| `components/BSP/KEY/key.c` | 73 | `key_init()` / `key_scan()`（阻塞去抖，本期未使用 `key_scan`） | — |

**分层**：`main.c`（业务）→ `BSP`（驱动）→ ESP-IDF（`driver/gpio.h`、`driver/ledc.h`）。
业务层不直接写寄存器，只调 `LED0()`、`ledc_set_duty()` 这类封装。

---

## 二、程序骨架（上电到跑起来）

```
上电复位
  └─ 二级 bootloader（ESP-IDF 自带，不在这份代码里）
       └─ app_main()                                   ← 第 270 行
            ├─ led_init()         配 GPIO1 为输出，默认灭   ← 第 272 行
            ├─ key_init()         配 GPIO0 为上拉输入      ← 第 273 行
            ├─ board_self_check() 打印自检报告             ← 第 275 行
            ├─ breath_init()      配好 LEDC（5 kHz/12 位） ← 第 277 行
            ├─ set_mode(BLINK)    进入 1Hz 闪烁            ← 第 278 行
            └─ while(1) 每 10 ms 循环一次                  ← 第 283~331 行
                 ├─ 读按键（非阻塞消抖）→ 有按下就切模式
                 └─ 按当前模式做动作（闪烁/常亮/呼吸）
```

**关键设计**：只有一个任务（`app_main` 本身就是 FreeRTOS 的 `main` 任务）。
所有动作都靠“每 10 ms 轮询一次 + 比较时间戳”实现，**没有任何 `while` 空转等待**。

---

## 三、main.c 逐段讲解

### 3.1 文件头注释（第 1–16 行）

```c
/**
 * @file        main.c
 * @brief       第 03 课：LED 点灯与板级自检
 */
```

- 第 6–9 行写清「本程序做三件事」，这是给自己和评审看的**行为契约**；
- 第 11–14 行是课件要点：GPIO1 低电平点亮、必须 `INPUT_OUTPUT`、延时用 `vTaskDelay`。

### 3.2 头文件（第 18–34 行）

| 行 | 头文件 | 用到什么 |
|---|---|---|
| 18 | `stdio.h` | 本文件其实没直接用（保留兼容） |
| 19 | `math.h` | `cosf()`、`M_PI`（第 314 行） |
| 20 | `inttypes.h` | `PRIu32` 打印 `uint32_t`（第 140 行） |
| 22–23 | `freertos/FreeRTOS.h`、`task.h` | `vTaskDelay()`、`pdMS_TO_TICKS()` |
| 24 | `esp_log.h` | `ESP_LOGI/W/E`、`ESP_ERROR_CHECK` |
| 25 | `esp_system.h` | `esp_reset_reason()` |
| 26 | `esp_chip_info.h` | `esp_chip_info_t`、芯片特性位 |
| 27 | `esp_flash.h` | `esp_flash_get_size()` |
| 28 | `esp_mac.h` | `esp_read_mac()` |
| 29 | `esp_heap_caps.h` | `heap_caps_get_total_size()`（查 PSRAM/内部 RAM） |
| 30 | `esp_timer.h` | `esp_timer_get_time()`（微秒级时间戳） |
| 31 | `driver/ledc.h` | PWM：`ledc_timer_config` / `ledc_channel_config` / `ledc_set_duty` |
| 33–34 | `led.h`、`key.h` | 板级封装：`LED0()`、`LED0_TOGGLE()`、`BOOT_GPIO_PIN` |

> 注意第 33–34 行是**双引号**（工程内头文件），上面是尖括号（系统/框架头文件）。

### 3.3 日志 TAG（第 36 行）

```c
static const char *TAG = "SELFTEST";
```

`static` 让这个变量只在本文件可见；`const char *` 指向字符串常量。
所有 `ESP_LOGI(TAG, ...)` 都会输出 `I (时间) SELFTEST: 内容`。

### 3.4 模式枚举（第 39–44 行）

```c
typedef enum {
    MODE_BLINK  = 0,   /* 1Hz 闪烁（课件要求） */
    MODE_STEADY,       /* 常亮 */
    MODE_BREATH,       /* 呼吸灯（课后作业） */
    MODE_NUM           /* 哨兵：等于 3，表示"有几个模式" */
} led_mode_t;
```

- 用枚举而不是 `int`：编译器帮你检查取值，代码可读；
- 第 43 行 `MODE_NUM` 是**哨兵值**，配合第 286 行的 `% MODE_NUM` 做循环取模，加模式时只要在它前面插一个即可。

### 3.5 全局状态（第 46–47 行）

```c
static led_mode_t s_mode = MODE_BLINK;
static int64_t    s_breath_index = 0;
```

- `s_` 前缀表示 static（本文件私有）；
- `s_mode` 是**当前模式**，全文件唯一真相；
- `s_breath_index` 记录“已完成几个呼吸周期”，用来保证每个周期只打印一次日志（第 318–322 行）。

### 3.6 按键消抖的三件套（第 56–58 行）

```c
static bool s_btn_level = true;   // 去抖后的稳定电平：true=松开
static int  s_btn_same  = 0;      // 连续读到"和稳定电平不同"的次数
static int  s_press_cnt = 0;      // 累计按下次数（日志用）
```

### 3.7 按键轮询（第 60–73 行）★核心

```c
static bool key_poll_press(void)
{
    const bool raw = (gpio_get_level(BOOT_GPIO_PIN) != 0);   // 62
    if (raw == s_btn_level) { s_btn_same = 0; return false; } // 63-66
    if (++s_btn_same < 2) return false;                       // 67-69
    s_btn_level = raw;                                        // 70
    s_btn_same  = 0;                                          // 71
    return (raw == false);                                    // 72
}
```

| 行 | 作用 | 为什么这么写 |
|---|---|---|
| 62 | 读原始电平，转成 bool：**高=松开、低=按下** | BOOT 键有上拉，松开是高；按下接地是低 |
| 63–66 | 读到的和稳定电平一致 → 清计数、返回“没事件” | 稳定状态不触发任何东西 |
| 67–69 | 不一致就累加；**不足 2 次先不认** | 这就是消抖：抖动时电平来回跳，连续两次一致才可信 |
| 70 | 确认跳变，更新稳定电平 | 从此以新电平为准 |
| 72 | **只有变低才返回 true**（按下沿） | 松开沿返回 false，避免一次按键触发两次 |

> 参数说明：主循环每 10 ms 调一次（第 330 行），所以“连续 2 次”≈ **20 ms 消抖窗口**。
> 旧版 `key_scan()` 先清标志再复检，短按会被丢弃——这就是你之前“第一次按没反应”的原因。

### 3.8 模式名（第 75–83 行）

```c
static const char *mode_name(led_mode_t m)
```

纯查表，把枚举转成中文，只用于日志（第 289 行）。**日志里能看懂的字符串比数字更重要**。

### 3.9 呼吸灯参数宏（第 85–93 行）★

| 行 | 宏 | 值 | 含义 |
|---|---|---|---|
| 87 | `BREATH_TIMER` | `LEDC_TIMER_0` | 用 0 号定时器 |
| 88 | `BREATH_CHANNEL` | `LEDC_CHANNEL_0` | 用 0 号通道 |
| 89 | `BREATH_RES` | `LEDC_TIMER_12_BIT` | **12 位 → 4096 级** |
| 90 | `BREATH_FREQ_HZ` | `5000` | **5 kHz**（周期 200 µs） |
| 91 | `BREATH_FULL` | `4095` | **满量程**（= 2¹²−1），占空比最大值 |
| 92 | `BREATH_PERIOD_MS` | `2500` | 一个呼吸周期 2.5 s |
| 93 | `BREATH_STEP_MS` | `10` | 每 10 ms 更新 → 250 级/周期 |

**为什么 5 kHz 能用 12 位**：LEDC 时钟 80 MHz，80 000 000 / 5000 = 16000 个计数，
log₂(16000) ≈ 13.9，即最高 13 位；取 12 位留余量。

### 3.10 复位原因转文字（第 98–112 行）

`esp_reset_reason()` 返回枚举，这里逐条映射成中文。
排查“为什么重启”时看这一行：`POWERON`=正常上电，`PANIC`=崩溃，`TASK_WDT`=任务卡死被看门狗复位。

### 3.11 自检报告（第 114–203 行）

统一用 `pass` / `total` 两个计数器（第 119 行），每检查一项就 `total++`，通过则 `pass++`，
最后第 201 行打印 `结论 : 5/5 项通过`。

| 行 | 检查项 | 用的 API | 判据 |
|---|---|---|---|
| 124–132 | 芯片型号/核数/主频/功能 | `esp_chip_info()`、`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 仅打印，不计分 |
| 135–143 | Flash 容量 | `esp_flash_get_size(NULL, &size)` | > 0 即通过 |
| 146–154 | PSRAM | `heap_caps_get_total_size(MALLOC_CAP_SPIRAM)` | > 0 通过；为 0 也只警告不失败 |
| 157–159 | 内部 RAM | `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` | 仅打印 |
| 162–170 | MAC 地址 | `esp_read_mac(mac, ESP_MAC_WIFI_STA)` | 返回 `ESP_OK` 通过 |
| 173 | 复位原因 | `esp_reset_reason()` | 仅打印 |
| 177–188 | **LED 回读** | `LED0(0)`→读 0；`LED0(1)`→读 1 | 两次都符合才通过 |
| 192–199 | 按键电平 | `gpio_get_level(BOOT_GPIO_PIN)` | 松开应为 1 |

**LED 回读为什么要延时 30 ms**（第 178、181 行）：写完 GPIO 到引脚真正翻转有延迟，
立刻读可能读到旧值；给 30 ms 让电平和电容稳定。

**为什么 PSRAM 为 0 也算通过**（第 153 行）：不是每块板都有 PSRAM，缺了不该判故障。

### 3.12 呼吸灯初始化（第 208–230 行）

```c
ledc_timer_config_t timer = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,  // 低速模式（所有芯片都支持）
    .duty_resolution = BREATH_RES,           // 12 位
    .timer_num       = BREATH_TIMER,         // TIMER_0
    .freq_hz         = BREATH_FREQ_HZ,       // 5 kHz
    .clk_cfg         = LEDC_AUTO_CLK,        // 自动选时钟源
};
ESP_ERROR_CHECK(ledc_timer_config(&timer));
```

- **定时器决定“频率 + 分辨率”**（一个定时器可带多个通道）；
- 第 219–227 行是**通道配置**：把通道绑到 GPIO1、选定时器、初始占空比 `4095`；
- 第 225 行注释 `duty 满 = 全灭` —— 因为 LED 低电平点亮，duty 大 → 高电平时间长 → 灯灭；
- 第 229 行 `ledc_stop(..., 1)`：停掉输出并让引脚空闲在高电平（灭），避免初始化瞬间闪一下。

### 3.13 设置亮度（第 232–238 行）★老师最关心的 6 行

```c
static void breath_set_brightness(float b)   /* b: 0.0（全灭）~ 1.0（全亮） */
{
    uint32_t duty = BREATH_FULL - (uint32_t)(b * (float)BREATH_FULL + 0.5f); // 235
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BREATH_CHANNEL, duty);                // 236
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BREATH_CHANNEL);                   // 237
}
```

| 行 | 讲解 |
|---|---|
| 235 | **取反**：`b=1`（全亮）→ duty=0；`b=0`（全灭）→ duty=4095。`+0.5f` 是**四舍五入**（浮点转整数默认截断，会少 1 级） |
| 236 | 把 duty 写入**影子寄存器**（此时还没生效） |
| 237 | 触发更新，让新占空比在下一个 PWM 周期生效 |

> 为什么要“先 set 再 update”：LEDC 是双缓冲的，`set` 只改影子值，
> 一次 `update` 才让所有通道同步切换，避免改到一半出现毛刺。

### 3.14 切换模式（第 240–265 行）

```c
s_mode = mode;                    // 242 先记状态
ledc_stop(..., 1);                // 245 无条件停 PWM（防残留）
led_init();                       // 246 引脚交回普通 GPIO，默认灭
switch (s_mode) { ... }           // 248 按新模式做初始化
```

- 第 245–246 行是**兜底**：不管从哪个模式切过来，先把 PWM 停掉、GPIO 复位，状态归零；
- `MODE_STEADY` 分支只做 `LED0(0)`（第 253 行）——点亮后就不需要再动；
- `MODE_BREATH` 分支重新 `breath_init()` 并把周期计数清 0（第 257–258 行），保证日志从 1 开始。

### 3.15 主函数（第 270–332 行）

```c
void app_main(void)
{
    led_init();               // 272  先初始化硬件
    key_init();               // 273
    board_self_check();       // 275  打印自检
    breath_init();            // 277  预配置 PWM
    set_mode(MODE_BLINK);     // 278  默认 1Hz 闪烁
```

- 第 280–281 行取两个时间戳（微秒 / 1000 = 毫秒）：`last_toggle_ms` 给闪烁用，
  `breath_start_ms` 给呼吸灯算相位；
- 第 283 行 `while (1)`：FreeRTOS 任务的标准写法，**永不返回**。

**主循环里三件事，顺序不能乱**：

| 行 | 顺序 | 为什么 |
|---|---|---|
| 285–292 | ① 先处理按键 | 模式变了要立刻生效，不能等这一轮的旧模式动作 |
| 294 | ② 取一次时间 `now_ms` | 一次循环只取一次，保证下面各分支用的是同一时刻 |
| 296–328 | ③ 按模式执行 | 闪烁 / 常亮 / 呼吸三个分支 |
| 330 | ④ 延时 10 ms | 让出 CPU 给其他任务 |

**按键分支细节**：

```c
led_mode_t next = (led_mode_t)((s_mode + 1) % MODE_NUM);  // 286 循环取模
s_press_cnt++;                                             // 287 计数
ESP_LOGI(... 第 %d 次按下（%lld ms）→ 模式：%s ...);        // 288-289 日志
set_mode(next);                                            // 290 执行切换
breath_start_ms = esp_timer_get_time() / 1000;              // 291 重新计时
```

- 第 286 行：`3 + 1 = 4`，`4 % 3 = 1` → 自动回到第一个模式，**加模式不用改这行**；
- 第 291 行：切到呼吸灯后必须把起点重置，否则相位会从上一次的旧值继续算。

**闪烁分支（297–302）**：

```c
if (now_ms - last_toggle_ms >= 500) {
    last_toggle_ms = now_ms;
    LED0_TOGGLE();
}
```

用“当前时间 − 上次翻转时间 ≥ 500”判断，**不是 `delay(500)`**——
这样按键响应不会被延时卡住。

**呼吸分支（307–324）★算法核心**：

```c
const int64_t elapsed  = now_ms - breath_start_ms;        // 308 进入呼吸模式后过了多久
const int64_t cycle_ms = elapsed % BREATH_PERIOD_MS;      // 309 在 2.5 s 周期内的位置
const float   phase    = (float)cycle_ms / BREATH_PERIOD_MS; // 310 归一化成 0~1
const float   b = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase); // 314 正弦曲线
breath_set_brightness(b);                                 // 315
```

第 314 行展开讲：

| phase | cos(2π·phase) | b = 0.5−0.5cos | 效果 |
|---|---|---|---|
| 0.0 | 1 | 0.0 | 全灭 |
| 0.25 | 0 | 0.5 | 半亮 |
| 0.5 | −1 | 1.0 | 全亮 |
| 0.75 | 0 | 0.5 | 半亮 |
| 1.0 | 1 | 0.0 | 全灭 |

用余弦而不是三角波：**两端导数为 0**，亮灭转换是“缓入缓出”，肉眼看不到拐点。

周期计数（318–322）：

```c
const int64_t idx = elapsed / BREATH_PERIOD_MS;
if (idx > s_breath_index) { s_breath_index = idx; ESP_LOGI("breath cycle = %d", idx); }
```

每跨过一个整周期才打印一次，不会每 10 ms 刷屏。

---

## 四、BSP 驱动讲解

### 4.1 `LED/led.h`

```c
#define LED0_GPIO_PIN    GPIO_NUM_1
#define LED0(x)          do { x ? gpio_set_level(LED0_GPIO_PIN, 1) : gpio_set_level(LED0_GPIO_PIN, 0); } while(0)
#define LED0_TOGGLE()    do { gpio_set_level(LED0_GPIO_PIN, !gpio_get_level(LED0_GPIO_PIN)); } while(0)
void led_init(void);
```

| 要点 | 说明 |
|---|---|
| `GPIO_NUM_1` | 板级事实：LED0 接 GPIO1（换板只改这一行） |
| `LED0(1)` = 灭 | **宏名是“LED0(值)”，值是电平不是开关**，1=高=灭、0=低=亮（课件特意强调的误区） |
| `do{...}while(0)` | 宏的标准写法：保证 `if (x) LED0(1); else ...` 这种用法不会展开出错 |
| `LED0_TOGGLE()` | `!gpio_get_level()` 取反——**依赖 GPIO 配成 `INPUT_OUTPUT` 才读得回电平** |

### 4.2 `LED/led.c`

```c
void led_init(void)
{
    gpio_config_t gpio_init_struct = {0};
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;         // 不用中断
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;         // 关键：能输出也能回读
    gpio_init_struct.pull_up_en = GPIO_PULLUP_DISABLE;      // 输出模式上下拉无效
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pin_bit_mask = 1ull << LED0_GPIO_PIN;  // 位掩码选中 GPIO1
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));
    LED0(1);                                                 // 上电默认熄灭
}
```

- `{0}` 把结构体清零，避免未初始化字段是随机值；
- `1ull << 1` = 0b10 = 选中 GPIO1；`ull` 是“无符号 long long”，32 位以上的引脚也要能移位；
- `ESP_ERROR_CHECK`：配置失败会打印文件名+行号并 abort，**比静默失败好排查**。

### 4.3 `KEY/key.h` 与 `key.c`

```c
#define BOOT_GPIO_PIN   GPIO_NUM_0
#define BOOT            gpio_get_level(BOOT_GPIO_PIN)
#define BOOT_PRES       1
uint8_t key_scan(uint8_t mode);
```

- `key_init()`：GPIO0 配成输入 + **上拉使能**（按键按下接地 → 读到 0）；
- `key_scan(0)`：非连按模式，按住不放只返回一次；
- **本期 main.c 没有调用 `key_scan`**，而是用自己实现的 `key_poll_press()`（第 60 行），
  原因见 3.7：`key_scan` 的阻塞去抖会丢短按。BSP 保持原样，留到第 04 课重写。

---

## 五、关键概念速查

| 概念 | 在这份代码里的位置 | 一句话 |
|---|---|---|
| PWM 频率 | `BREATH_FREQ_HZ = 5000` | 一秒钟 5000 个周期，每个 200 µs |
| 占空比 | `duty / 4096` | 高电平占整个周期的比例 |
| 分辨率 | `LEDC_TIMER_12_BIT` | 把 100% 分成 4096 份，每份 0.024% |
| 满量程 | `BREATH_FULL = 4095` | 12 位下占空比的最大值 |
| 低电平点亮 | `duty = FULL - b*FULL` | 亮度越高，高电平时间越短 |
| 非阻塞 | 全文无 `while` 空转 | 靠“时间戳比较 + vTaskDelay” |
| 时间基准 | `esp_timer_get_time()` | 微秒级单调时钟，不受系统时间影响 |
| 日志等级 | `ESP_LOGI/W/E` | I=正常、W=警告、E=错误，串口颜色不同 |

---

## 六、常见问题 → 代码位置

| 现象 | 先查这几行 |
|---|---|
| 灯不亮 | 272 行有没有 `led_init()`；`led.h` 的引脚是不是 `GPIO_NUM_1` |
| 灯常亮不闪 | 298 行阈值、300 行 `LED0_TOGGLE()`、`led.c` 是不是 `INPUT_OUTPUT` |
| 呼吸灯有台阶 | 89 行分辨率、93 行步长、314 行曲线 |
| 呼吸灯周期不对 | 92 行 `BREATH_PERIOD_MS`、309 行取模、291 行起点重置 |
| 按键没反应 | 62 行电平方向、67 行消抖次数、273 行 `key_init()` |
| 一次按下切两档 | 72 行只认“按下沿”，检查是否被改成了双边沿 |
| 上电就进呼吸灯 | 278 行 `set_mode(MODE_BLINK)` 有没有被改 |

---

## 七、与作业要求的对应

| 作业要求 | 代码位置 | 证据 |
|---|---|---|
| LEDC 5 kHz PWM 驱动 GPIO1 | 87–93 行宏、208–230 行初始化 | 串口日志“5 kHz PWM，12 位分辨率” |
| 占空比 0 → 最大 → 0 循环 | 235 行取反、314 行正弦 | 呼吸现象 + 视频 |
| 周期 2~3 s | 92 行 `2500` | 串口 `breath cycle` 时间戳间隔 2500 ms |
| 不用 for+delay 硬凑 | 全文用 `vTaskDelay(10)` + 时间戳 | 代码审查 |
| 每周期打印 `breath cycle = n` | 318–322 行 | 串口日志 |
| 占空比阶梯 ≥32 级 | 12 位 = 4096 级，250 级/周期 | 文档说明 + 截图 |

---

## 八、可以继续改进的地方

1. **`key_scan` 仍是阻塞版**：第 04 课会把 BSP 驱动也改成非阻塞，让驱动层与业务层一致；
2. **模式切换没有“长按”语义**：现在只有短按循环，可以加长按直接进呼吸灯；
3. **自检里的 PWM 回读**：本板 LEDC 接管引脚后 `gpio_get_level` 读不到（实测全 0），
   要做真实占空比测量需要把 GPIO1 跳线到 GPIO2 再用输入采样；
4. **`main.c` 已经 332 行**：进入第 05 课后建议把自检、呼吸灯各自拆成独立模块
   （`app/self_check.c`、`driver/led_pwm.c`），符合课程要求的分层。

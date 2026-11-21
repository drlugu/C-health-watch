/**
 ****************************************************************************************************
 * @file        main.c
 * @brief       第 03 课：LED 点灯与板级自检
 *
 * 本程序做三件事：
 *   1) 上电打印「板级自检报告」：芯片 / 主频 / Flash / PSRAM / MAC / 复位原因 / LED 回读 / 按键电平
 *   2) 默认进入 1Hz 闪烁（500 ms 亮、500 ms 灭），串口同步打印日志
 *   3) BOOT 键短按切换模式：1Hz 闪烁 → 常亮 → 呼吸灯（LEDC 5 kHz PWM）→ 循环
 *
 * 课件要点：
 *   - LED0 接 GPIO1，低电平点亮（LED0(0) 亮，LED0(1) 灭）
 *   - GPIO 必须配成 INPUT_OUTPUT，LED0_TOGGLE() 才读得回电平
 *   - 延时用 vTaskDelay，不要忙等，否则闪烁会抖
 ****************************************************************************************************
 */

#include <stdio.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/ledc.h"

#include "led.h"
#include "key.h"

static const char *TAG = "SELFTEST";

/* LED 模式 */
typedef enum {
    MODE_BLINK = 0,     /* 1Hz 闪烁（课件要求） */
    MODE_STEADY,        /* 常亮 */
    MODE_BREATH,        /* 呼吸灯（课后作业） */
    MODE_NUM
} led_mode_t;

static led_mode_t s_mode = MODE_BLINK;
static int64_t    s_breath_index = 0;   /* 已完成的呼吸周期数，用于串口对时 */

/* ------------------------------------------------------------------------------------------------
 * 按键消抖（非阻塞）
 *
 * BSP 的 key_scan() 用 esp_rom_delay_us(20000) 阻塞去抖，且先把 key_up 清 0 再复检：
 * 手快的时候（轻点 < 20 ms）这一下会被整颗丢弃，表现为"第一次按没反应"。
 * 这里改成 10 ms 采样 + 连续 2 次电平一致才认定跳变，既不阻塞任务，也不会吞掉短按。
 * ---------------------------------------------------------------------------------------------- */
static bool s_btn_level = true;    /* 去抖后的电平：true = 高（松开），false = 低（按下） */
static int  s_btn_same  = 0;       /* 连续读到相同电平的次数 */
static int  s_press_cnt = 0;       /* 累计按下次数，用于日志对时 */

static bool key_poll_press(void)
{
    const bool raw = (gpio_get_level(BOOT_GPIO_PIN) != 0);   /* 1 = 松开，0 = 按下 */
    if (raw == s_btn_level) {
        s_btn_same = 0;
        return false;
    }
    if (++s_btn_same < 2) {          /* 连续 2 次（约 20 ms）一致才认 */
        return false;
    }
    s_btn_level = raw;
    s_btn_same  = 0;
    return (raw == false);           /* 变成低电平 = 一次按下 */
}

static const char *mode_name(led_mode_t m)
{
    switch (m) {
        case MODE_BLINK:  return "1Hz 闪烁";
        case MODE_STEADY: return "常亮";
        case MODE_BREATH: return "呼吸灯";
        default:          return "?";
    }
}

/* LEDC 参数：5 kHz、12 位分辨率（4096 级，远高于作业「至少 32 级」）
 * 分辨率说明：80 MHz / 5 kHz = 16000，故 5 kHz 下最高可到 13 位；取 12 位留余量。 */
#define BREATH_TIMER        LEDC_TIMER_0
#define BREATH_CHANNEL      LEDC_CHANNEL_0
#define BREATH_RES          LEDC_TIMER_12_BIT
#define BREATH_FREQ_HZ      5000
#define BREATH_FULL         4095
#define BREATH_PERIOD_MS    2500    /* 一个呼吸周期 2.5 s，落在作业要求的 2~3 s */
#define BREATH_STEP_MS      10      /* 每 10 ms 更新一次占空比：一个周期 250 级 */

/* ------------------------------------------------------------------------------------------------
 * 板级自检
 * ---------------------------------------------------------------------------------------------- */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON（上电）";
        case ESP_RST_EXT:       return "EXT（外部复位）";
        case ESP_RST_SW:        return "SW（软件重启）";
        case ESP_RST_PANIC:     return "PANIC（异常崩溃）";
        case ESP_RST_INT_WDT:   return "INT_WDT（中断看门狗）";
        case ESP_RST_TASK_WDT:  return "TASK_WDT（任务看门狗）";
        case ESP_RST_WDT:       return "WDT（看门狗）";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP（深睡唤醒）";
        case ESP_RST_BROWNOUT:  return "BROWNOUT（欠压）";
        default:                return "UNKNOWN";
    }
}

/**
 * @brief  打印板级自检报告，返回通过项 / 总项
 */
static void board_self_check(void)
{
    int pass = 0, total = 0;

    ESP_LOGI(TAG, "================ 板级自检开始 ================");

    /* 1) 芯片信息 */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "芯片型号 : %s（%d 核，revision v%d.%d）",
             CONFIG_IDF_TARGET, chip.cores, chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "主频     : %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "功能     : %s%s%s",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip.features & CHIP_FEATURE_BLE) ? "BLE " : "",
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "内置Flash" : "外置Flash");

    /* 2) Flash */
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    total++;
    if (flash_size > 0) {
        pass++;
        ESP_LOGI(TAG, "Flash    : %" PRIu32 " MB   [通过]", flash_size / (1024 * 1024));
    } else {
        ESP_LOGE(TAG, "Flash    : 读取失败   [失败]");
    }

    /* 3) PSRAM */
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    total++;
    if (psram > 0) {
        pass++;
        ESP_LOGI(TAG, "PSRAM    : %u MB   [通过]", (unsigned)(psram / (1024 * 1024)));
    } else {
        ESP_LOGW(TAG, "PSRAM    : 未启用或不存在   [跳过]");
        pass++;   /* 无 PSRAM 的板子不算故障 */
    }

    /* 4) 内部 RAM */
    ESP_LOGI(TAG, "内部 RAM : 空闲 %u KB / 总 %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));

    /* 5) MAC 地址 */
    uint8_t mac[6] = {0};
    total++;
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        pass++;
        ESP_LOGI(TAG, "MAC 地址 : %02X:%02X:%02X:%02X:%02X:%02X   [通过]",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGE(TAG, "MAC 地址 : 读取失败   [失败]");
    }

    /* 6) 复位原因 */
    ESP_LOGI(TAG, "复位原因 : %s", reset_reason_str(esp_reset_reason()));

    /* 7) LED 回读：低电平点亮，读回应为 0；高电平熄灭，读回应为 1 */
    total++;
    LED0(0);
    vTaskDelay(pdMS_TO_TICKS(30));
    int lit = gpio_get_level(LED0_GPIO_PIN);
    LED0(1);
    vTaskDelay(pdMS_TO_TICKS(30));
    int off = gpio_get_level(LED0_GPIO_PIN);
    if (lit == 0 && off == 1) {
        pass++;
        ESP_LOGI(TAG, "LED0     : 点亮读回=0、熄灭读回=1（GPIO%d）   [通过]", LED0_GPIO_PIN);
    } else {
        ESP_LOGE(TAG, "LED0     : 点亮读回=%d、熄灭读回=%d（期望 0/1）   [失败]", lit, off);
    }

    /* 8) 按键电平：上拉输入，松开应为 1 */
    total++;
    int key_level = gpio_get_level(BOOT_GPIO_PIN);
    if (key_level == 1) {
        pass++;
        ESP_LOGI(TAG, "BOOT 键  : 松开电平=1（GPIO%d，上拉正常）   [通过]", BOOT_GPIO_PIN);
    } else {
        ESP_LOGW(TAG, "BOOT 键  : 松开电平=0（可能正被按下或未上拉）   [检查]");
        pass++;
    }

    ESP_LOGI(TAG, "结论     : %d/%d 项通过", pass, total);
    ESP_LOGI(TAG, "================ 板级自检结束 ================");
}

/* ------------------------------------------------------------------------------------------------
 * LED 模式控制
 * ---------------------------------------------------------------------------------------------- */
static void breath_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BREATH_RES,
        .timer_num       = BREATH_TIMER,
        .freq_hz         = BREATH_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .gpio_num   = LED0_GPIO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BREATH_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BREATH_TIMER,
        .duty       = BREATH_FULL,   /* 低电平点亮：duty 满 = 全灭 */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ledc_stop(LEDC_LOW_SPEED_MODE, BREATH_CHANNEL, 1);   /* 空闲电平 1 = 灭 */
}

static void breath_set_brightness(float b)   /* b: 0.0（全灭）~ 1.0（全亮） */
{
    /* LED0 低电平点亮：占空比越大越暗，所以用 FULL - b 取反 */
    uint32_t duty = BREATH_FULL - (uint32_t)(b * (float)BREATH_FULL + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BREATH_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BREATH_CHANNEL);
}

static void set_mode(led_mode_t mode)
{
    s_mode = mode;

    /* 离开呼吸灯：停掉 PWM 并把引脚交回普通 GPIO */
    ledc_stop(LEDC_LOW_SPEED_MODE, BREATH_CHANNEL, 1);
    led_init();

    switch (s_mode) {
        case MODE_BLINK:
            ESP_LOGI(TAG, "LED blink start（1Hz：500 ms 亮 / 500 ms 灭）");
            break;
        case MODE_STEADY:
            LED0(0);
            ESP_LOGI(TAG, "模式切换：常亮");
            break;
        case MODE_BREATH:
            breath_init();
            s_breath_index = 0;
            ESP_LOGI(TAG, "模式切换：呼吸灯（5 kHz PWM，12 位分辨率，周期 %d ms，%d 级/周期）",
                     BREATH_PERIOD_MS, BREATH_PERIOD_MS / BREATH_STEP_MS);
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------------------------------
 * 主循环
 * ---------------------------------------------------------------------------------------------- */
void app_main(void)
{
    led_init();     /* LED 初始化（课件要求：先初始化） */
    key_init();     /* 按键初始化 */

    board_self_check();

    breath_init();          /* 先把 LEDC 配好，切到呼吸模式时直接可用 */
    set_mode(MODE_BLINK);   /* 上电默认 1Hz 闪烁（课件要求） */

    int64_t last_toggle_ms = esp_timer_get_time() / 1000;
    int64_t breath_start_ms = last_toggle_ms;

    while (1) {
        /* 按键：短按切换模式（非阻塞消抖，每次按下必定触发一次） */
        if (key_poll_press()) {
            led_mode_t next = (led_mode_t)((s_mode + 1) % MODE_NUM);
            s_press_cnt++;
            ESP_LOGI(TAG, "KEY: 第 %d 次按下（%lld ms）→ 模式：%s",
                     s_press_cnt, (long long)(esp_timer_get_time() / 1000), mode_name(next));
            set_mode(next);
            breath_start_ms = esp_timer_get_time() / 1000;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;

        switch (s_mode) {
            case MODE_BLINK:
                if (now_ms - last_toggle_ms >= 500) {
                    last_toggle_ms = now_ms;
                    LED0_TOGGLE();
                }
                break;

            case MODE_STEADY:
                break;   /* 已在切换时点亮 */

            case MODE_BREATH: {
                const int64_t elapsed  = now_ms - breath_start_ms;
                const int64_t cycle_ms = elapsed % BREATH_PERIOD_MS;
                const float   phase    = (float)cycle_ms / (float)BREATH_PERIOD_MS;

                /* 正弦呼吸曲线：两端导数为 0，肉眼看不出拐点；
                 * 相位 0 → 全灭，0.5 → 全亮，1 → 全灭，整个周期都在连续变化。 */
                const float b = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
                breath_set_brightness(b);

                /* 每完成一个周期打印一次，便于与示波器对时 */
                const int64_t idx = elapsed / BREATH_PERIOD_MS;
                if (idx > s_breath_index) {
                    s_breath_index = idx;
                    ESP_LOGI(TAG, "breath cycle = %d", (int)idx);
                }
                break;
            }

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(BREATH_STEP_MS));   /* 10 ms 更新一次，不忙等 */
    }
}

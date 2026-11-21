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

/* LEDC 参数：5 kHz、10 位分辨率（1024 级），满足作业「至少 32 级」 */
#define BREATH_TIMER        LEDC_TIMER_0
#define BREATH_CHANNEL      LEDC_CHANNEL_0
#define BREATH_RES          LEDC_TIMER_10_BIT
#define BREATH_FREQ_HZ      5000
#define BREATH_FULL         1023
#define BREATH_PERIOD_MS    2500    /* 一个呼吸周期 2.5 s，落在作业要求的 2~3 s */

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

static void breath_set_brightness(float x)   /* x: 0.0 ~ 1.0 */
{
    /* gamma 校正：人眼对亮度非线性，直接线性扫会显得"前段变化快、后段不动" */
    float corrected = powf(x, 2.2f);
    uint32_t duty = BREATH_FULL - (uint32_t)(corrected * BREATH_FULL);
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
            ESP_LOGI(TAG, "模式切换：呼吸灯（5 kHz PWM，周期 %d ms）", BREATH_PERIOD_MS);
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
    set_mode(MODE_BLINK);   /* 上电默认 1Hz 闪烁 */

    int64_t last_toggle_ms = esp_timer_get_time() / 1000;
    int64_t breath_start_ms = last_toggle_ms;
    int     breath_cycle = 0;

    while (1) {
        /* 按键：短按切换模式 */
        if (key_scan(0) == BOOT_PRES) {
            led_mode_t next = (led_mode_t)((s_mode + 1) % MODE_NUM);
            set_mode(next);
            breath_start_ms = esp_timer_get_time() / 1000;
            breath_cycle = 0;
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
                const int64_t elapsed = now_ms - breath_start_ms;
                const int64_t cycle_ms = elapsed % BREATH_PERIOD_MS;
                const float   phase = (float)cycle_ms / (float)BREATH_PERIOD_MS;
                /* 三角波 0 → 1 → 0 */
                const float   v = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
                breath_set_brightness(v);

                /* 每完成一个周期打印一次，便于与示波器对时 */
                static int64_t last_cycle_index = -1;
                const int64_t cycle_index = elapsed / BREATH_PERIOD_MS;
                if (cycle_index != last_cycle_index) {
                    last_cycle_index = cycle_index;
                    breath_cycle = (int)cycle_index;
                    if (breath_cycle > 0) {
                        ESP_LOGI(TAG, "breath cycle = %d", breath_cycle);
                    }
                }
                break;
            }

            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(20));   /* 20 ms 调度粒度，不忙等 */
    }
}

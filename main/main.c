// main/main.c —— FoloToy AI Passport BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* -------- 考研助手 -------- */
#include "app_study/app_study.h"
#include "app_study/study_recorder.h"   /* is_recording / is_playing: 忙碌时不自动休眠 */

static const char *TAG = "main";

/* -------- 自动休眠(官方身份牌"无操作→深睡,任意键唤醒") -------- */
#define IDLE_SLEEP_MS      (3 * 60 * 1000)   /* 无操作 3 分钟进入深睡 */
#define SLEEP_WAKE_PIN     GPIO_NUM_0        /* 三键共用 ADC 引脚;按下任意键拉低→低电平唤醒 */
static int64_t s_last_activity_ms = 0;       /* 最近一次按键活动时刻(esp_timer, ms) */

static void touch_activity(void) { s_last_activity_ms = esp_timer_get_time() / 1000; }

/* 闲置监看：每秒检查,连续 3 分钟无按键且不在录音/回放 → 关背光 → 深睡。
 * 醒来是冷启动(esp_deep_sleep_start 不返回),app_main 会重新初始化并直接重进考研助手。 */
static void idle_sleep_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (study_recorder_is_recording() || study_recorder_is_playing()) continue; /* 忙碌不休眠 */
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_last_activity_ms < IDLE_SLEEP_MS) continue;
        ESP_LOGI(TAG, "已闲置 %lld ms,准备深睡(按任意键唤醒)",
                 (long long)(now - s_last_activity_ms));
        /* ① 把三键共用引脚从 ADC 切回数字输入(带上拉):空闲为高,按键拉低即可唤唤醒 */
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << SLEEP_WAKE_PIN,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        /* ② 先配好唤醒源并确认成功,再关背光/睡眠 —— 避免"配失败却睡死"或"黑屏空转"。
         *    注意第一参数是位掩码(1ULL<<0),不是引脚号; LOW = 任意键按下即唤醒 */
        esp_err_t we = esp_deep_sleep_enable_gpio_wakeup(1ULL << SLEEP_WAKE_PIN,
                                                         ESP_GPIO_WAKEUP_GPIO_LOW);
        if (we != ESP_OK) {
            ESP_LOGE(TAG, "唤醒配置失败(%s),本次不休眠,3 分钟后再试", esp_err_to_name(we));
            s_last_activity_ms = esp_timer_get_time() / 1000;
            continue;
        }
        bsp_display_backlight(0);
        /* ③ 深睡前把当前时间写进 NVS:否则唤醒(尤其离线断网)后会退回"烧录时刻" */
        app_study_persist_time();
        ESP_LOGI(TAG, "进入深睡:按任意键(GPIO0 低电平)唤醒");
        /* ④ 深睡:该函数声明为 void __noreturn__,调用后不再返回,其后代码不可达 */
        esp_deep_sleep_start();
    }
}

static const demo_entry_t DEMOS[] = {
    { "Study",   app_study_enter,    app_study_exit,    app_study_key    },  /* 考研助手（默认第一项） */
    { "Display", demo_display_enter, demo_display_exit, demo_display_key },
    { "Button",  demo_button_enter,  demo_button_exit,  demo_button_key  },
    { "Audio",   demo_audio_enter,   demo_audio_exit,   demo_audio_key   },
    { "Battery", demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "Wi-Fi",   demo_wifi_enter,    demo_wifi_exit,    demo_wifi_key    },
    { "BLE",     demo_ble_enter,     demo_ble_exit,     demo_ble_key     },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

// 各外设初始化结果:失败的项在菜单里标 [FAIL] 且不允许进入。
static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static int  s_sel;                 // 当前选中项
static int  s_active = -1;         // 当前所在演示页;-1 = 在菜单

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name,
                              s_ok[i] ? "" : "  [FAIL]");
        bool sel = ((int)i == s_sel);
        if (sel) {
            lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(0x4C7DFF), 0);
            lv_obj_set_style_border_color(s_cards[i], lv_color_hex(0x3569E8), 0);
            lv_obj_set_style_text_color(s_rows[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_color(s_cards[i], lv_color_hex(0xE3EAF4), 0);
            lv_obj_set_style_text_color(s_rows[i],
                s_ok[i] ? lv_color_hex(0x22314D) : lv_color_hex(0x7A2020), 0);
        }
    }
}

static lv_obj_t *menu_card(lv_obj_t *parent, int x, int y) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, 102, 40);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0xE3EAF4), 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    return c;
}

static void menu_build(void) {
    s_menu_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_menu_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_menu_scr, lv_color_hex(0xEFF3FA), 0);
    lv_obj_set_style_border_width(s_menu_scr, 0, 0);
    lv_obj_set_style_pad_all(s_menu_scr, 0, 0);

    /* 顶部标题栏（现代扁平卡片） */
    lv_obj_t *head = lv_obj_create(s_menu_scr);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(head, 224, 46); lv_obj_set_pos(head, 8, 8);
    lv_obj_set_style_radius(head, 14, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_t *ht = lv_label_create(head);
    lv_label_set_text(ht, "AI Passport");
    lv_obj_set_style_text_font(ht, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ht, lv_color_hex(0x22314D), 0);
    lv_obj_set_pos(ht, 16, 10);

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int x = 11 + (int)(i % 2) * 112;
        int y = 64 + (int)(i / 2) * 52;
        s_cards[i] = menu_card(s_menu_scr, x, y);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    menu_build();
}

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    touch_activity();   /* 任意按键都视为用户活动,重置 3 分钟记时 */
    if (!bsp_lvgl_lock(100)) return;   /* 超时从 500ms→100ms，避免按键被 LVGL 刷新阻塞 */

    if (s_active >= 0) {
        /* 非 Study 演示：长按 OK 统一返回目录；
         * Study 内部的长按返回由 app_study_key 处理，仅当它在封面页长按时
         * 才通过 app_study_wants_exit() 请求完全退出回目录。 */
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG && s_active != 0) {
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
            if (s_active == 0 && app_study_wants_exit()) {
                DEMOS[s_active].exit();
                enter_menu();
            }
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)   { s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % DEMO_COUNT;              menu_refresh(); }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            DEMOS[s_active].enter();
        }
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport BSP demo 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本 demo 的 UI 载体,失败就没有菜单可言 —— 打清楚日志后退出,
    // 不做"串口菜单"降级(那会让本文件复杂一倍,违背参考示例的初衷)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,demo 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 其余外设单项失败不阻塞:菜单里标 [FAIL],其他项照常可测。
    s_ok[0] = true;                                   // Study:子应用在 enter 时自初始化
    s_ok[1] = true;                                   // Display 已确认可用
    s_ok[2] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[3] = (bsp_audio_init() == ESP_OK);
    s_ok[4] = (bsp_battery_init() == ESP_OK);
    s_ok[5] = true;                                    // 页面内按需初始化并显示错误
    s_ok[6] = true;

    if (bsp_lvgl_lock(1000)) {
        /* 开机直接进入考研助手（Study 为第一个菜单项），
         * 不再显示官方像素机器狗菜单。长按 OK 可回到上方现代目录。 */
        s_active = 0;
        touch_activity();                          // 初始化活动时钟,避免开机立刻进休眠
        DEMOS[0].enter();
        bsp_lvgl_unlock();
    }

    /* 启动闲置监测任务:无操作 3 分钟自动深睡,按任意键(GPIO0 低电平)唤醒 */
    xTaskCreate(idle_sleep_task, "idle_sleep", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "就绪:Display=%d Button=%d Audio=%d Battery=%d",
             s_ok[0], s_ok[1], s_ok[2], s_ok[3]);
}

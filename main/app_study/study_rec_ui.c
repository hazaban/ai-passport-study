/*
 * study_rec_ui.c — 录音机子页 UI（device-only）
 *
 * 由设置页「录音笔」进入。固定黑底白字，不随日夜主题。
 * 列表布局：顶部【开始录音】→ 录音记录列表 → 底部两钮【退出录音 / 返回主页面】。
 * 光标：0=开始录音；1..n=第 i 条录音；n+1=退出录音；n+2=返回主页面。
 * 按键：上下=移动；短按OK：开始录音(光标在底部钮上则退出/回主页)；
 *       长按OK=播放选中；长按上=删除选中；长按下=Wi-Fi导出。
 * 录音中：OK=停止并保存。播放中：上下=音量，OK=停止。
 */
#include "study_ui.h"
#include "study_recorder.h"
#include "study_wifi.h"
#include "study_font.h"
#include "ui_pixel.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define F_STUDY  (&study_font)

#define REC_BG     0x000000
#define REC_INK    0xFFFFFF
#define REC_MUTED  0x9A9A9A
#define REC_ACC    0xE43B2F
#define REC_OK     0x2FBF71
#define REC_GRAY   0x2A2A2A
#define REC_BTN    0x1F1F1F

#define VIS_ROWS   5

typedef enum { SUB_LIST = 0, SUB_REC, SUB_PLAY, SUB_CONFIRM, SUB_EXPORT } rec_sub_t;

static lv_obj_t      *s_scr = NULL;
static rec_sub_t      s_sub = SUB_LIST;
static lv_timer_t    *s_timer = NULL;
static bool           s_wants_back = false;   /* 回设置页 */
static bool           s_wants_home = false;   /* 回封面主页面 */

/* 光标:0=开始, 1..n=录音, n+1=退出, n+2=返回主页面 */
static int s_cursor = 0;
static study_rec_entry_t s_items[REC_MAX_FILES];
static int s_item_n = 0;
static lv_obj_t *s_list_cont = NULL;
static lv_obj_t *s_btn_start = NULL;
static lv_obj_t *s_btn_exit  = NULL;
static lv_obj_t *s_btn_home  = NULL;
static uint32_t s_item_seq = 0;

static lv_obj_t *s_rec_time_lbl = NULL;
static lv_obj_t *s_play_vol_lbl = NULL;

static void sub_show(rec_sub_t sub);

static int cur_item(void) {           /* 光标所指录音下标，-1=非录音 */
    int idx = s_cursor - 1;
    return (idx >= 0 && idx < s_item_n) ? idx : -1;
}
static int exit_pos(void) { return s_item_n + 1; }
static int home_pos(void) { return s_item_n + 2; }

static lv_obj_t *mk_screen(void) {
    lv_obj_t *o = lv_obj_create(NULL);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(REC_BG), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_screen_load(o);
    return o;
}
static lv_obj_t *txt(lv_obj_t *parent, const char *s, uint32_t color, int x, int y) {
    lv_obj_t *l = ui_pixel_label(parent, s, F_STUDY, color);
    lv_obj_set_pos(l, x, y);
    return l;
}
static void topbar(lv_obj_t *scr, const char *title) {
    txt(scr, title, REC_INK, 8, 6);
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        char b[16];
        snprintf(b, sizeof(b), "%d%%", soc);
        lv_obj_t *l = txt(scr, b, REC_MUTED, 0, 8);
        lv_obj_set_align(l, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_pos(l, -12, 0);
    }
}
static void mmss(char *out, size_t cap, uint32_t ms) {
    snprintf(out, cap, "%02lu:%02lu", (unsigned long)(ms / 60000),
             (unsigned long)((ms / 1000) % 60));
}
static void rec_label(uint32_t epoch, uint32_t seq, char *out, size_t cap) {
    if (epoch > 0) {
        time_t t = (time_t)epoch;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(out, cap, "%02d/%02d %02d:%02d #%u",
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, (unsigned)seq);
    } else {
        snprintf(out, cap, "录音 #%u", (unsigned)seq);
    }
}
static void hint_line(lv_obj_t *scr, const char *h1, const char *h2, const char *h3) {
    txt(scr, h1, REC_MUTED, 8, 226);
    txt(scr, h2, REC_MUTED, 8, 248);
    txt(scr, h3, REC_MUTED, 8, 270);
}
static lv_obj_t *mk_btn(lv_obj_t *parent, int x, int y, int w, int h, const char *s,
                        uint32_t bg, uint32_t fg) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_t *lb = ui_pixel_label(b, s, F_STUDY, fg);
    lv_obj_center(lb);
    return b;
}

/* ---------------- LIST ---------------- */
static void list_refresh(void) {
    if (!s_scr) return;
    s_item_n = study_recorder_scan(s_items, REC_MAX_FILES);
    if (s_item_n > VIS_ROWS) s_item_n = VIS_ROWS;
    int maxp = home_pos();
    if (s_cursor > maxp) s_cursor = maxp;

    if (s_btn_start) lv_obj_set_style_bg_color(s_btn_start, lv_color_hex(s_cursor == 0 ? REC_ACC : REC_BTN), 0);
    if (s_btn_exit)  lv_obj_set_style_bg_color(s_btn_exit,  lv_color_hex(s_cursor == exit_pos() ? REC_GRAY : REC_BTN), 0);
    if (s_btn_home)  lv_obj_set_style_bg_color(s_btn_home,  lv_color_hex(s_cursor == home_pos() ? REC_GRAY : REC_BTN), 0);

    if (!s_list_cont) return;
    lv_obj_clean(s_list_cont);
    if (s_item_n == 0) {
        txt(s_list_cont, "暂无录音 · OK 开始", REC_MUTED, 8, 6);
        return;
    }
    for (int i = 0; i < s_item_n; i++) {
        char line[64], t1[40], mm[16];
        rec_label(s_items[i].created_epoch, s_items[i].seq, t1, sizeof(t1));
        mmss(mm, sizeof(mm), s_items[i].ms_len);
        snprintf(line, sizeof(line), "%s  %s", t1, mm);
        bool sel = (cur_item() == i);
        if (sel) {
            lv_obj_t *hl = lv_obj_create(s_list_cont);
            lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(hl, 0, i * 24);
            lv_obj_set_size(hl, 224, 22);
            lv_obj_set_style_radius(hl, 6, 0);
            lv_obj_set_style_bg_color(hl, lv_color_hex(REC_GRAY), 0);
            lv_obj_set_style_border_width(hl, 0, 0);
        }
        txt(s_list_cont, line, sel ? REC_INK : REC_MUTED, 6, 3 + i * 24);
    }
}

static void list_build(void) {
    s_cursor = 0;
    s_scr = mk_screen();
    topbar(s_scr, "录音笔");

    s_btn_start = mk_btn(s_scr, 10, 30, 220, 30, "开始录音", REC_ACC, 0xFFFFFF);

    s_list_cont = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_list_cont, 8, 66);
    lv_obj_set_size(s_list_cont, 224, 116);
    lv_obj_set_style_bg_opa(s_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_list_cont, 0, 0);

    s_btn_exit = mk_btn(s_scr, 10, 186, 106, 32, "退出录音", REC_BTN, REC_MUTED);
    s_btn_home = mk_btn(s_scr, 124, 186, 106, 32, "返回主页面", REC_BTN, REC_MUTED);

    list_refresh();
    hint_line(s_scr, "上下:选择 短按OK:开始录音", "长按OK:播放 长按上:删除", "长按下:Wi-Fi导出");
}

/* ---------------- REC / PLAY / CONFIRM / EXPORT ---------------- */
static void rec_build(void) {
    s_scr = mk_screen();
    topbar(s_scr, "正在录音");
    lv_obj_t *dot = lv_obj_create(s_scr);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dot, 14, 60);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(REC_ACC), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    txt(s_scr, "录制中", REC_ACC, 34, 56);
    s_rec_time_lbl = txt(s_scr, "00:00 / 20:00", REC_INK, 8, 96);
    lv_obj_set_style_text_align(s_rec_time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    hint_line(s_scr, "短按OK:停止并保存", "", "");
}
static void rec_update(void) {
    if (s_sub != SUB_REC || !s_rec_time_lbl) return;
    char mm[16], el[16];
    mmss(el, sizeof(el), study_recorder_elapsed_ms());
    mmss(mm, sizeof(mm), (uint32_t)REC_MAX_SEC * 1000);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s / %s", el, mm);
    lv_label_set_text(s_rec_time_lbl, buf);
}
static void play_build(uint32_t seq) {
    s_item_seq = seq;
    s_scr = mk_screen();
    topbar(s_scr, "播放中");
    char line[40];
    snprintf(line, sizeof(line), "录音 #%lu", (unsigned long)seq);
    txt(s_scr, line, REC_INK, 8, 60);
    s_play_vol_lbl = txt(s_scr, "", REC_MUTED, 8, 88);
    lv_label_set_text(s_play_vol_lbl, "音量 80%");
    hint_line(s_scr, "上下:音量  OK:停止", "长按OK:停止并返回", "");
}
static void play_vol_update(void) {
    if (s_play_vol_lbl) {
        char b[16];
        snprintf(b, sizeof(b), "音量 %d%%", study_recorder_volume());
        lv_label_set_text(s_play_vol_lbl, b);
    }
}
static void confirm_build(uint32_t seq) {
    s_item_seq = seq;
    s_scr = mk_screen();
    topbar(s_scr, "删除确认");
    char line[40];
    snprintf(line, sizeof(line), "录音 #%lu 将删除", (unsigned long)seq);
    txt(s_scr, line, REC_INK, 8, 70);
    txt(s_scr, "短按OK:删除  上下:取消", REC_MUTED, 8, 100);
}
static void export_build(void) {
    s_scr = mk_screen();
    topbar(s_scr, "Wi-Fi导出");
    txt(s_scr, "手机连热点:", REC_MUTED, 8, 48);
    const char *ap = study_wifi_get_ap_ssid();
    txt(s_scr, ap ? ap : "STU_STUDY_xxxx", REC_INK, 12, 68);
    txt(s_scr, "密码:", REC_MUTED, 8, 96);
    txt(s_scr, STUDY_WIFI_AP_PASS, REC_INK, 12, 116);
    txt(s_scr, "打开网址:", REC_MUTED, 8, 144);
    txt(s_scr, "http://192.168.4.1/rec/list", REC_OK, 12, 164);
    hint_line(s_scr, "长按下:退出导出", "浏览器播放/下载 WAV", "");
}

static void sub_show(rec_sub_t sub) {
    if (s_scr) lv_obj_delete(s_scr);
    s_scr = NULL;
    s_list_cont = NULL; s_btn_start = NULL; s_btn_exit = NULL; s_btn_home = NULL;
    s_rec_time_lbl = NULL; s_play_vol_lbl = NULL;
    s_sub = sub;
    switch (sub) {
        case SUB_LIST:    list_build(); break;
        case SUB_REC:     rec_build(); rec_update(); break;
        case SUB_PLAY:    play_build(s_item_seq); play_vol_update(); break;
        case SUB_CONFIRM: confirm_build(s_item_seq); break;
        case SUB_EXPORT:  export_build(); break;
    }
}

/* ---------------- 按键 ---------------- */
void ui_rec_key(uint8_t btn_u, uint8_t ev_u) {
    bsp_btn_t btn = (bsp_btn_t)btn_u;
    bsp_btn_ev_t ev = (bsp_btn_ev_t)ev_u;
    int maxp = home_pos();

    switch (s_sub) {
        case SUB_LIST: {
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
                if (s_cursor < maxp) s_cursor++;
                list_refresh();
            } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
                if (s_cursor > 0) s_cursor--;
                list_refresh();
            } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                if (s_cursor == exit_pos()) { s_wants_back = true; }
                else if (s_cursor == home_pos()) { s_wants_home = true; }
                else if (study_recorder_start() == 0) sub_show(SUB_REC);
            } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                int i = cur_item();
                if (i >= 0) { s_item_seq = s_items[i].seq;
                              if (study_recorder_play_seq(s_items[i].seq) == 0) sub_show(SUB_PLAY); }
            } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {
                int i = cur_item();
                if (i >= 0) { s_item_seq = s_items[i].seq; sub_show(SUB_CONFIRM); }
            } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
                sub_show(SUB_EXPORT);
            }
            break;
        }
        case SUB_REC: {
            if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG))
                study_recorder_stop();
            break;
        }
        case SUB_PLAY: {
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
                study_recorder_set_volume(study_recorder_volume() + 10); play_vol_update();
            } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
                study_recorder_set_volume(study_recorder_volume() - 10); play_vol_update();
            } else if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
                study_recorder_stop_playback(); sub_show(SUB_LIST);
            }
            break;
        }
        case SUB_CONFIRM: {
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
                study_recorder_delete_seq(s_item_seq); sub_show(SUB_LIST);
            } else if ((ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) ||
                       ev == BSP_BTN_LONG) {
                sub_show(SUB_LIST);
            }
            break;
        }
        case SUB_EXPORT: {
            if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) sub_show(SUB_LIST);
            break;
        }
    }
}

/* ---------------- 周期刷新 ---------------- */
static void poll_events(void) {
    study_rec_evt_t e;
    while ((e = study_recorder_poll_evt()) != REC_EVT_NONE) {
        switch (e) {
            case REC_EVT_REC_STARTED:    break;
            case REC_EVT_MAX_TIME:
            case REC_EVT_STORAGE_FULL:
            case REC_EVT_REC_CANCELLED:
            case REC_EVT_REC_ERR:
            case REC_EVT_AUDIO_ERR:
            case REC_EVT_REC_SAVED:
                if (s_sub == SUB_REC) sub_show(SUB_LIST);
                break;
            case REC_EVT_PLAY_DONE:
            case REC_EVT_PLAY_ERR:
                if (s_sub == SUB_PLAY) sub_show(SUB_LIST);
                break;
            case REC_EVT_DEL_DONE:
            case REC_EVT_DEL_ERR:
                if (s_sub == SUB_LIST) list_refresh();
                break;
            default: break;
        }
    }
}
static void rec_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_sub == SUB_REC) rec_update();
    if (s_sub == SUB_PLAY) play_vol_update();
    poll_events();
}

/* ---------------- 对外接口 ---------------- */
void ui_rec_enter(void) {
    s_wants_back = false;
    s_wants_home = false;
    s_cursor = 0;
    if (study_recorder_active()) {
        study_recorder_stop_playback();
        study_recorder_cancel();
    }
    study_recorder_set_volume(80);
    if (!s_timer) s_timer = lv_timer_create(rec_timer_cb, 250, NULL);
    sub_show(SUB_LIST);
}
void ui_rec_destroy(void) {
    study_recorder_stop_playback();
    study_recorder_cancel();
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_scr)   { lv_obj_delete(s_scr); s_scr = NULL; }
    s_list_cont = NULL; s_btn_start = NULL; s_btn_exit = NULL; s_btn_home = NULL;
    s_rec_time_lbl = NULL; s_play_vol_lbl = NULL;
    s_wants_back = false;
    s_wants_home = false;
}
bool ui_rec_wants_back(void) { return s_wants_back; }
bool ui_rec_wants_home(void) { return s_wants_home; }

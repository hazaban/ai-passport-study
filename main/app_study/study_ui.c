/*
 * study_ui.c — 考研助手全部 UI 页面（竖屏 240×320）。
 *
 * 现代扁平 App 风格：圆角卡片、柔和阴影、品牌色强调，不再使用机器狗/
 * 像素草地背景。中文统一使用内嵌的 study_font（ZCOOL KuaiLe 子集）。
 * 页面逻辑与旧版一致，仅重构了视觉层。
 */
#include "study_ui.h"
#include "study_task.h"
#include "study_group.h"
#include "study_category.h"
#include "study_wifi.h"
#include "study_time.h"
#include "study_scheduler.h"
#include "study_font.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef ESP_PLATFORM

#include "lvgl.h"
#include "ui_pixel.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"

/* ==============================================================
 * 现代扁平设计令牌
 * ============================================================== */
#define W       240
#define H       320

#define C_BG        0x111A24   /* 页面背景（柔和深蓝灰，护眼） */
#define C_CARD      0x1D2834   /* 卡片（深） */
#define C_INK       0xEDF3F8   /* 主文字（浅） */
#define C_MUTED     0x9AA8B8   /* 次要文字 */
#define C_PRIMARY   0x6FA8FF   /* 柔和蓝 */
#define C_PRIMARY_D 0x4E86E0
#define C_LINE      0x2B3948   /* 分隔线/描边 */
#define C_ACCENT    0xFFB23E   /* 强调橙 */

/* 会话字体（整个 Study UI 统一用一个中文+ASCII 字体） */
#define F_STUDY  (&study_font)

/* ==============================================================
 * 共用对象（跨页共享）
 * ============================================================== */
static const study_ui_callbacks_t *s_cb = NULL;
static lv_obj_t *s_cur_scr = NULL;

void study_ui_init(const study_ui_callbacks_t *cb) {
    s_cb = cb;
}

/* ==============================================================
 * 现代控件小工具
 * ============================================================== */

/* 圆角卡片：可带投影与可选描边 */
static lv_obj_t *mod_card(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t bg, int radius, bool shadow) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    if (shadow) {
        /* 扁平风格：不启用阴影（LVGL9 软渲染阴影吃内存/CPU，ESP32-C3 上易卡顿/复位） */
        (void)0;
    }
    return o;
}

/* 圆角实心/描边按钮（水平居中一个 label） */
static lv_obj_t *mod_button(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t bg, uint32_t fg, bool filled) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(o, filled ? 0 : 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(fg), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static void mod_button_label(lv_obj_t *btn, const char *text, uint32_t color) {
    lv_obj_t *l = ui_pixel_label(btn, text, F_STUDY, color);
    lv_obj_center(l);
}

/* 普通文本标签（自动换行可选） */
static lv_obj_t *mod_label(lv_obj_t *parent, const char *text,
                           uint32_t color, bool wrap, int w) {
    lv_obj_t *l = ui_pixel_label(parent, text, F_STUDY, color);
    if (wrap) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

/* ==============================================================
 * PAGE_HOME — 封面主页面
 *
 * 顶部：应用名 + 日期
 * 中部：考研倒计时大数字
 * 底部：[进入学习] [设置] 两个现代大按钮
 * ============================================================== */
static lv_obj_t *s_home_scr;
static lv_obj_t *s_home_cnt;
static lv_obj_t *s_home_date;        /* 首页日期(含星期/WiFi图标) */
static lv_obj_t *s_home_clock;       /* 首页当前时间(秒级走表) */
static lv_timer_t *s_home_timer;     /* 每秒刷新日期与时间 */
static lv_obj_t *s_home_btn[2];      /* 0=进入学习 1=设置 */
static int s_home_sel;
static bool s_home_wants_study;
static bool s_home_wants_settings;

/* 首页柔和浅色令牌（浅色但更柔和：近白卡片 + 淡描边 + 低饱和柔和蓝，不晃眼） */
#define HBG     0xF1F5FB   /* 页面底：柔和浅蓝灰 */
#define HCARD   0xFFFFFF   /* 卡片近白，加淡描边 */
#define HCARD2  0xEAF1FB   /* 倒计时卡：柔和浅蓝 */
#define HINK    0x35435E   /* 主文字：柔和藏青 */
#define HMUTED  0x93A1B5   /* 次要文字 */
#define HB      0x7A9BCC   /* 主蓝：低饱和、柔和 */
#define HB_D    0x6390C4   /* 主蓝加深(选中态) */
#define HACC    0xE8923C   /* 强调橙(柔和) */
#define HBORD   0xDFE7F1   /* 卡片描边 */
#define HBAT_HI 0x53A66A   /* 电量高：柔和绿 */
#define HBAT_MD 0xD9A13B   /* 电量中：柔和橙 */
#define HBAT_LO 0xD96A5A   /* 电量低：柔和红 */

static lv_obj_t *home_card(int y, int h, uint32_t bg) {
    lv_obj_t *c = lv_obj_create(s_home_scr);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, 8, y);
    lv_obj_set_size(c, 224, h);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(HBORD), 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    return c;
}

/* 首页两枚入口按钮：进入学习=实心，设置=描边(ghost)；选中态统一变深蓝 */
static lv_obj_t *home_big_button(int x, int y, int w, int h, bool primary) {
    lv_obj_t *b = lv_obj_create(s_home_scr);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (primary) {  /* 进入学习：柔和实心蓝 */
        lv_obj_set_style_bg_color(b, lv_color_hex(HB), 0);
        lv_obj_set_style_border_width(b, 0, 0);
    } else {        /* 设置：柔和中性(无蓝描边) */
        lv_obj_set_style_bg_color(b, lv_color_hex(0xEDF1F7), 0);
        lv_obj_set_style_border_width(b, 0, 0);
    }
    return b;
}

static void home_refresh_buttons(void) {
    for (int i = 0; i < 2; i++) {
        if (!s_home_btn[i]) continue;
        bool sel = (s_home_sel == i);
        if (i == 0) {  /* 进入学习：实心蓝 */
            lv_obj_set_style_bg_color(s_home_btn[i],
                lv_color_hex(sel ? HB_D : HB), 0);
        } else {       /* 设置：柔和中性 ⇄ 实心蓝(选中) */
            lv_obj_set_style_bg_color(s_home_btn[i],
                lv_color_hex(sel ? HB : 0xEDF1F7), 0);
        }
        lv_obj_t *lab = lv_obj_get_child(s_home_btn[i], 0);
        if (lab) lv_obj_set_style_text_color(lab,
            lv_color_hex((i == 0 || sel) ? 0xFFFFFF : HINK), 0);
    }
}

/* 首页日期+时间每秒刷新：走表可见；联网后时间变绿、自动变成真实北京时间 */
static void home_time_refresh(void) {
    struct tm tv;
    bool ok = study_time_civil_tm(&tv);
    bool wi = (study_wifi_get_state() == WIFI_STATE_CONNECTED);
    if (s_home_date) {
        char dbuf[36];
        if (ok) {
            snprintf(dbuf, sizeof(dbuf), "%d年%d月%d日 %d",
                     tv.tm_year + 1900, tv.tm_mon + 1, tv.tm_mday, tv.tm_wday);
        } else {
            snprintf(dbuf, sizeof(dbuf), "请设时间");
        }
        lv_label_set_text(s_home_date, dbuf);
    }
    if (s_home_clock && ok) {
        char cb[16];
        snprintf(cb, sizeof(cb), "%02d:%02d:%02d", tv.tm_hour, tv.tm_min, tv.tm_sec);
        lv_label_set_text(s_home_clock, cb);
        lv_obj_set_style_text_color(s_home_clock,
            lv_color_hex(wi ? HBAT_HI : HMUTED), 0);
    }
}
static void home_timer_cb(lv_timer_t *t) { (void)t; home_time_refresh(); }

void ui_home_build(void) {
    s_home_sel = 0;
    s_home_wants_study = false;
    s_home_wants_settings = false;

    s_home_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(0xEFF3FA), 0);   /* 首页亮色原配色 */
    lv_obj_set_style_border_width(s_home_scr, 0, 0);
    lv_obj_set_style_pad_all(s_home_scr, 0, 0);
    s_cur_scr = s_home_scr;

    /* 顶部卡：第一行 = 应用名 + 右侧电池(进度条式填充)；第二行 = 年月日周几 + 时分秒 */
    lv_obj_t *head = home_card(8, 70, 0xFFFFFF);

    lv_obj_t *t = ui_pixel_label(head, "考研日程助手", F_STUDY, HINK);
    lv_obj_set_pos(t, 16, 16);

    /* 电池：横向进度条式填充（无数字/无竖条），低电红、中电橙、满电/高电绿 */
    {
        int soc = bsp_battery_soc();
        if (soc >= 0) {
            uint32_t col = (soc <= 20) ? HBAT_LO : ((soc <= 40) ? HBAT_MD : HBAT_HI);
            lv_obj_t *body = lv_obj_create(head);
            lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(body, 42, 18);
            lv_obj_set_style_radius(body, 4, 0);
            lv_obj_set_style_bg_color(body, lv_color_hex(0xF6F9FD), 0);
            lv_obj_set_style_border_width(body, 1, 0);
            lv_obj_set_style_border_color(body, lv_color_hex(0xB9C6D6), 0);
            lv_obj_set_style_pad_all(body, 0, 0);
            lv_obj_set_align(body, LV_ALIGN_TOP_RIGHT);
            lv_obj_set_pos(body, -14, 16);
            /* 极帽(右侧小突出) */
            lv_obj_t *nub = lv_obj_create(body);
            lv_obj_remove_flag(nub, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(nub, 4, 8);
            lv_obj_set_pos(nub, 40, 5);
            lv_obj_set_style_radius(nub, 2, 0);
            lv_obj_set_style_border_width(nub, 0, 0);
            lv_obj_set_style_bg_color(nub, lv_color_hex(0xB9C6D6), 0);
            /* 内部进度填充 */
            lv_obj_t *fill = lv_obj_create(body);
            lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
            int fw = (soc > 100) ? 38 : (38 * soc / 100);
            if (fw < 2) fw = 2;
            lv_obj_set_size(fill, fw, 12);
            lv_obj_set_pos(fill, 2, 3);
            lv_obj_set_style_radius(fill, 2, 0);
            lv_obj_set_style_border_width(fill, 0, 0);
            lv_obj_set_style_bg_color(fill, lv_color_hex(col), 0);
        }
    }

    /* 第二行：日期(左) + 秒级时钟(右) */
    s_home_date = ui_pixel_label(head, "", F_STUDY, HMUTED);
    lv_obj_set_pos(s_home_date, 16, 47);

    s_home_clock = ui_pixel_label(head, "", &lv_font_montserrat_14, HMUTED);
    lv_obj_set_align(s_home_clock, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(s_home_clock, -14, 48);

    if (!s_home_timer) s_home_timer = lv_timer_create(home_timer_cb, 1000, NULL);
    home_time_refresh();

    /* 倒计时卡(柔和浅蓝)：标题 + 大数字 + 单位 + 考试日期 */
    lv_obj_t *cnt = home_card(82, 138, HCARD2);
    lv_obj_t *cap = ui_pixel_label(cnt, "考研倒计时", F_STUDY, HMUTED);
    lv_obj_set_align(cap, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(cap, 0, 16);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    int left = study_time_days_until(STUDY_EXAM_MONTH, STUDY_EXAM_DAY);
    char nbuf[16];
    if (left < 0) snprintf(nbuf, sizeof(nbuf), "GO");
    else          snprintf(nbuf, sizeof(nbuf), "%d", left);
    s_home_cnt = ui_pixel_label(cnt, nbuf, &lv_font_montserrat_20, HINK);
    lv_obj_set_align(s_home_cnt, LV_ALIGN_CENTER);
    lv_obj_set_pos(s_home_cnt, -16, 8);

    lv_obj_t *unit = ui_pixel_label(cnt, "天", F_STUDY, HACC);
    lv_obj_set_align(unit, LV_ALIGN_CENTER);
    lv_obj_set_pos(unit, 24, 8);

    if (left < 0) {
        lv_obj_t *g = ui_pixel_label(cnt, "考研日已过，继续加油", F_STUDY, HMUTED);
        lv_obj_set_align(g, LV_ALIGN_BOTTOM_MID); lv_obj_set_pos(g, 0, -12);
    } else {
        char s2[48];
        snprintf(s2, sizeof(s2), "距离 %d月%d日", STUDY_EXAM_MONTH, STUDY_EXAM_DAY);
        lv_obj_t *g = ui_pixel_label(cnt, s2, F_STUDY, HMUTED);
        lv_obj_set_align(g, LV_ALIGN_BOTTOM_MID); lv_obj_set_pos(g, 0, -12);
    }

    /* 两个入口按钮：并排贴近底部：进入学习=实心蓝，设置=柔和中性(无蓝描边) */
    s_home_btn[0] = home_big_button(8, 246, 108, 44, true);
    lv_obj_t *b0 = ui_pixel_label(s_home_btn[0], "进入学习", F_STUDY, 0xFFFFFF);
    lv_obj_center(b0);

    s_home_btn[1] = home_big_button(124, 246, 100, 44, false);
    lv_obj_t *b1 = ui_pixel_label(s_home_btn[1], "设置", F_STUDY, HINK);
    lv_obj_center(b1);

    home_refresh_buttons();
    lv_screen_load(s_home_scr);
}

void ui_home_destroy(void) {
    if (s_home_timer) { lv_timer_del(s_home_timer); s_home_timer = NULL; }
    s_home_date = NULL;
    s_home_clock = NULL;
    if (s_home_scr) { lv_obj_delete(s_home_scr); s_home_scr = NULL; }
}

void ui_home_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;
    if (btn == BSP_BTN_UP && s_home_sel > 0) s_home_sel--;
    if (btn == BSP_BTN_DOWN && s_home_sel < 1) s_home_sel++;
    if (btn == BSP_BTN_OK) {
        if (s_home_sel == 0) s_home_wants_study = true;
        else                 s_home_wants_settings = true;
    }
    home_refresh_buttons();
}

bool ui_home_wants_study(void)     { return s_home_wants_study; }
bool ui_home_wants_settings(void)  { return s_home_wants_settings; }

/* ==============================================================
 * PAGE_TODO — 今日 Task（日常秩序 / 各科学习 两个分组）
 *
 * 布局：顶部白卡（标题+倒计时+进度条）→ 分组分段控件 → 任务列表 → 底部按钮
 * ============================================================== */
#define HEAD_H   78
#define SEG_Y    84
#define SEG_H    24
#define PANEL_Y  112
#define PANEL_H  170
#define BOT_Y    286
#define BOT_H    16
#define GRP_H    16
#define CARD_H   34
#define CARD_MARGIN 4

/* 内部状态 */
static study_todo_tab_t s_tab;
static int s_sel;
static bool s_bottom_focus;
static int s_bottom_idx;           /* 0 = +添加，1 = 设置 */
static bool s_seg_focus;           /* 焦点在页签(日常/各科)时 true；此时上下键左右切换页签 */
static int s_scroll;
static int s_off[16], s_hgt[16];
static int s_list_h;

/* 页面切换请求（由 app_study.c 读取后统一切页，保证 s_page 状态一致） */
static bool s_todo_wants_detail;
static int  s_todo_want_detail_id;
static bool s_todo_wants_add;
static bool s_todo_wants_settings;
static bool s_todo_wants_toggle;      /* OK 快速勾选/取消当前任务 */
static int  s_todo_want_toggle_id;
static bool s_del_arm;                /* 长按OK后的“确认删除”态 */
static int  s_del_id;
static bool s_todo_wants_delete;
static int  s_todo_want_delete_id;
static lv_obj_t *todo_scr;
static lv_obj_t *todo_title_label;   /* 标题「考研助手」 */
static lv_obj_t *todo_cnt_label;     /* 倒计时 pill */
static lv_obj_t *todo_date_label;    /* 日期 */
static lv_obj_t *todo_prog_label;    /* 进度文本 */
static lv_obj_t *todo_prog_bar;      /* 进度条 */
static lv_obj_t *todo_seg[2];        /* 分段控件按钮 */
static lv_obj_t *todo_panel;
static lv_obj_t *todo_card_objs[16];
static int       todo_card_ids[16];
static int       todo_card_n;
static lv_obj_t *btn_add;
static lv_obj_t *btn_set;

static void fill_card_ids_for_tab(void) {
    study_group_t grp = (s_tab == TAB_DAILY) ? STUDY_GROUP_DAILY_ORDER
                                             : STUDY_GROUP_SUBJECTS;
    int pend[16], done[16];
    int np = study_group_list_today(grp, STUDY_DONE_PENDING, pend, 16);
    int nd = study_group_list_today(grp, STUDY_DONE_DONE,    done, 16);

    todo_card_n = 0;
    todo_card_ids[todo_card_n++] = -1 - (int)grp;
    for (int i = 0; i < np && todo_card_n < 16; i++) todo_card_ids[todo_card_n++] = pend[i];
    for (int i = 0; i < nd && todo_card_n < 16; i++) todo_card_ids[todo_card_n++] = done[i];

    int y = 0;
    for (int ci = 0; ci < todo_card_n; ci++) {
        int hh = (todo_card_ids[ci] < 0) ? GRP_H : (CARD_H + CARD_MARGIN);
        s_off[ci] = y; s_hgt[ci] = hh; y += hh;
    }
    s_list_h = y;
}

static void clamp_scroll_to_selection(void) {
    if (s_sel < 0 || s_sel >= todo_card_n) { s_scroll = 0; return; }
    int top = s_off[s_sel];
    int bot = top + s_hgt[s_sel];
    if (top < s_scroll) s_scroll = top;
    if (bot > s_scroll + PANEL_H) s_scroll = bot - PANEL_H;
    int maxs = s_list_h - PANEL_H; if (maxs < 0) maxs = 0;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > maxs) s_scroll = maxs;
}

static void render_todo_cards(void) {
    for (int i = 0; i < 16; i++) {
        if (todo_card_objs[i]) { lv_obj_delete(todo_card_objs[i]); todo_card_objs[i] = NULL; }
    }

    int view_bot = s_scroll + PANEL_H;
    for (int ci = 0; ci < todo_card_n; ci++) {
        int tid = todo_card_ids[ci];
        int abs_y = s_off[ci];
        int hgt   = s_hgt[ci];
        if (abs_y + hgt <= s_scroll || abs_y >= view_bot) continue;
        int y = abs_y - s_scroll;

        if (tid < 0) {
            /* 分组标题 */
            const char *label = (tid == -1) ? "日常秩序" : "各科学习";
            lv_obj_t *h = ui_pixel_label(todo_panel, label, F_STUDY, C_PRIMARY);
            lv_obj_set_pos(h, 6, y);
            lv_obj_set_size(h, 200, GRP_H);
            todo_card_objs[ci] = h;
            continue;
        }
        study_task_t t;
        if (study_task_get(tid, &t) != 0) continue;
        const study_category_t *cat = study_category_get(t.category);
        bool selected = !s_bottom_focus && (ci == s_sel);
        bool armed = selected && s_del_arm;      /* 确认删除态：变红警示 */

        /* 白色圆角任务卡 */
        lv_obj_t *card = mod_card(todo_panel, 2, y, 218, CARD_H,
                                  armed ? 0xFFE2E2 : (selected ? 0x2E3A55 : C_CARD), 12, true);
        if (selected || armed) lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(armed ? 0xE43B2F : C_PRIMARY), 0);

        /* 左侧类别色条（圆角胶囊） */
        lv_obj_t *tab = lv_obj_create(card);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tab, 5, CARD_H - 12);
        lv_obj_set_pos(tab, 8, 6);
        lv_obj_set_style_radius(tab, 3, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(cat ? cat->color_hex : C_PRIMARY), 0);

        /* 复选框(真实方框，按 OK 快速勾选/取消) + 标题 */
        lv_obj_t *cb = lv_obj_create(card);
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cb, 18, 18);
        lv_obj_set_pos(cb, 10, (CARD_H - 18) / 2);
        lv_obj_set_style_radius(cb, 5, 0);
        if (t.done) {
            lv_obj_set_style_bg_color(cb, lv_color_hex(0x2FBF71), 0);
            lv_obj_set_style_border_width(cb, 0, 0);
            lv_obj_t *tick = ui_pixel_label(cb, "✓", F_STUDY, 0xFFFFFF);
            lv_obj_center(tick);
        } else {
            lv_obj_set_style_bg_color(cb, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(cb, 2, 0);
            lv_obj_set_style_border_color(cb, lv_color_hex(C_MUTED), 0);
        }
        lv_obj_t *title = ui_pixel_label(card, t.title, F_STUDY,
                                         t.done ? C_MUTED : C_INK);
        lv_obj_set_pos(title, 36, 2);
        lv_obj_set_width(title, 118);
        lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
        if (t.done) lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_STRIKETHROUGH, 0);

        /* 右上角时间（洗头发：显示“下次洗头日期”） */
        char tbuf[24];
        if (t.hour < 0 && strstr(t.title, "洗头发") != NULL) {
            long nd = study_sched_hair_next_epoch_day();
            if (nd > 0) {
                time_t tt = nd * 86400L;
                struct tm hm;
                localtime_r(&tt, &hm);
                snprintf(tbuf, sizeof(tbuf), "下次%d/%d", hm.tm_mon + 1, hm.tm_mday);
            } else {
                strncpy(tbuf, "周期提醒", sizeof(tbuf));
            }
        } else if (t.hour >= 0 && t.minute >= 0) {
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t.hour, t.minute);
        } else {
            snprintf(tbuf, sizeof(tbuf), "待办");
        }
        lv_obj_t *tt = ui_pixel_label(card, tbuf, F_STUDY, C_MUTED);
        lv_obj_set_align(tt, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_pos(tt, -8, 4);

        todo_card_objs[ci] = card;
    }
}

static void render_todo_seg(void) {
    for (int i = 0; i < 2; i++) {
        if (!todo_seg[i]) continue;
        bool sel = ((int)s_tab == i);
        bool focus = sel && s_seg_focus;
        lv_obj_set_style_bg_color(todo_seg[i],
            lv_color_hex(focus ? C_PRIMARY_D : (sel ? C_PRIMARY : C_CARD)), 0);
        lv_obj_set_style_border_width(todo_seg[i], sel ? 0 : 1, 0);
        /* 内部文字颜色更新 */
        lv_obj_t *lab = lv_obj_get_child(todo_seg[i], 0);
        if (lab) lv_obj_set_style_text_color(lab,
                     lv_color_hex(sel ? 0xFFFFFF : C_MUTED), 0);
    }
}

static void render_header(void) {
    lv_label_set_text(todo_title_label, "考研助手");

    /* 倒计时 */
    int left = study_time_days_until(STUDY_EXAM_MONTH, STUDY_EXAM_DAY);
    char cbuf[24];
    if (left < 0) snprintf(cbuf, sizeof(cbuf), "倒计时 GO");
    else          snprintf(cbuf, sizeof(cbuf), "距考研 %d天", left);
    lv_label_set_text(todo_cnt_label, cbuf);

    /* 日期（民用时间：已校时用真实时间，离线用手动时间） */
    struct tm tv;
    char dbuf[24];
    if (study_time_civil_tm(&tv)) {
        static const char *wd[] = {"日","一","二","三","四","五","六"};
        snprintf(dbuf, sizeof(dbuf), "%d月%d日 %s",
                 tv.tm_mon + 1, tv.tm_mday, wd[tv.tm_wday]);
    } else {
        snprintf(dbuf, sizeof(dbuf), "请设时间");
    }
    lv_label_set_text(todo_date_label, dbuf);

    /* 进度 */
    study_daily_stats_t st = {0};
    study_task_compute_today_stats(&st);
    char pbuf[24];
    snprintf(pbuf, sizeof(pbuf), "%d / %d", st.done, st.total);
    lv_label_set_text(todo_prog_label, pbuf);
    int pct = (st.total > 0) ? (st.done * 100 / st.total) : 0;
    if (pct > 100) pct = 100;
    lv_obj_set_width(todo_prog_bar, (int)(192 * pct / 100));
}

/* 底部栏（+添加 / 设置）选中态：只有选中项变蓝，避免默认全蓝 */
static void render_todo_bottom(void) {
    lv_obj_t *b[2] = { btn_add, btn_set };
    for (int i = 0; i < 2; i++) {
        if (!b[i]) continue;
        bool sel = s_bottom_focus && (s_bottom_idx == i);
        lv_obj_set_style_bg_color(b[i], lv_color_hex(sel ? C_PRIMARY : C_CARD), 0);
        lv_obj_set_style_border_width(b[i], sel ? 0 : 2, 0);
        lv_obj_t *lab = lv_obj_get_child(b[i], 0);
        if (lab) lv_obj_set_style_text_color(lab, lv_color_hex(sel ? 0xFFFFFF : C_PRIMARY), 0);
    }
}

/* 修正选中位置：任务列表存在→选中第一条；为空→自动落到底部栏"+添加" */
static void ensure_todo_selection(void) {
    if (s_bottom_focus) return;
    if (todo_card_n > 1) {
        if (s_sel < 1) s_sel = 1;
        if (s_sel > todo_card_n - 1) s_sel = todo_card_n - 1;
    } else {
        s_bottom_focus = true;
        s_bottom_idx = 0;
    }
}

void ui_todo_build(void) {
    s_tab = TAB_DAILY;
    s_sel = 0;
    s_scroll = 0;
    s_bottom_focus = false;
    s_bottom_idx = 0;
    s_seg_focus = false;
    s_todo_wants_detail = false;
    s_todo_want_detail_id = -1;
    s_todo_wants_add = false;
    s_todo_wants_settings = false;
    s_todo_wants_toggle = false;
    s_todo_want_toggle_id = -1;
    s_del_arm = false;
    s_del_id = -1;
    s_todo_wants_delete = false;
    s_todo_want_delete_id = -1;

    /* 干净的现代背景（不再有草地/云朵） */
    todo_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(todo_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(todo_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(todo_scr, 0, 0);
    lv_obj_set_style_pad_all(todo_scr, 0, 0);
    s_cur_scr = todo_scr;

    /* 顶部白卡 */
    lv_obj_t *head = mod_card(todo_scr, 8, 8, 224, HEAD_H, C_CARD, 16, true);

    todo_title_label = ui_pixel_label(head, "考研助手", F_STUDY, C_INK);
    lv_obj_set_pos(todo_title_label, 16, 8);
    lv_obj_set_style_text_font(todo_title_label, F_STUDY, 0);

    todo_cnt_label = ui_pixel_label(head, "", F_STUDY, C_PRIMARY);
    lv_obj_set_align(todo_cnt_label, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(todo_cnt_label, -14, 8);
    lv_obj_set_style_bg_color(todo_cnt_label, lv_color_hex(0xEAF0FF), 0);
    lv_obj_set_style_pad_hor(todo_cnt_label, 6, 0);
    lv_obj_set_style_radius(todo_cnt_label, 8, 0);

    todo_date_label = ui_pixel_label(head, "", F_STUDY, C_MUTED);
    lv_obj_set_pos(todo_date_label, 16, 32);

    todo_prog_label = ui_pixel_label(head, "", F_STUDY, C_MUTED);
    lv_obj_set_align(todo_prog_label, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(todo_prog_label, -14, 32);

    /* 进度条 */
    lv_obj_t *track = mod_card(head, 16, 56, 192, 8, C_LINE, 4, false);
    todo_prog_bar = mod_card(track, 0, 0, 0, 8, C_PRIMARY, 4, false);

    /* 分段控件：日常秩序 / 各科学习 */
    const char *segname[2] = { "日常秩序", "各科学习" };
    for (int i = 0; i < 2; i++) {
        todo_seg[i] = mod_button(todo_scr, 12 + i * 108, SEG_Y, 104, SEG_H,
                                 (i == 0) ? C_PRIMARY : C_CARD,
                                 (i == 0) ? C_PRIMARY : C_MUTED, i == 0);
        mod_button_label(todo_seg[i], segname[i], (i == 0) ? 0xFFFFFF : C_MUTED);
    }

    /* 任务列表容器 */
    todo_panel = lv_obj_create(todo_scr);
    lv_obj_remove_flag(todo_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(todo_panel, 8, PANEL_Y);
    lv_obj_set_size(todo_panel, 224, PANEL_H);
    lv_obj_set_style_bg_opa(todo_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(todo_panel, 0, 0);
    lv_obj_set_style_pad_all(todo_panel, 0, 0);
    lv_obj_set_style_clip_corner(todo_panel, true, 0);

    /* 底部操作按钮 */
    btn_add = mod_button(todo_scr, 12, BOT_Y, 104, 30, C_CARD, C_PRIMARY, false);
    mod_button_label(btn_add, "+ 添加任务", C_PRIMARY);
    btn_set = mod_button(todo_scr, 124, BOT_Y, 104, 30, C_CARD, C_PRIMARY, false);
    mod_button_label(btn_set, "主页面", C_PRIMARY);

    fill_card_ids_for_tab();
    ensure_todo_selection();
    render_header();
    render_todo_seg();
    render_todo_cards();
    render_todo_bottom();
    lv_screen_load(todo_scr);
}

void ui_todo_destroy(void) {
    if (todo_scr) { lv_obj_delete(todo_scr); todo_scr = NULL; }
    memset(todo_card_objs, 0, sizeof(todo_card_objs));
}

void ui_todo_refresh(void) {
    if (!todo_scr) return;
    fill_card_ids_for_tab();
    ensure_todo_selection();
    render_header();
    render_todo_seg();
    render_todo_cards();
    render_todo_bottom();
    if (s_del_arm && todo_title_label) {
        lv_label_set_text(todo_title_label, "删除？OK删 · 上下取消");
    }
}

/* 任务列表中的可选项区间 [first..last]，索引 0 是分组标题，不参与选中 */
static int todo_first(void) { return (todo_card_n > 1) ? 1 : 0; }
static int todo_last(void)  { return todo_card_n - 1; }

void ui_todo_key(uint8_t btn_u, uint8_t ev_u) {
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    /* 双击 OK = 删除当前选中的任务 */
    if (ev_u == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
        int tid = ui_todo_selected_task_id();
        if (tid > 0) {
            s_todo_wants_delete = true;
            s_todo_want_delete_id = tid;
            ui_todo_refresh();
        }
        return;
    }

    /* 确认删除态：OK = 确认删除；其它键/长按 = 取消 */
    if (s_del_arm) {
        if (ev_u == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            s_todo_wants_delete = true;
            s_todo_want_delete_id = s_del_id;
        } else {
            s_todo_wants_delete = false;
            s_todo_want_delete_id = -1;
        }
        s_del_arm = false;
        s_del_id = -1;
        ui_todo_refresh();
        return;
    }

    /* 长按 上/下 = 切换大页签：日常任务 ⇄ 学习科目 */
    if (ev_u == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        s_tab = (s_tab == TAB_DAILY) ? TAB_SUBJECTS : TAB_DAILY;
        s_bottom_focus = false;
        s_bottom_idx = 0;
        s_sel = todo_first();
        s_scroll = 0;
        ui_todo_refresh();
        return;
    }

    if (ev_u != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        if (s_bottom_focus) {
            if (s_bottom_idx == 0) s_todo_wants_add = true;
            else                   s_todo_wants_settings = true;
        } else if (s_sel >= 1 && s_sel < todo_card_n) {
            int tid = todo_card_ids[s_sel];
            if (tid > 0) {
                /* OK = 点勾选框：快速完成/取消（app_study 统一处理 + 语音） */
                s_todo_wants_toggle = true;
                s_todo_want_toggle_id = tid;
            }
        }
        ui_todo_refresh();
        return;
    }

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (s_bottom_focus) {
            if (btn == BSP_BTN_UP) {
                s_bottom_focus = false;
                s_sel = todo_last();
                clamp_scroll_to_selection();
            } else {
                s_bottom_idx = (s_bottom_idx + 1) % 2;
            }
        } else {
            if (btn == BSP_BTN_DOWN) {
                if (s_sel < todo_last()) s_sel++;
                else { s_bottom_focus = true; s_bottom_idx = 0; }
            } else {
                if (s_sel > todo_first()) s_sel--;
            }
            if (!s_bottom_focus) clamp_scroll_to_selection();
        }
    }
    ui_todo_refresh();
}

bool ui_todo_wants_toggle(int *out_task_id) {
    if (out_task_id) *out_task_id = s_todo_want_toggle_id;
    bool r = s_todo_wants_toggle;
    s_todo_wants_toggle = false;
    return r;
}

/* 删除任务：长按 OK 进入“确认删除”，再按一次 OK 执行 */
void ui_todo_arm_delete(void) {
    int tid = ui_todo_selected_task_id();
    if (tid <= 0) return;
    s_del_arm = true;
    s_del_id = tid;
    ui_todo_refresh();
}
bool ui_todo_delete_armed(void) { return s_del_arm; }
void ui_todo_cancel_delete(void) {
    if (!s_del_arm) return;
    s_del_arm = false;
    s_del_id = -1;
    ui_todo_refresh();
}
bool ui_todo_wants_delete(int *out_task_id) {
    if (out_task_id) *out_task_id = s_todo_want_delete_id;
    bool r = s_todo_wants_delete;
    s_todo_wants_delete = false;
    return r;
}

bool ui_todo_wants_detail(int *out_task_id) {
    if (out_task_id) *out_task_id = s_todo_want_detail_id;
    bool r = s_todo_wants_detail;
    s_todo_wants_detail = false;
    return r;
}
bool ui_todo_wants_add(void) {
    bool r = s_todo_wants_add;
    s_todo_wants_add = false;
    return r;
}
bool ui_todo_wants_settings(void) {
    bool r = s_todo_wants_settings;
    s_todo_wants_settings = false;
    return r;
}

int ui_todo_selected_task_id(void) {
    if (s_bottom_focus) return -1;
    if (s_sel < 0 || s_sel >= todo_card_n) return -1;
    int tid = todo_card_ids[s_sel];
    return tid > 0 ? tid : -1;
}

/* ==============================================================
 * PAGE_ADD_TASK — 添加任务（预设模板选择）
 * ============================================================== */
#define ADD_PER_PAGE 6
static int s_add_step;
static int s_add_sel;
static int s_add_tab;             /* 0=日常任务 1=学习任务 */
static study_task_t s_draft;
static lv_obj_t *s_add_panel;
static lv_obj_t *s_add_hint;
static lv_obj_t *s_add_tabs[2];

/* 返回当前栏的全局预设下标（tab0=日常 cat0；tab1=其余学习科目） */
static int add_filter_ids(int *out_ids, int max) {
    const study_preset_t *ps;
    int n = study_task_presets(&ps);
    int c = 0;
    for (int i = 0; i < n && c < max; i++)
        if ((ps[i].category == 0) == (s_add_tab == 0)) out_ids[c++] = i;
    return c;
}

static void add_render_tabs(void) {
    for (int i = 0; i < 2; i++) {
        if (!s_add_tabs[i]) continue;
        bool sel = (s_add_tab == i);
        lv_obj_set_style_bg_color(s_add_tabs[i], lv_color_hex(sel ? C_PRIMARY : C_CARD), 0);
        lv_obj_t *l = lv_obj_get_child(s_add_tabs[i], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(sel ? 0xFFFFFF : C_PRIMARY), 0);
    }
}

static void add_render_preset_list(void) {
    int ids[48];
    int total = add_filter_ids(ids, 48);
    lv_obj_clean(s_add_panel);
    if (total <= 0) { lv_label_set_text(s_add_hint, "本栏暂无模板"); add_render_tabs(); return; }
    if (s_add_sel >= total) s_add_sel = total - 1;
    if (s_add_sel < 0) s_add_sel = 0;

    const study_preset_t *presets;
    study_task_presets(&presets);
    int page = s_add_sel / ADD_PER_PAGE;
    int first = page * ADD_PER_PAGE;
    int cnt = total - first; if (cnt > ADD_PER_PAGE) cnt = ADD_PER_PAGE;

    for (int k = 0; k < cnt; k++) {
        int row = first + k;
        int idx = ids[row];
        int y = 2 + k * (CARD_H + 4);
        lv_obj_t *card = mod_card(s_add_panel, 0, y, 220, CARD_H,
                                  (row == s_add_sel) ? 0x2E3A55 : C_CARD, 12, true);
        if (row == s_add_sel) {
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_PRIMARY), 0);
        }
        const study_category_t *cat = study_category_get(presets[idx].category);
        lv_obj_t *tab = lv_obj_create(card);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tab, 5, CARD_H - 12); lv_obj_set_pos(tab, 8, 6);
        lv_obj_set_style_radius(tab, 3, 0); lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(cat ? cat->color_hex : C_PRIMARY), 0);
        lv_obj_t *l = ui_pixel_label(card, presets[idx].title, F_STUDY, C_INK);
        lv_obj_set_pos(l, 22, 2); lv_obj_set_width(l, 190);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }

    int total_pages = (total + ADD_PER_PAGE - 1) / ADD_PER_PAGE;
    char h[72];
    snprintf(h, sizeof(h), "第%d/%d页 · 长按上下换栏 · OK添加", page + 1, total_pages);
    lv_label_set_text(s_add_hint, h);
    add_render_tabs();
}

void ui_add_build(void) {
    s_add_step = 0;
    s_add_sel = 0;
    s_add_tab = 0;
    memset(&s_draft, 0, sizeof(s_draft));
    s_draft.hour = -1; s_draft.minute = -1;
    s_draft.repeat = STUDY_REPEAT_ONCE;
    s_add_tabs[0] = s_add_tabs[1] = NULL;

    s_cur_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_cur_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_cur_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_cur_scr, 0, 0);
    lv_obj_set_style_pad_all(s_cur_scr, 0, 0);

    lv_obj_t *head = mod_card(s_cur_scr, 8, 8, 224, 40, C_CARD, 14, true);
    lv_obj_t *title = ui_pixel_label(head, "添加任务", F_STUDY, C_INK);
    lv_obj_set_pos(title, 16, 8);

    /* 两栏页签：日常任务 / 学习任务 */
    for (int i = 0; i < 2; i++) {
        s_add_tabs[i] = mod_button(s_cur_scr, 12 + i * 112, 50, 104, 24,
                                   (i == 0) ? C_PRIMARY : C_CARD,
                                   (i == 0) ? C_PRIMARY : C_PRIMARY, i == 0);
        mod_button_label(s_add_tabs[i], (i == 0) ? "日常任务" : "学习任务",
                         (i == 0) ? 0xFFFFFF : C_PRIMARY);
    }

    s_add_panel = lv_obj_create(s_cur_scr);
    lv_obj_remove_flag(s_add_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_add_panel, 8, 80);
    lv_obj_set_size(s_add_panel, 224, 190);
    lv_obj_set_style_bg_opa(s_add_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_add_panel, 0, 0);
    lv_obj_set_style_pad_all(s_add_panel, 0, 0);
    lv_obj_set_style_clip_corner(s_add_panel, true, 0);

    s_add_hint = ui_pixel_label(s_cur_scr, "", F_STUDY, C_PRIMARY);
    lv_obj_set_align(s_add_hint, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(s_add_hint, 0, -8);

    add_render_preset_list();
    lv_screen_load(s_cur_scr);
}
void ui_add_destroy(void) { if (s_cur_scr) { lv_obj_delete(s_cur_scr); s_cur_scr = NULL; } }

bool ui_add_is_finished(int *out_newly_added_id) {
    if (s_add_step >= 99) {
        if (out_newly_added_id) *out_newly_added_id = s_draft.id;
        return true;
    }
    return false;
}

void ui_add_key(uint8_t btn_u, uint8_t ev_u) {
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    /* 长按 上/下 = 切换栏（日常任务 ⇄ 学习任务） */
    if (ev_u == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        s_add_tab = (s_add_tab == 0) ? 1 : 0;
        s_add_sel = 0;
        add_render_preset_list();
        return;
    }
    if (ev_u != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int ids[48];
        int total = add_filter_ids(ids, 48);
        int old = s_add_sel;
        if (btn == BSP_BTN_UP)   { if (s_add_sel > 0) s_add_sel--; }
        else                     { if (s_add_sel < total - 1) s_add_sel++; }
        if (s_add_sel != old) add_render_preset_list();
        return;
    }

    if (btn == BSP_BTN_OK) {
        int ids[48];
        int total = add_filter_ids(ids, 48);
        if (total <= 0) return;
        if (s_add_step == 0) {
            const study_preset_t *presets;
            study_task_presets(&presets);
            int idx = ids[s_add_sel < total ? s_add_sel : 0];
            strncpy(s_draft.title, presets[idx].title, TASK_TITLE_LEN - 1);
            s_draft.title[TASK_TITLE_LEN - 1] = '\0';
            s_draft.category = presets[idx].category;
            s_draft.subtype  = presets[idx].subtype;
            s_draft.hour     = presets[idx].hour;
            s_draft.minute   = presets[idx].minute;
            s_draft.repeat   = STUDY_REPEAT_DAILY;   /* 每天重复，次日自动复位 */
            if (s_cb && s_cb->on_task_added) {
                study_task_t tmp = s_draft;
                (void)s_cb->on_task_added(&tmp);
            }
            s_add_step = 99;
            return;
        }
    }
}

/* ==============================================================
 * PAGE_TASK_DETAIL — 任务详情
 * ============================================================== */
static int s_detail_id;
static bool s_detail_wants_back;
static lv_obj_t *s_detail_scr;

void ui_detail_build(int task_id) {
    s_detail_id = task_id;
    s_detail_wants_back = false;
    study_task_t t;
    if (study_task_get(task_id, &t) != 0) return;
    const study_category_t *cat = study_category_get(t.category);

    s_detail_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_detail_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_detail_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_detail_scr, 0, 0);
    lv_obj_set_style_pad_all(s_detail_scr, 0, 0);
    s_cur_scr = s_detail_scr;

    lv_obj_t *head = mod_card(s_detail_scr, 8, 8, 224, 40, C_CARD, 14, true);
    lv_obj_t *htitle = ui_pixel_label(head, "任务详情", F_STUDY, C_INK);
    lv_obj_set_pos(htitle, 16, 8);

    lv_obj_t *panel = mod_card(s_detail_scr, 8, 56, 224, 190, C_CARD, 16, true);

    lv_obj_t *tab = lv_obj_create(panel);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tab, 8, 20); lv_obj_set_pos(tab, 14, 14);
    lv_obj_set_style_radius(tab, 4, 0); lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(cat ? cat->color_hex : C_PRIMARY), 0);

    char catbuf[48];
    const study_subtype_info_t *st = study_subtype_get(t.subtype);
    snprintf(catbuf, sizeof(catbuf), "%s · %s", cat ? cat->name_cn : "?",
             st ? st->name_cn : "");
    lv_obj_t *catl = ui_pixel_label(panel, catbuf, F_STUDY, C_INK);
    lv_obj_set_pos(catl, 32, 12);

    lv_obj_t *tlab = ui_pixel_label(panel, t.title, F_STUDY, C_INK);
    lv_obj_set_pos(tlab, 16, 48); lv_obj_set_width(tlab, 192);
    lv_label_set_long_mode(tlab, LV_LABEL_LONG_WRAP);

    char timebuf[40]; char status[40];
    if (t.hour >= 0 && t.minute >= 0) snprintf(timebuf, sizeof(timebuf), "时间 %02d:%02d", t.hour, t.minute);
    else snprintf(timebuf, sizeof(timebuf), "时间 未设定");
    snprintf(status, sizeof(status), "%s", t.done ? "已完成" : "待完成");
    lv_obj_t *tl = ui_pixel_label(panel, timebuf, F_STUDY, C_MUTED);
    lv_obj_set_pos(tl, 16, 96);
    lv_obj_t *sl = ui_pixel_label(panel, status, F_STUDY,
                                  t.done ? C_MUTED : C_PRIMARY);
    lv_obj_set_pos(sl, 16, 124);

    /* 底部按钮 */
    btn_set = NULL;
    lv_obj_t *btn_done = mod_button(s_detail_scr, 16, 262, 96, 34,
                                    t.done ? C_LINE : C_PRIMARY,
                                    t.done ? C_MUTED : 0xFFFFFF, !t.done);
    mod_button_label(btn_done, t.done ? "取消完成" : "完成!", t.done ? C_MUTED : 0xFFFFFF);
    lv_obj_t *btn_del = mod_button(s_detail_scr, 128, 262, 96, 34, C_CARD, 0xE43B2F, false);
    mod_button_label(btn_del, "删除", 0xE43B2F);

    lv_obj_t *hint = ui_pixel_label(s_detail_scr, "OK 完成/取消 · 长按返回", F_STUDY, C_MUTED);
    lv_obj_set_align(hint, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(hint, 0, -6);

    lv_screen_load(s_detail_scr);
}

void ui_detail_destroy(void) {
    if (s_detail_scr) { lv_obj_delete(s_detail_scr); s_detail_scr = NULL; }
}

void ui_detail_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;
    if (btn == BSP_BTN_OK) {
        study_task_t t;
        if (study_task_get(s_detail_id, &t) != 0) return;
        if (s_cb && s_cb->on_task_done_changed) s_cb->on_task_done_changed(s_detail_id, !t.done);
        s_detail_wants_back = true;   /* 由 app_study 统一返回 Todo 页 */
        return;
    }
}

bool ui_detail_wants_back(void) {
    bool r = s_detail_wants_back;
    s_detail_wants_back = false;
    return r;
}

/* ==============================================================
 * PAGE_SETTINGS — 设置（WiFi / 亮度 / 电量 / 音量 / 时间 / 返回主界面）
 * ============================================================== */
enum { SET_WIFI = 0, SET_BRIGHT, SET_VOL, SET_NOW, SET_WAKE, SET_SLEEP, SET_BATT, SET_HOME, SET_N };
static lv_obj_t *s_set_scr;
static lv_obj_t *s_set_cards[SET_N];
static int s_set_sel;
static bool s_set_wants_wifi;
static bool s_set_wants_home;
static bool s_set_wants_todo;
static bool s_set_edit;      /* 正在编辑某数值行 */
static int  s_edit_unit;     /* 多单元行当前编辑的单元 */
static int  s_bright;        /* 亮度 0..100 */
static int  s_vol;           /* 音量 0..100 */
static int  s_man[5];        /* 手动时间编辑用: y mo d h mi */
static bool s_man_valid;
static bool s_wake_set, s_sleep_set;

static int cfg_geti(const char *k, int def) { return (s_cb && s_cb->cfg_get) ? s_cb->cfg_get(k, def) : def; }
static void cfg_seti(const char *k, int v)   { if (s_cb && s_cb->cfg_set) s_cb->cfg_set(k, v); }

static int month_days(int y, int mo) {
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return (mo >= 1 && mo <= 12) ? md[mo - 1] : 30;
}

/* 该行可编辑单元数：亮度/音量=1；时间类=2(时/分)；当前日期时间=3(日/时/分) */
static int row_units(int i) {
    if (i == SET_NOW) return 3;
    if (i == SET_WAKE || i == SET_SLEEP) return 2;
    return 1;
}

static void ui_settings_render_all(void);
static void ui_settings_refresh_highlight(void);

void ui_settings_build(void) {
    s_set_sel = 0;
    s_set_wants_wifi = false;
    s_set_wants_home = false;
    s_set_wants_todo = false;
    s_set_edit = false;
    s_edit_unit = 0;
    s_bright = cfg_geti("bright", 45);   /* 柔和默认亮度，深色主题不刺眼 */
    s_vol    = cfg_geti("volume", 80);
    s_man[0] = cfg_geti("t_y", 0);
    s_man[1] = cfg_geti("t_mo", 0);
    s_man[2] = cfg_geti("t_d", 0);
    s_man[3] = cfg_geti("t_h", 0);
    s_man[4] = cfg_geti("t_mi", 0);
    s_man_valid = (s_man[0] >= 2000);
    s_wake_set = cfg_geti("wake_h", -1) >= 0;
    s_sleep_set = cfg_geti("sleep_h", -1) >= 0;

    s_set_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_set_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_set_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_set_scr, 0, 0);
    lv_obj_set_style_pad_all(s_set_scr, 0, 0);
    s_cur_scr = s_set_scr;

    lv_obj_t *head = mod_card(s_set_scr, 8, 8, 224, 40, C_CARD, 14, true);
    lv_obj_t *title = ui_pixel_label(head, "设置", F_STUDY, C_INK);
    lv_obj_set_pos(title, 16, 8);

    for (int i = 0; i < SET_N; i++) {
        lv_obj_t *card = mod_card(s_set_scr, 12, 58 + i * 30, 216, 26, C_CARD, 10, true);
        lv_obj_t *l = ui_pixel_label(card, "", F_STUDY, C_INK);
        lv_obj_set_pos(l, 12, 2);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        s_set_cards[i] = card;
    }
    ui_settings_render_all();
    lv_screen_load(s_set_scr);
}

static void render_one_row(int i) {
    if (!s_set_cards[i]) return;
    lv_obj_t *l = lv_obj_get_child(s_set_cards[i], 0);
    if (!l) return;
    char buf[48];
    switch (i) {
        case SET_WIFI:  strcpy(buf, "WiFi 连接/配网"); break;
        case SET_BRIGHT: snprintf(buf, sizeof(buf), "屏幕亮度 %d%%", s_bright); break;
        case SET_VOL:    snprintf(buf, sizeof(buf), "音量 %d%%", s_vol); break;
        case SET_BATT: {
            int soc = bsp_battery_soc();
            if (soc < 0) strcpy(buf, "电池电量 --%");
            else snprintf(buf, sizeof(buf), "电池电量 %d%%", soc);
            break;
        }
        case SET_NOW: {
            int y,mo,d,h,mi; bool have=false;
            struct tm tm;
            if (study_time_civil_tm(&tm)) { y=tm.tm_year+1900; mo=tm.tm_mon+1; d=tm.tm_mday; h=tm.tm_hour; mi=tm.tm_min; have=true; }
            else if (s_man_valid) { y=s_man[0]; mo=s_man[1]; d=s_man[2]; h=s_man[3]; mi=s_man[4]; have=true; }
            if (have) snprintf(buf, sizeof(buf), "当前时间 %04d-%02d-%02d %02d:%02d", y,mo,d,h,mi);
            else strcpy(buf, "当前时间 未设置");
            break;
        }
        case SET_WAKE: {
            int h = cfg_geti("wake_h", 7), mi = cfg_geti("wake_m", 0);
            snprintf(buf, sizeof(buf), "起床时间 %02d:%02d", h, mi); break;
        }
        case SET_SLEEP: {
            int h = cfg_geti("sleep_h", 23), mi = cfg_geti("sleep_m", 0);
            snprintf(buf, sizeof(buf), "睡觉时间 %02d:%02d", h, mi); break;
        }
        default: strcpy(buf, "返回主界面"); break;
    }
    if (s_set_edit && s_set_sel == i) {
        /* 编辑态：行首加 ▸ 并高亮当前单元值 */
        char pre[4]; pre[0] = (char)0xE2; pre[1] = (char)0x96; pre[2] = (char)0xB8; pre[3] = 0; /* "▸" UTF8 */
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  <%s>", pre);
    }
    lv_label_set_text(l, buf);
}

static void ui_settings_render_all(void) {
    for (int i = 0; i < SET_N; i++) {
        if (!s_set_cards[i]) continue;
        render_one_row(i);
    }
}

static void persist_now(void) {
    if (!s_man_valid) return;
    cfg_seti("t_y", s_man[0]); cfg_seti("t_mo", s_man[1]); cfg_seti("t_d", s_man[2]);
    cfg_seti("t_h", s_man[3]); cfg_seti("t_mi", s_man[4]);
    study_time_set_manual(s_man[0], s_man[1], s_man[2], s_man[3], s_man[4]);
}

void ui_settings_destroy(void) { if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; } }

void ui_settings_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    if (btn == BSP_BTN_OK) {
        if (s_set_edit) {
            if (s_set_sel == SET_NOW) persist_now();
            int nu = row_units(s_set_sel);
            if (s_edit_unit + 1 < nu) { s_edit_unit++; }
            else { s_set_edit = false; s_edit_unit = 0; }
            ui_settings_refresh_highlight();
            ui_settings_render_all();
            return;
        }
        if (s_set_sel == SET_WIFI) { s_set_wants_wifi = true; return; }
        if (s_set_sel == SET_HOME) { s_set_wants_home = true; return; }
        if (s_set_sel == SET_BRIGHT || s_set_sel == SET_VOL || s_set_sel == SET_NOW ||
            s_set_sel == SET_WAKE || s_set_sel == SET_SLEEP) {
            /* 进入编辑：先依据当前来源初始化工作值 */
            if (s_set_sel == SET_NOW && !s_man_valid && !study_time_synced()) {
                s_man[0] = 2026; s_man[1] = 1; s_man[2] = 1; s_man[3] = 8; s_man[4] = 0;
                s_man_valid = true;
            }
            if (s_set_sel == SET_NOW && s_man_valid) {
                struct tm tm;
                if (study_time_civil_tm(&tm)) {
                    s_man[0]=tm.tm_year+1900; s_man[1]=tm.tm_mon+1; s_man[2]=tm.tm_mday;
                    s_man[3]=tm.tm_hour; s_man[4]=tm.tm_min;
                }
            }
            s_set_edit = true;
            s_edit_unit = 0;
            ui_settings_refresh_highlight();
            ui_settings_render_all();
            return;
        }
        return;
    }

    int dir = (btn == BSP_BTN_UP) ? +1 : -1;
    if (btn != BSP_BTN_UP && btn != BSP_BTN_DOWN) return;

    if (s_set_edit) {
        int row = s_set_sel;
        if (row == SET_BRIGHT) {
            s_bright += dir * 5; if (s_bright < 2) s_bright = 2; if (s_bright > 100) s_bright = 100;
            bsp_display_backlight((uint8_t)s_bright);
            cfg_seti("bright", s_bright);
        } else if (row == SET_VOL) {
            s_vol += dir * 5; if (s_vol < 0) s_vol = 0; if (s_vol > 100) s_vol = 100;
            cfg_seti("volume", s_vol);
        } else if (row == SET_NOW) {
            if (!s_man_valid) return;
            if (s_edit_unit == 0) {   /* 日(步进1天，自动跨月/年) */
                s_man[2] += dir;
                int md = month_days(s_man[0], s_man[1]);
                if (s_man[2] < 1) { s_man[1]--; if (s_man[1] < 1) { s_man[1] = 12; s_man[0]--; } s_man[2] = month_days(s_man[0], s_man[1]); }
                else if (s_man[2] > md) { s_man[2] = 1; s_man[1]++; if (s_man[1] > 12) { s_man[1] = 1; s_man[0]++; } }
                if (s_man[0] < 2000) s_man[0] = 2000;
            } else if (s_edit_unit == 1) {
                s_man[3] = (s_man[3] + dir + 24) % 24;
            } else {
                s_man[4] = (s_man[4] + dir * 5 + 60) % 60;
            }
        } else if (row == SET_WAKE || row == SET_SLEEP) {
            const char *hk = (row == SET_WAKE) ? "wake_h" : "sleep_h";
            const char *mk = (row == SET_WAKE) ? "wake_m" : "sleep_m";
            int h = cfg_geti(hk, row == SET_WAKE ? 7 : 23);
            int mi = cfg_geti(mk, 0);
            if (s_edit_unit == 0) h = (h + dir + 24) % 24;
            else mi = (mi + dir * 5 + 60) % 60;
            cfg_seti(hk, h); cfg_seti(mk, mi);
        }
        ui_settings_render_all();
        return;
    }

    /* 浏览：移动高亮 */
    if (dir > 0) { if (s_set_sel > 0) s_set_sel--; }
    else         { if (s_set_sel < SET_N - 1) s_set_sel++; }
    ui_settings_refresh_highlight();
}

void ui_settings_refresh_highlight(void) {
    for (int i = 0; i < SET_N; i++) {
        if (!s_set_cards[i]) continue;
        bool sel = (i == s_set_sel);
        lv_obj_set_style_bg_color(s_set_cards[i], lv_color_hex(sel ? 0x2E3A55 : C_CARD), 0);
        lv_obj_set_style_border_width(s_set_cards[i], sel ? 2 : 0, 0);
    }
}

bool ui_settings_wants_wifi(void) { return s_set_wants_wifi; }
bool ui_settings_wants_home(void) { return s_set_wants_home; }
bool ui_settings_wants_todo(void) { return s_set_wants_todo; }

/* ==============================================================
 * PAGE_WIFI — WiFi 状态 / 配网引导
 * ============================================================== */
static lv_obj_t *s_wifi_scr;
static lv_obj_t *s_wifi_info;
static lv_timer_t *s_wifi_timer;

static void ui_wifi_refresh(void) {
    if (!s_wifi_info) return;
    const char *ap_ssid = study_wifi_get_ap_ssid();
    if (!ap_ssid || !ap_ssid[0]) ap_ssid = "STU_STUDY_xxxx";
    study_wifi_state_t st = study_wifi_get_state();
    char buf[230];
    if (st == WIFI_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf), "状态：已联网 ✓\n\n网络   %s\n信号   %d dBm\n\n联网校时后日期/倒计时自动准确。",
                 study_wifi_get_ssid(), study_wifi_get_rssi());
    } else if (st == WIFI_STATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "状态：正在连接…\n\n请稍候；成功后会自动校时。");
    } else if (st == WIFI_STATE_FAILED) {
        snprintf(buf, sizeof(buf), "状态：连接失败\n\n请让手机连热点 %s\n密码 %s\n打开 http://%s/ 重填 WiFi 账号密码",
                 ap_ssid, STUDY_WIFI_AP_PASS, STUDY_WIFI_AP_GATEWAY);
    } else {
        snprintf(buf, sizeof(buf), "状态：未连接（热点已开）\n\n①手机连热点  %s\n②密码        %s\n③手机浏览器打开 http://%s/\n④填你家的WiFi 名称与密码并保存",
                 ap_ssid, STUDY_WIFI_AP_PASS, STUDY_WIFI_AP_GATEWAY);
    }
    lv_label_set_text(s_wifi_info, buf);
}

static void wifi_timer_cb(lv_timer_t *t) { (void)t; ui_wifi_refresh(); }

void ui_wifi_build(void) {
    s_wifi_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_wifi_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_wifi_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_wifi_scr, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_scr, 0, 0);
    s_cur_scr = s_wifi_scr;

    lv_obj_t *head = mod_card(s_wifi_scr, 8, 8, 224, 40, C_CARD, 14, true);
    lv_obj_t *title = ui_pixel_label(head, "WiFi 配网", F_STUDY, C_INK);
    lv_obj_set_pos(title, 16, 8);

    lv_obj_t *panel = mod_card(s_wifi_scr, 8, 56, 224, 200, C_CARD, 16, true);

    s_wifi_info = ui_pixel_label(panel, "", F_STUDY, C_INK);
    lv_obj_set_width(s_wifi_info, 200);
    lv_obj_set_pos(s_wifi_info, 12, 12);
    lv_label_set_long_mode(s_wifi_info, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = ui_pixel_label(s_wifi_scr, "长按返回 · 自动刷新状态", F_STUDY, C_MUTED);
    lv_obj_set_pos(hint, 40, 296);

    ui_wifi_refresh();
    if (!s_wifi_timer) s_wifi_timer = lv_timer_create(wifi_timer_cb, 800, NULL);  /* 800ms 自动刷新 */
    lv_screen_load(s_wifi_scr);
}
void ui_wifi_destroy(void) {
    if (s_wifi_timer) { lv_timer_del(s_wifi_timer); s_wifi_timer = NULL; }
    s_wifi_info = NULL;
    if (s_wifi_scr) { lv_obj_delete(s_wifi_scr); s_wifi_scr = NULL; }
}
void ui_wifi_key(uint8_t btn_u, uint8_t ev_u) {
    (void)btn_u; (void)ev_u;
    ui_wifi_refresh();   /* 按键即刷新一次状态 */
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;
    if (btn == BSP_BTN_OK) {
        study_wifi_start_ap_config();
        ui_wifi_refresh();
    }
}

/* ==============================================================
 * 鼓励 & 场景弹窗
 * ============================================================== */
static lv_obj_t *s_enc_scr = NULL;
static lv_obj_t *s_enc_prev = NULL;   /* 弹窗打开前的页面，关闭时切回 */
static lv_obj_t *s_scn_scr = NULL;
static lv_obj_t *s_scn_prev = NULL;

void ui_encourage_show(int category_id) {
    const study_category_t *cat = study_category_get(category_id);
    if (!cat) return;
    if (s_enc_scr) return;
    s_enc_prev = lv_screen_active();   /* 记住来源页面 */
    s_enc_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_enc_scr, W, H);
    lv_obj_set_style_bg_color(s_enc_scr, lv_color_hex(cat->color_hex), 0);
    lv_obj_set_style_border_width(s_enc_scr, 0, 0);

    lv_obj_t *big = ui_pixel_label(s_enc_scr, "完成啦!", F_STUDY, 0xFFFFFF);
    lv_obj_set_pos(big, 80, 70);
    lv_obj_t *msg = ui_pixel_label(s_enc_scr, cat->encouragement, F_STUDY, 0xFFFFFF);
    lv_obj_set_width(msg, W - 40);
    lv_obj_set_pos(msg, 20, 120);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *okl = ui_pixel_label(s_enc_scr, "[按OK继续]", F_STUDY, 0xFFFFFF);
    lv_obj_set_pos(okl, 80, 260);
    lv_screen_load(s_enc_scr);
}
void ui_encourage_close(void) {
    if (s_enc_scr) { lv_obj_delete(s_enc_scr); s_enc_scr = NULL; }
    if (s_enc_prev) { lv_screen_load(s_enc_prev); s_enc_prev = NULL; }  /* 回到来源页 */
}
bool ui_encourage_is_showing(void) { return s_enc_scr != NULL; }

static const char *s_scene_msgs[] = {
    "早安！今天也要加油鸭！",
    "请把手机放到另一个房间!\n保持专注，你可以的！",
    "午饭+背单词+午休30分钟\n记得背一会单词哦！",
    "晚饭后散散步，回来继续",
    "洗漱完毕，辛苦啦！",
    "睡觉啦！回顾一下今天任务\n请放好手机，晚安～",
    "该洗头发啦！清清爽爽～",
};

void ui_scene_show(study_scene_msg_t which) {
    if (which < 0 || which >= (int)(sizeof(s_scene_msgs)/sizeof(s_scene_msgs[0]))) return;
    if (s_scn_scr) return;
    s_scn_prev = lv_screen_active();   /* 记住来源页面 */
    s_scn_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scn_scr, W, H);
    lv_obj_set_style_bg_color(s_scn_scr, lv_color_hex(C_PRIMARY_D), 0);
    lv_obj_set_style_border_width(s_scn_scr, 0, 0);
    lv_obj_t *msg = ui_pixel_label(s_scn_scr, s_scene_msgs[which], F_STUDY, 0xFFFFFF);
    lv_obj_set_width(msg, W - 40);
    lv_obj_set_pos(msg, 20, 120);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *okl = ui_pixel_label(s_scn_scr, "[OK 我知道了]", F_STUDY, 0xFFFFFF);
    lv_obj_set_pos(okl, 75, 260);
    lv_screen_load(s_scn_scr);
}
void ui_scene_close(void) {
    if (s_scn_scr) { lv_obj_delete(s_scn_scr); s_scn_scr = NULL; }
    if (s_scn_prev) { lv_screen_load(s_scn_prev); s_scn_prev = NULL; }  /* 回到来源页 */
}
bool ui_scene_is_showing(void) { return s_scn_scr != NULL; }

#else  /* !ESP_PLATFORM — 宿主编译存根 */

void study_ui_init(const study_ui_callbacks_t *cb) { (void)cb; }
void ui_home_build(void) {}
void ui_home_destroy(void) {}
void ui_home_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
bool ui_home_wants_study(void) { return false; }
bool ui_home_wants_settings(void) { return false; }
void ui_todo_build(void) {}
void ui_todo_destroy(void) {}
void ui_todo_refresh(void) {}
void ui_todo_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
int  ui_todo_selected_task_id(void) { return -1; }
bool ui_todo_wants_detail(int *out_task_id) { (void)out_task_id; return false; }
bool ui_todo_wants_add(void) { return false; }
bool ui_todo_wants_settings(void) { return false; }
void ui_add_build(void) {}
void ui_add_destroy(void) {}
void ui_add_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
bool ui_add_is_finished(int *out_newly_added_id) { (void)out_newly_added_id; return false; }
void ui_detail_build(int task_id) { (void)task_id; }
void ui_detail_destroy(void) {}
void ui_detail_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
bool ui_detail_wants_back(void) { return false; }
void ui_settings_build(void) {}
void ui_settings_destroy(void) {}
void ui_settings_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
bool ui_settings_wants_wifi(void) { return false; }
bool ui_settings_wants_home(void) { return false; }
bool ui_settings_wants_todo(void) { return false; }
void ui_wifi_build(void) {}
void ui_wifi_destroy(void) {}
void ui_wifi_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
void ui_encourage_show(int category_id) { (void)category_id; }
void ui_encourage_close(void) {}
bool ui_encourage_is_showing(void) { return false; }
void ui_scene_show(study_scene_msg_t which) { (void)which; }
void ui_scene_close(void) {}
bool ui_scene_is_showing(void) { return false; }

#endif
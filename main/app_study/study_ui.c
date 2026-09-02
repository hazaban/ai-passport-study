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

#define C_BG        0xEFF3FA   /* 页面背景（浅蓝灰） */
#define C_CARD      0xFFFFFF   /* 卡片白 */
#define C_INK       0x22314D   /* 主文字 */
#define C_MUTED     0x8B9BB5   /* 次要文字 */
#define C_PRIMARY   0x4C7DFF   /* 品牌蓝 */
#define C_PRIMARY_D 0x3569E8
#define C_LINE      0xE3EAF4   /* 分隔线/描边 */
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
static lv_obj_t *s_home_btn[2];      /* 0=进入学习 1=设置 */
static int s_home_sel;
static bool s_home_wants_study;
static bool s_home_wants_settings;

static lv_obj_t *home_card(int y, int h, uint32_t bg) {
    lv_obj_t *c = lv_obj_create(s_home_scr);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, 12, y);
    lv_obj_set_size(c, 216, h);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    /* 扁平风格：不启用阴影 */
    return c;
}

static lv_obj_t *home_big_button(int y, bool primary) {
    lv_obj_t *b = lv_obj_create(s_home_scr);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(b, 12, y);
    lv_obj_set_size(b, 216, 56);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_PRIMARY : C_CARD), 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_PRIMARY), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    (void)primary;   /* 扁平风格：不启用阴影 */
    return b;
}

static void home_refresh_buttons(void) {
    for (int i = 0; i < 2; i++) {
        if (!s_home_btn[i]) continue;
        bool sel = (s_home_sel == i);
        if (i == 0) {
            lv_obj_set_style_bg_color(s_home_btn[i],
                lv_color_hex(sel ? C_PRIMARY_D : C_PRIMARY), 0);
        } else {
            lv_obj_set_style_bg_color(s_home_btn[i],
                lv_color_hex(sel ? 0x4C7DFF : C_CARD), 0);
            lv_obj_set_style_border_width(s_home_btn[i], sel ? 0 : 2, 0);
        }
        lv_obj_t *lab = lv_obj_get_child(s_home_btn[i], 0);
        if (lab) lv_obj_set_style_text_color(lab,
            lv_color_hex((i == 0 || sel) ? 0xFFFFFF : C_PRIMARY), 0);
    }
}

void ui_home_build(void) {
    s_home_sel = 0;
    s_home_wants_study = false;
    s_home_wants_settings = false;

    s_home_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_home_scr, 0, 0);
    lv_obj_set_style_pad_all(s_home_scr, 0, 0);
    s_cur_scr = s_home_scr;

    /* 顶部应用名 + 日期 */
    lv_obj_t *head = home_card(8, 44, C_CARD);
    lv_obj_t *t = ui_pixel_label(head, "考研日程助手", F_STUDY, C_INK);
    lv_obj_set_pos(t, 16, 10);

    time_t now = time(NULL);
    struct tm tv;
    localtime_r(&now, &tv);
    char dbuf[24];
    snprintf(dbuf, sizeof(dbuf), "%d月%d日", tv.tm_mon + 1, tv.tm_mday);
    lv_obj_t *d = ui_pixel_label(head, dbuf, F_STUDY, C_MUTED);
    lv_obj_set_align(d, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(d, -16, 10);

    /* 中部倒计时大卡片 */
    lv_obj_t *cnt = home_card(60, 150, 0x22314D);
    lv_obj_t *cap = ui_pixel_label(cnt, "距离考研", F_STUDY, 0xAAB6D0);
    lv_obj_set_align(cap, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(cap, 0, 18);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);

    /* 大数字：APP-style 用日期字体太小时放大，用稍大字重 */
    char nbuf[16];
    int left = study_time_days_until(STUDY_EXAM_MONTH, STUDY_EXAM_DAY);
    if (left < 0) snprintf(nbuf, sizeof(nbuf), "GO");
    else          snprintf(nbuf, sizeof(nbuf), "%d", left);
    s_home_cnt = ui_pixel_label(cnt, nbuf, F_STUDY, 0xFFFFFF);
    lv_obj_center(s_home_cnt);
    /* 放大视觉效果：与大卡片垂直居中偏下 */
    lv_obj_set_style_text_font(s_home_cnt, F_STUDY, 0);
    lv_obj_set_pos(s_home_cnt, 0, 48);
    lv_obj_set_style_text_align(s_home_cnt, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *unit = ui_pixel_label(cnt, "天", F_STUDY, C_ACCENT);
    lv_obj_set_align(unit, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(unit, 0, -18);

    lv_obj_t *sub = ui_pixel_label(cnt, "学习，让每一天都有意义", F_STUDY, 0x8B9BB5);
    lv_obj_set_align(sub, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(sub, 0, -2);

    /* 底部两个入口按钮 */
    s_home_btn[0] = home_big_button(220, true);
    lv_obj_t *b0 = ui_pixel_label(s_home_btn[0], "进入学习", F_STUDY, 0xFFFFFF);
    lv_obj_center(b0);

    s_home_btn[1] = home_big_button(284, false);
    lv_obj_t *b1 = ui_pixel_label(s_home_btn[1], "设置", F_STUDY, C_PRIMARY);
    lv_obj_center(b1);

    home_refresh_buttons();
    lv_screen_load(s_home_scr);
}

void ui_home_destroy(void) {
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
#define SEG_Y    88
#define SEG_H    26
#define PANEL_Y  120
#define PANEL_H  170
#define BOT_Y    300
#define BOT_H    16
#define GRP_H    20
#define CARD_H   40
#define CARD_MARGIN 6

/* 内部状态 */
static study_todo_tab_t s_tab;
static int s_sel;
static bool s_bottom_focus;
static int s_bottom_idx;           /* 0 = +添加，1 = 设置 */
static int s_scroll;
static int s_off[16], s_hgt[16];
static int s_list_h;
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

        /* 白色圆角任务卡 */
        lv_obj_t *card = mod_card(todo_panel, 2, y, 218, CARD_H,
                                  selected ? 0xEDF2FF : C_CARD, 12, true);
        if (selected) lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_PRIMARY), 0);

        /* 左侧类别色条（圆角胶囊） */
        lv_obj_t *tab = lv_obj_create(card);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tab, 5, CARD_H - 12);
        lv_obj_set_pos(tab, 8, 6);
        lv_obj_set_style_radius(tab, 3, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(cat ? cat->color_hex : C_PRIMARY), 0);

        /* 复选框 + 标题 */
        char title_buf[TASK_TITLE_LEN + 8];
        snprintf(title_buf, sizeof(title_buf), "%s %s",
                 t.done ? "✓" : "○", t.title);
        lv_obj_t *title = ui_pixel_label(card, title_buf, F_STUDY,
                                         t.done ? C_MUTED : C_INK);
        lv_obj_set_pos(title, 22, 2);
        lv_obj_set_width(title, 140);
        lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);

        /* 右上角时间 */
        if (t.hour >= 0 && t.minute >= 0) {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t.hour, t.minute);
            lv_obj_t *tt = ui_pixel_label(card, tbuf, F_STUDY, C_MUTED);
            lv_obj_set_align(tt, LV_ALIGN_TOP_RIGHT);
            lv_obj_set_pos(tt, -8, 4);
        } else {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "待办");
            lv_obj_t *tt = ui_pixel_label(card, tbuf, F_STUDY, C_MUTED);
            lv_obj_set_align(tt, LV_ALIGN_TOP_RIGHT);
            lv_obj_set_pos(tt, -8, 4);
        }

        todo_card_objs[ci] = card;
    }
}

static void render_todo_seg(void) {
    for (int i = 0; i < 2; i++) {
        if (!todo_seg[i]) continue;
        bool sel = ((int)s_tab == i);
        lv_obj_set_style_bg_color(todo_seg[i],
            lv_color_hex(sel ? C_PRIMARY : C_CARD), 0);
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

    /* 日期 */
    time_t now = time(NULL);
    struct tm tv;
    localtime_r(&now, &tv);
    char dbuf[24];
    snprintf(dbuf, sizeof(dbuf), "%d月%d日", tv.tm_mon + 1, tv.tm_mday);
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

void ui_todo_build(void) {
    s_tab = TAB_DAILY;
    s_sel = 0;
    s_scroll = 0;
    s_bottom_focus = false;
    s_bottom_idx = 0;

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
    btn_add = mod_button(todo_scr, 12, BOT_Y, 104, 32, C_PRIMARY, 0xFFFFFF, true);
    mod_button_label(btn_add, "+ 添加任务", 0xFFFFFF);
    btn_set = mod_button(todo_scr, 124, BOT_Y, 104, 32, C_CARD, C_PRIMARY, false);
    mod_button_label(btn_set, "设置", C_PRIMARY);

    fill_card_ids_for_tab();
    render_header();
    render_todo_seg();
    render_todo_cards();
    lv_screen_load(todo_scr);
}

void ui_todo_destroy(void) {
    if (todo_scr) { lv_obj_delete(todo_scr); todo_scr = NULL; }
    memset(todo_card_objs, 0, sizeof(todo_card_objs));
}

void ui_todo_refresh(void) {
    if (!todo_scr) return;
    fill_card_ids_for_tab();
    render_header();
    render_todo_seg();
    render_todo_cards();
}

void ui_todo_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    if (btn == BSP_BTN_OK) {
        if (!s_bottom_focus) {
            int ci = s_sel;
            if (ci >= 0 && ci < todo_card_n) {
                int tid = todo_card_ids[ci];
                if (tid > 0) { ui_todo_destroy(); ui_detail_build(tid); return; }
            }
        } else {
            if (s_bottom_idx == 0) { ui_todo_destroy(); ui_add_build(); return; }
            else                   { ui_todo_destroy(); ui_settings_build(); return; }
        }
    }

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (!s_bottom_focus) {
            if (btn == BSP_BTN_UP) {
                if (s_sel <= 0) {
                    s_tab = (s_tab == TAB_DAILY) ? TAB_SUBJECTS : TAB_DAILY;
                    s_sel = 0; s_scroll = 0;
                } else s_sel--;
            } else {
                if (s_sel >= todo_card_n - 1) { s_bottom_focus = true; s_bottom_idx = 0; }
                else s_sel++;
            }
            if (!s_bottom_focus) clamp_scroll_to_selection();
        } else {
            if (btn == BSP_BTN_UP) {
                s_bottom_focus = false;
                s_sel = (todo_card_n > 0) ? todo_card_n - 1 : 0;
                clamp_scroll_to_selection();
            } else s_bottom_idx = (s_bottom_idx + 1) % 2;
        }
    }
    ui_todo_refresh();
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
static study_task_t s_draft;
static lv_obj_t *s_add_panel;
static lv_obj_t *s_add_hint;

static void add_render_preset_list(void) {
    const study_preset_t *presets;
    int total = study_task_presets(&presets);
    if (total <= 0) return;

    lv_obj_clean(s_add_panel);
    int page = s_add_sel / ADD_PER_PAGE;
    int first = page * ADD_PER_PAGE;
    int cnt = total - first; if (cnt > ADD_PER_PAGE) cnt = ADD_PER_PAGE;

    for (int i = 0; i < cnt; i++) {
        int idx = first + i;
        int y = 2 + i * (CARD_H + 4);
        lv_obj_t *card = mod_card(s_add_panel, 0, y, 220, CARD_H,
                                  (idx == s_add_sel) ? 0xEDF2FF : C_CARD, 12, true);
        if (idx == s_add_sel) lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_PRIMARY), 0);
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
    char h[64];
    snprintf(h, sizeof(h), "第%d/%d页 · OK确认 · 上下选择", page + 1, total_pages);
    lv_label_set_text(s_add_hint, h);
}

void ui_add_build(void) {
    s_add_step = 0;
    s_add_sel = 0;
    memset(&s_draft, 0, sizeof(s_draft));
    s_draft.hour = -1; s_draft.minute = -1;
    s_draft.repeat = STUDY_REPEAT_ONCE;

    s_cur_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_cur_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_cur_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_cur_scr, 0, 0);
    lv_obj_set_style_pad_all(s_cur_scr, 0, 0);

    lv_obj_t *head = mod_card(s_cur_scr, 8, 8, 224, 40, C_CARD, 14, true);
    lv_obj_t *title = ui_pixel_label(head, "添加任务", F_STUDY, C_INK);
    lv_obj_set_pos(title, 16, 8);

    s_add_panel = lv_obj_create(s_cur_scr);
    lv_obj_remove_flag(s_add_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_add_panel, 8, 56);
    lv_obj_set_size(s_add_panel, 224, 220);
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
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        const study_preset_t *presets;
        int total = study_task_presets(&presets);
        int old = s_add_sel;
        if (btn == BSP_BTN_UP)   { if (s_add_sel > 0) s_add_sel--; }
        else                     { if (s_add_sel < total - 1) s_add_sel++; }
        if (s_add_sel != old) add_render_preset_list();
    }

    if (btn == BSP_BTN_OK) {
        const study_preset_t *presets;
        int pn = study_task_presets(&presets);
        if (s_add_step == 0) {
            if (s_add_sel < pn) {
                strncpy(s_draft.title, presets[s_add_sel].title, TASK_TITLE_LEN - 1);
                s_draft.category = presets[s_add_sel].category;
                s_draft.subtype  = presets[s_add_sel].subtype;
                s_draft.hour     = presets[s_add_sel].hour;
                s_draft.minute   = presets[s_add_sel].minute;
            }
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
static lv_obj_t *s_detail_scr;

void ui_detail_build(int task_id) {
    s_detail_id = task_id;
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
        ui_detail_destroy();
        ui_todo_build();
    }
}

/* ==============================================================
 * PAGE_SETTINGS — 设置（WiFi / 亮度 / 电量 / 音量 / 时间 / 返回主界面）
 * ============================================================== */
enum {
    SET_WIFI = 0,
    SET_BRIGHT,
    SET_BATT,
    SET_VOL,
    SET_WAKE,
    SET_SLEEP,
    SET_HOME,
    SET_N
};
static lv_obj_t *s_set_scr;
static lv_obj_t *s_set_cards[SET_N];
static int s_set_sel;
static bool s_set_wants_wifi;
static bool s_set_wants_home;
static int  s_bright;            /* 当前亮度 0..100 */
static bool s_bright_editing;

static void ui_settings_refresh_highlight(void);

static void settings_set_label_text(int i, const char *txt) {
    if (i < 0 || i >= SET_N) return;
    lv_obj_t *l = lv_obj_get_child(s_set_cards[i], 0);
    if (l) lv_label_set_text(l, txt);
}

void ui_settings_build(void) {
    s_set_sel = 0;
    s_set_wants_wifi = false;
    s_set_wants_home = false;
    s_bright_editing = false;
    s_bright = (s_cb && s_cb->cfg_get) ? s_cb->cfg_get("bright", 80) : 80;

    s_set_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_set_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_set_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_set_scr, 0, 0);
    lv_obj_set_style_pad_all(s_set_scr, 0, 0);
    s_cur_scr = s_set_scr;

    lv_obj_t *head = mod_card(s_set_scr, 8, 8, 224, 40, C_CARD, 14, true);
    lv_obj_t *title = ui_pixel_label(head, "设置", F_STUDY, C_INK);
    lv_obj_set_pos(title, 16, 8);

    /* 亮度：调用硬件调节并持久化 */
    char bright_buf[32];
    snprintf(bright_buf, sizeof(bright_buf), "屏幕亮度 %d%%", s_bright);
    /* 电量：只读显示 */
    char batt_buf[32];
    int soc = bsp_battery_soc();
    if (soc < 0) snprintf(batt_buf, sizeof(batt_buf), "电池电量 --%%");
    else         snprintf(batt_buf, sizeof(batt_buf), "电池电量 %d%%", soc);

    static const char *items[SET_N] = {
        "WiFi 连接/配网",
        NULL,
        NULL,
        "音量 80%",
        "起床时间 07:00",
        "睡觉时间 23:00",
        "返回主界面",
    };

    for (int i = 0; i < SET_N; i++) {
        lv_obj_t *card = mod_card(s_set_scr, 12, 60 + i * 32, 216, 28,
                                  (i == s_set_sel) ? 0xEDF2FF : C_CARD, 10, true);
        if (i == s_set_sel) lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_PRIMARY), 0);
        const char *txt = items[i];
        if (i == SET_BRIGHT) txt = bright_buf;
        else if (i == SET_BATT) txt = batt_buf;
        lv_obj_t *l = ui_pixel_label(card, txt, F_STUDY, C_INK);
        lv_obj_set_pos(l, 12, 2);
        s_set_cards[i] = card;
    }
    lv_screen_load(s_set_scr);
}
void ui_settings_destroy(void) { if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; } }
void ui_settings_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    if (btn == BSP_BTN_OK) {
        if (s_bright_editing) { s_bright_editing = false; ui_settings_refresh_highlight(); return; }
        if (s_set_sel == SET_WIFI) { s_set_wants_wifi = true; ui_settings_refresh_highlight(); return; }
        if (s_set_sel == SET_BRIGHT) { s_bright_editing = true; ui_settings_refresh_highlight(); return; }
        if (s_set_sel == SET_HOME) { s_set_wants_home = true; return; }
        ui_settings_destroy();
        ui_todo_build();
        return;
    }

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (s_bright_editing) {
            s_bright += (btn == BSP_BTN_UP) ? 10 : -10;
            if (s_bright < 20) s_bright = 20;
            if (s_bright > 100) s_bright = 100;
            bsp_display_backlight((uint8_t)s_bright);
            if (s_cb && s_cb->cfg_set) s_cb->cfg_set("bright", s_bright);
            char buf[24];
            snprintf(buf, sizeof(buf), "屏幕亮度 %d%%", s_bright);
            settings_set_label_text(SET_BRIGHT, buf);
            ui_settings_refresh_highlight();
            return;
        }
        if (btn == BSP_BTN_UP   && s_set_sel > 0) s_set_sel--;
        if (btn == BSP_BTN_DOWN && s_set_sel < SET_N - 1) s_set_sel++;
        ui_settings_refresh_highlight();
    }
}
void ui_settings_refresh_highlight(void) {
    for (int i = 0; i < SET_N; i++) {
        if (!s_set_cards[i]) continue;
        bool sel = (i == s_set_sel && !s_bright_editing);
        lv_obj_set_style_bg_color(s_set_cards[i], lv_color_hex(sel ? 0xEDF2FF : C_CARD), 0);
        lv_obj_set_style_border_width(s_set_cards[i], sel ? 2 : 0, 0);
    }
}
bool ui_settings_wants_wifi(void) { return s_set_wants_wifi; }
bool ui_settings_wants_home(void) { return s_set_wants_home; }

/* ==============================================================
 * PAGE_WIFI — WiFi 状态 / 配网引导
 * ============================================================== */
static lv_obj_t *s_wifi_scr;
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

    const char *ap_ssid = study_wifi_get_ap_ssid();
    if (!ap_ssid || !ap_ssid[0]) ap_ssid = "STU_STUDY_xxxx";

    study_wifi_state_t st = study_wifi_get_state();
    char buf[220];
    if (st == WIFI_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf), "已联网\n\n网络  %s\n信号  %d dBm\n\n按 OK 重新配网",
                 study_wifi_get_ssid(), study_wifi_get_rssi());
    } else if (st == WIFI_STATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "连接中…\n\n请稍候");
    } else if (st == WIFI_STATE_FAILED) {
        snprintf(buf, sizeof(buf), "连接失败\n\n1.连热点 %s\n2.打开 http://%s/\n3.重填账号密码",
                 ap_ssid, STUDY_WIFI_AP_GATEWAY);
    } else {
        snprintf(buf, sizeof(buf), "按 OK 开热点\n\n1.连热点 %s\n2.打开 http://%s/\n3.填 WiFi 账号密码",
                 ap_ssid, STUDY_WIFI_AP_GATEWAY);
    }
    lv_obj_t *info = ui_pixel_label(panel, buf, F_STUDY, C_INK);
    lv_obj_set_width(info, 200);
    lv_obj_set_pos(info, 12, 12);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = ui_pixel_label(s_wifi_scr, "OK 开启/重配 · 长按返回", F_STUDY, C_MUTED);
    lv_obj_set_pos(hint, 50, 296);
    lv_screen_load(s_wifi_scr);
}
void ui_wifi_destroy(void) { if (s_wifi_scr) { lv_obj_delete(s_wifi_scr); s_wifi_scr = NULL; } }
void ui_wifi_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;
    if (btn == BSP_BTN_OK) {
        study_wifi_start_ap_config();
        ui_wifi_destroy();
        ui_wifi_build();
    }
}

/* ==============================================================
 * 鼓励 & 场景弹窗
 * ============================================================== */
static lv_obj_t *s_enc_scr = NULL;
static lv_obj_t *s_scn_scr = NULL;

void ui_encourage_show(int category_id) {
    const study_category_t *cat = study_category_get(category_id);
    if (!cat) return;
    if (s_enc_scr) return;
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
void ui_encourage_close(void) { if (s_enc_scr) { lv_obj_delete(s_enc_scr); s_enc_scr = NULL; } }
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
void ui_scene_close(void) { if (s_scn_scr) { lv_obj_delete(s_scn_scr); s_scn_scr = NULL; } }
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
void ui_add_build(void) {}
void ui_add_destroy(void) {}
void ui_add_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
bool ui_add_is_finished(int *out_newly_added_id) { (void)out_newly_added_id; return false; }
void ui_detail_build(int task_id) { (void)task_id; }
void ui_detail_destroy(void) {}
void ui_detail_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
void ui_settings_build(void) {}
void ui_settings_destroy(void) {}
void ui_settings_key(uint8_t btn, uint8_t ev) { (void)btn; (void)ev; }
bool ui_settings_wants_wifi(void) { return false; }
bool ui_settings_wants_home(void) { return false; }
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
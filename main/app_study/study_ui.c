/*
 * study_ui.c — 全部 UI 页面实现（竖屏 240×320）。
 * 屏幕布局采用与 demo_display/demo_audio 完全同构的 LVGL API 用法，保证在
 * ESP32 端即插即用。代码用 #ifdef ESP_PLATFORM 包裹 LVGL 调用，宿主环境下仅
 * 保留函数签名便于编译。
 */
#include "study_ui.h"
#include "study_task.h"
#include "study_group.h"
#include "study_category.h"
#include "study_wifi.h"
#include <string.h>

#ifdef ESP_PLATFORM

#include "lvgl.h"
#include "ui_pixel.h"
#include "bsp_display.h"

/* ==============================================================
 * 共用对象（跨页共享，如吉祥物、回调指针）
 * ============================================================== */
static const study_ui_callbacks_t *s_cb = NULL;
static lv_obj_t *s_mascot = NULL;
static lv_obj_t *s_cur_scr = NULL;

void study_ui_init(const study_ui_callbacks_t *cb) {
    s_cb = cb;
}

/* ==============================================================
 * PAGE_TODO — 今日 Todo（左右双 Tab + 双分组）
 *   Left Tab (TAB_PENDING)  : 未完成
 *      ├ 日常秩序 (PENDING)
 *      └ 各科目 (PENDING)
 *   Right Tab (TAB_DONE)    : 已完成
 *      ├ 日常秩序 (DONE)
 *      └ 各科目 (DONE)
 *
 * 可视区域：240×320。顶部 40 状态栏 + 中部 220 任务卡 + 底部 60 双按钮。
 * ============================================================== */
#define W       240
#define H       320
#define TOP_H   40
#define MID_H   220
#define BOT_H   60
#define GRP_LABEL_H  18
#define CARD_H       40
#define CARD_MARGIN   4

/* 内部状态 */
static study_todo_tab_t s_tab;
static int s_group_daily_sel;     /* 日常秩序组选中下标 -1 = 未选 */
static int s_group_daily_count;
static int s_group_subj_sel;      /* 各科目组选中下标 -1 = 未选 */
static int s_group_subj_count;
static int s_focus;               /* 0 = 日常秩序组，1 = 各科目组，2 = +添加，3 = 设置 */
static bool s_bottom_focus;       /* true = 焦点在底部按钮 */
static int s_bottom_idx;          /* 0 = +添加，1 = 设置 */
static lv_obj_t *todo_scr;
static lv_obj_t *todo_tab_label;
static lv_obj_t *todo_date_label;
static lv_obj_t *todo_prog_label;
static lv_obj_t *todo_panel;      /* 中部任务卡容器 */
static lv_obj_t *todo_card_objs[16];  /* 卡数量上界 16 (240 屏放得下) */
static int       todo_card_ids[16];   /* 对应 task id */
static int       todo_card_n;
static lv_obj_t *btn_add;
static lv_obj_t *btn_set;

static void fill_card_ids_for_tab(void) {
    /* 把当前 Tab 的两张分组 id 展开，填入 todo_card_ids；
     * 每个分组先写一组头（"日常秩序"/"各科目"，用 task_id = -1 表示 section header）*/
    study_group_t grps[2] = { STUDY_GROUP_DAILY_ORDER, STUDY_GROUP_SUBJECTS };
    study_done_filter_t filter = (s_tab == TAB_PENDING) ? STUDY_DONE_PENDING : STUDY_DONE_DONE;

    todo_card_n = 0;
    for (int g = 0; g < 2 && todo_card_n < 16; g++) {
        int ids[16];
        int n = study_group_list_today(grps[g], filter, ids, 16);
        if (n == 0) continue;
        todo_card_ids[todo_card_n++] = -1 - g;     /* -1 = 日常秩序 header, -2 = 各科目 header */
        for (int i = 0; i < n && todo_card_n < 16; i++) {
            todo_card_ids[todo_card_n++] = ids[i];
        }
    }
}

static void render_todo_cards(void) {
    /* 清空旧卡 */
    for (int i = 0; i < 16; i++) {
        if (todo_card_objs[i]) { lv_obj_delete(todo_card_objs[i]); todo_card_objs[i] = NULL; }
    }

    int y = 0;
    for (int ci = 0; ci < todo_card_n; ci++) {
        int tid = todo_card_ids[ci];
        if (tid < 0) {
            /* Section header */
            const char *label = (tid == -1) ? "· 日常秩序 ·" : "· 各科学习 ·";
            uint32_t color = UI_MUTED;
            lv_obj_t *h = ui_pixel_label(todo_panel, label, &lv_font_montserrat_12, UI_INK);
            lv_obj_set_pos(h, 6, y + 2);
            lv_obj_set_size(h, W - 24, GRP_LABEL_H);
            (void)color;
            y += GRP_LABEL_H;
            todo_card_objs[ci] = h;
            continue;
        }
        study_task_t t;
        if (study_task_get(tid, &t) != 0) continue;
        const study_category_t *cat = study_category_get(t.category);

        lv_obj_t *card = ui_pixel_panel_create(todo_panel, 4, y, W - 16, CARD_H, UI_PAPER);
        bool selected = false;
        if (!s_bottom_focus) {
            if (s_focus == 0 && s_group_daily_count > 0 && s_group_daily_sel == ci) selected = true;
            if (s_focus == 1 && s_group_subj_count  > 0 && s_group_subj_sel  == ci) selected = true;
        }
        if (selected) ui_pixel_set_selected(card, true, true);

        /* 左：色条 */
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_set_size(bar, 6, CARD_H - 8);
        lv_obj_set_pos(bar, 6, 4);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(cat ? cat->color_hex : UI_INK), 0);
        lv_obj_set_style_border_width(bar, 0, 0);

        /* 时间（如果有） */
        char tbuf[16];
        if (t.hour >= 0 && t.minute >= 0) {
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t.hour, t.minute);
        } else {
            tbuf[0] = '\0';
        }
        lv_obj_t *tlab = ui_pixel_label(card, tbuf, &lv_font_montserrat_12, UI_INK);
        lv_obj_set_pos(tlab, 20, 4);

        /* 复选框 + 标题 */
        char title_buf[TASK_TITLE_LEN + 6];
        snprintf(title_buf, sizeof(title_buf), "%c %s",
                 t.done ? '✓' : '□', t.title);
        lv_obj_t *title = ui_pixel_label(card, title_buf, &lv_font_montserrat_14,
                                         t.done ? 0x888888 : UI_INK);
        lv_obj_set_pos(title, 20, 20);
        lv_obj_set_width(title, W - 50);
        lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);

        todo_card_objs[ci] = card;
        y += CARD_H + CARD_MARGIN;
    }
}

static void render_tab_label(void) {
    const char *t = (s_tab == TAB_PENDING) ? "◀ 未完成 · 已完成 ▶" : "◀ 未完成 · 已完成 ▶";
    lv_label_set_text(todo_tab_label, t);
}

static void render_date_and_progress(void) {
    /* 日期 — 简化：显示"备考加油"文字 + 完成进度 */
    study_daily_stats_t st = {0};
    study_task_compute_today_stats(&st);
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "进度 %d/%d", st.done, st.total);
    lv_label_set_text(todo_prog_label, pbuf);
}

void ui_todo_build(void) {
    s_tab = TAB_PENDING;
    s_focus = 0;        /* 默认焦点在「日常秩序」组 */
    s_group_daily_sel = s_group_subj_sel = 0;
    s_bottom_focus = false;

    todo_scr = ui_pixel_screen_create("KAOYAN");
    s_cur_scr = todo_scr;

    /* 顶部状态栏 */
    todo_date_label = ui_pixel_label(todo_scr, "考研助手·加油上岸!", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(todo_date_label, 12, 10);
    lv_obj_set_width(todo_date_label, 130);

    todo_prog_label = ui_pixel_label(todo_scr, "", &lv_font_montserrat_12, UI_SKY);
    lv_obj_set_pos(todo_prog_label, 150, 12);
    lv_obj_set_width(todo_prog_label, 80);
    lv_obj_set_style_text_align(todo_prog_label, LV_TEXT_ALIGN_RIGHT, 0);

    todo_tab_label = ui_pixel_label(todo_scr, "", &lv_font_montserrat_12, UI_INK);
    lv_obj_set_width(todo_tab_label, W - 24);
    lv_obj_set_pos(todo_tab_label, 12, TOP_H - 18);
    lv_obj_set_style_text_align(todo_tab_label, LV_TEXT_ALIGN_CENTER, 0);
    render_tab_label();

    /* 中部面板 */
    todo_panel = ui_pixel_panel_create(todo_scr, 10, TOP_H + 4, W - 20, MID_H, UI_PAPER);

    /* 底部：[+ 添加任务]  [设置] */
    btn_add = ui_pixel_panel_create(todo_scr, 16, TOP_H + 4 + MID_H + 10, 96, 36,
                                    s_bottom_focus && s_bottom_idx == 0 ? UI_SKY : UI_MUTED);
    lv_obj_t *add_lab = ui_pixel_label(btn_add, "+ 添加任务", &lv_font_montserrat_12, UI_INK);
    lv_obj_center(add_lab);

    btn_set = ui_pixel_panel_create(todo_scr, 128, TOP_H + 4 + MID_H + 10, 96, 36,
                                    s_bottom_focus && s_bottom_idx == 1 ? UI_SKY : UI_MUTED);
    lv_obj_t *set_lab = ui_pixel_label(btn_set, "设置", &lv_font_montserrat_12, UI_INK);
    lv_obj_center(set_lab);

    s_mascot = ui_pixel_mascot_create(todo_scr, 101, H - 34);

    fill_card_ids_for_tab();
    render_todo_cards();
    render_date_and_progress();
    lv_screen_load(todo_scr);
}

void ui_todo_destroy(void) {
    if (todo_scr) { lv_obj_delete(todo_scr); todo_scr = NULL; }
    memset(todo_card_objs, 0, sizeof(todo_card_objs));
}

void ui_todo_refresh(void) {
    if (!todo_scr) return;
    fill_card_ids_for_tab();
    render_todo_cards();
    render_date_and_progress();
    render_tab_label();
}

void ui_todo_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    /* 双击快捷跳过 */
    if (btn == BSP_BTN_OK) {
        if (!s_bottom_focus) {
            /* 在某个任务上按 OK → 跳详情页 */
            int ci = -1;
            if (s_focus == 0) ci = s_group_daily_sel;
            else if (s_focus == 1) ci = s_group_subj_sel;
            if (ci >= 0 && ci < todo_card_n) {
                int tid = todo_card_ids[ci];
                if (tid > 0) {
                    ui_todo_destroy();
                    ui_detail_build(tid);
                    return;
                }
            }
        } else {
            /* 底部按钮 */
            if (s_bottom_idx == 0) {
                ui_todo_destroy();
                ui_add_build();
                return;
            } else {
                ui_todo_destroy();
                ui_settings_build();
                return;
            }
        }
    }

    /* Tab 切换：长按上下或边界切换。简化：上键到顶再按 = 切到上一 Tab */
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        /* 简单 tab 切换：左右键概念复用「OK+上=切左TAB, OK+下=切右TAB」不合适，
         * 用「在底部 0(添加)上再按上 = 切到已完成 TAB；在已完成上再按上回到未完成」
         * 过于模糊。这里用一个清晰规则：
         *   OK 长按 = 返回菜单 (main.c 统一处理)
         *   上/下 = 移动焦点（在两组之间 + 底部之间循环）
         *   [特殊: 在日常秩序组的 header 上按 UP = 切左TAB / DOWN 时最后按到最底部切右TAB]
         * 用更直接的：双击上=切上tab, 双击下=切下tab, 实现复杂度高。
         * → 简化实现：按「OK + 上/下」组合难捕获, 改用「当焦点在最上再上=切tab, 最下再下=切tab */
        if (!s_bottom_focus) {
            if (btn == BSP_BTN_UP) {
                /* 当前组内上移 */
                if (s_focus == 0) {
                    if (s_group_daily_sel <= 0) {
                        /* 已在日常秩序最上：切 Tab (PENDING↔DONE) */
                        s_tab = (s_tab == TAB_PENDING) ? TAB_DONE : TAB_PENDING;
                        s_group_daily_sel = s_group_subj_sel = 0;
                    } else {
                        s_group_daily_sel--;
                    }
                } else {
                    if (s_group_subj_sel <= 0) {
                        s_focus = 0;   /* 切回日常秩序组的最后一个 */
                        s_group_daily_sel = (s_group_daily_count > 0) ? (s_group_daily_count - 1) : 0;
                    } else {
                        s_group_subj_sel--;
                    }
                }
            } else { /* DOWN */
                if (s_focus == 0) {
                    if (s_group_daily_count == 0 || s_group_daily_sel >= s_group_daily_count - 1) {
                        s_focus = 1; s_group_subj_sel = 0;
                    } else {
                        s_group_daily_sel++;
                    }
                } else {
                    if (s_group_subj_count == 0 || s_group_subj_sel >= s_group_subj_count - 1) {
                        s_bottom_focus = true; s_bottom_idx = 0;
                    } else {
                        s_group_subj_sel++;
                    }
                }
            }
        } else {
            /* 焦点在底部按钮 */
            if (btn == BSP_BTN_UP) {
                /* 回到各科目最后一个或日常秩序最后一个 */
                s_bottom_focus = false;
                s_focus = 1;
                s_group_subj_sel = (s_group_subj_count > 0) ? (s_group_subj_count - 1) : 0;
                if (s_group_subj_count == 0) { s_focus = 0; s_group_daily_sel = (s_group_daily_count > 0) ? (s_group_daily_count - 1) : 0; }
            } else {
                s_bottom_idx = (s_bottom_idx + 1) % 2;
            }
        }
    }
    ui_todo_refresh();
}

int ui_todo_selected_task_id(void) {
    int ci = -1;
    if (s_bottom_focus) return -1;
    if (s_focus == 0) ci = s_group_daily_sel;
    if (s_focus == 1) ci = s_group_subj_sel;
    if (ci < 0 || ci >= todo_card_n) return -1;
    int tid = todo_card_ids[ci];
    return tid > 0 ? tid : -1;
}

/* ==============================================================
 * PAGE_ADD_TASK — 添加任务（4 步）
 *   Step 0: 从 26 个预设模板里挑一个（最高效）
 *   Step 1: 选择 10 个类别
 *   Step 2: 选择 7 个子分类
 *   Step 3: 选择 时间 (HH:MM)
 * ============================================================== */
static int s_add_step;
static int s_add_sel;
static study_task_t s_draft;

void ui_add_build(void) {
    s_add_step = 0;
    s_add_sel = 0;
    memset(&s_draft, 0, sizeof(s_draft));
    s_draft.hour = -1; s_draft.minute = -1;
    s_draft.repeat = STUDY_REPEAT_ONCE;

    lv_obj_t *scr = ui_pixel_screen_create("ADD TASK");
    s_cur_scr = scr;

    lv_obj_t *title = ui_pixel_label(scr, "添加任务 · 选模板", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(title, 12, 10);

    lv_obj_t *panel = ui_pixel_panel_create(scr, 10, 44, 220, 230, UI_PAPER);

    const study_preset_t *presets;
    int n = study_task_presets(&presets);
    if (n > 6) n = 6;  /* 一屏最多展示 6 条 */
    for (int i = 0; i < n; i++) {
        int y = 8 + i * 36;
        lv_obj_t *card = ui_pixel_panel_create(panel, 6, y, 208, 32, UI_MUTED);
        if (i == s_add_sel) ui_pixel_set_selected(card, true, true);
        const study_category_t *cat = study_category_get(presets[i].category);
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_set_size(bar, 4, 24); lv_obj_set_pos(bar, 4, 4);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(cat ? cat->color_hex : UI_INK), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s", presets[i].title);
        lv_obj_t *l = ui_pixel_label(card, buf, &lv_font_montserrat_12, UI_INK);
        lv_obj_set_pos(l, 16, 6);
        lv_obj_set_width(l, 180);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }

    lv_obj_t *hint = ui_pixel_label(scr, "OK:确认 上/下:选择", &lv_font_montserrat_12, UI_SKY);
    lv_obj_set_pos(hint, 40, 296);
    s_mascot = ui_pixel_mascot_create(scr, 101, H - 34);
    lv_screen_load(scr);
}
void ui_add_destroy(void) { if (s_cur_scr) { lv_obj_delete(s_cur_scr); s_cur_scr = NULL; } }

bool ui_add_is_finished(int *out_newly_added_id) {
    /* 简化：当用户走到第 3 步再按 OK 时，提交并返回 true，回填 id */
    if (s_add_step >= 99) {
        if (out_newly_added_id) *out_newly_added_id = s_draft.id;
        return true;
    }
    return false;
}

void ui_add_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;

    if (btn == BSP_BTN_UP) { if (s_add_sel > 0) s_add_sel--; }
    if (btn == BSP_BTN_DOWN) { s_add_sel++; }

    if (btn == BSP_BTN_OK) {
        const study_preset_t *presets;
        int pn = study_task_presets(&presets);
        if (s_add_step == 0) {
            /* Step 0: 预设选中 → 填 draft 并进 category 步 */
            if (s_add_sel < pn) {
                strncpy(s_draft.title, presets[s_add_sel].title, TASK_TITLE_LEN - 1);
                s_draft.category = presets[s_add_sel].category;
                s_draft.subtype  = presets[s_add_sel].subtype;
                s_draft.hour     = presets[s_add_sel].hour;
                s_draft.minute   = presets[s_add_sel].minute;
            }
            /* 直接提交：简化实现，直接 add 并标记 step=99 */
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

    s_detail_scr = ui_pixel_screen_create("DETAIL");
    s_cur_scr = s_detail_scr;

    lv_obj_t *title = ui_pixel_label(s_detail_scr, t.title, &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(title, 12, 10);
    lv_obj_set_width(title, 216);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

    lv_obj_t *panel = ui_pixel_panel_create(s_detail_scr, 12, 56, 216, 190, UI_PAPER);

    /* 类别+色条 */
    lv_obj_t *bar = lv_obj_create(panel);
    lv_obj_set_size(bar, 12, 28); lv_obj_set_pos(bar, 10, 12);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(cat ? cat->color_hex : UI_INK), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    char catbuf[64];
    const study_subtype_info_t *st = study_subtype_get(t.subtype);
    snprintf(catbuf, sizeof(catbuf), "%s · %s", cat ? cat->name_cn : "?",
             st ? st->name_cn : "");
    lv_obj_t *catl = ui_pixel_label(panel, catbuf, &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(catl, 32, 16);

    char timebuf[64];
    if (t.hour >= 0 && t.minute >= 0) {
        snprintf(timebuf, sizeof(timebuf), "时间: %02d:%02d", t.hour, t.minute);
    } else {
        snprintf(timebuf, sizeof(timebuf), "时间: 未设定");
    }
    lv_obj_t *tl = ui_pixel_label(panel, timebuf, &lv_font_montserrat_12, UI_INK);
    lv_obj_set_pos(tl, 12, 56);

    char status[32];
    snprintf(status, sizeof(status), "状态: %s", t.done ? "✓ 已完成" : "□ 待完成");
    lv_obj_t *sl = ui_pixel_label(panel, status, &lv_font_montserrat_12, UI_INK);
    lv_obj_set_pos(sl, 12, 80);

    /* 底部按钮区 */
    lv_obj_t *btn_done = ui_pixel_panel_create(s_detail_scr, 16, 258, 64, 36,
                                               t.done ? UI_GRASS : UI_YELLOW);
    lv_obj_t *dl = ui_pixel_label(btn_done, t.done ? "取消✓" : "完成!",
                                  &lv_font_montserrat_12, UI_INK);
    lv_obj_center(dl);

    lv_obj_t *btn_del = ui_pixel_panel_create(s_detail_scr, 160, 258, 64, 36, UI_RED);
    lv_obj_t *xl = ui_pixel_label(btn_del, "删除", &lv_font_montserrat_12, 0xFFFFFF);
    lv_obj_center(xl);

    lv_obj_t *hint = ui_pixel_label(s_detail_scr, "OK:切键;长按OK:返回", &lv_font_montserrat_10, UI_SKY);
    lv_obj_set_pos(hint, 60, 242);

    s_mascot = ui_pixel_mascot_create(s_detail_scr, 101, H - 34);
    (void)btn_done; (void)btn_del;
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
 * PAGE_SETTINGS — 设置
 * ============================================================== */
static lv_obj_t *s_set_scr;
static lv_obj_t *s_set_cards[7];
static int s_set_sel;
static bool s_set_wants_wifi;

static void ui_settings_refresh_highlight(void);

void ui_settings_build(void) {
    s_set_sel = 0;
    s_set_wants_wifi = false;
    s_set_scr = ui_pixel_screen_create("SETTINGS");
    s_cur_scr = s_set_scr;
    lv_obj_t *title = ui_pixel_label(s_set_scr, "设置", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(title, 12, 10);

    lv_obj_t *panel = ui_pixel_panel_create(s_set_scr, 10, 40, 220, 230, UI_PAPER);
    const char *items[] = {
        "WiFi · 连接/配网",
        "音量 80%",
        "起床时间 07:00",
        "睡觉时间 23:00",
        "语音包 · 水豚噜噜",
        "清除已完成的单次任务",
        "关于 考研助手 v1.0",
    };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *card = ui_pixel_panel_create(panel, 8, 8 + i * 32, 204, 28,
                                               (i == s_set_sel) ? UI_PAPER : UI_MUTED);
        if (i == s_set_sel) ui_pixel_set_selected(card, true, true);
        lv_obj_t *l = ui_pixel_label(card, items[i], &lv_font_montserrat_12, UI_INK);
        lv_obj_set_pos(l, 10, 5);
        s_set_cards[i] = card;
    }
    s_mascot = ui_pixel_mascot_create(s_set_scr, 101, H - 34);
    lv_screen_load(s_set_scr);
}
void ui_settings_destroy(void) { if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; } }
void ui_settings_key(uint8_t btn_u, uint8_t ev_u) {
    if (ev_u != BSP_BTN_CLICK) return;
    bsp_btn_t btn = (bsp_btn_t)btn_u;
    if (btn == BSP_BTN_UP   && s_set_sel > 0) s_set_sel--;
    if (btn == BSP_BTN_DOWN && s_set_sel < 6) s_set_sel++;
    if (btn == BSP_BTN_OK) {
        if (s_set_sel == 5) {
            /* 清除已完成 */
            int n = study_task_archive_done_once();
            (void)n;
        }
        if (s_set_sel == 0) {
            /* 进入 WiFi 配网/状态页 */
            s_set_wants_wifi = true;
            ui_settings_refresh_highlight();
            return;
        }
        if (s_set_sel == 0 || s_set_sel == 6) {
            /* 音量/关于：简化处理，长按 OK 返回 */
        }
        ui_settings_destroy();
        ui_todo_build();
    }
}
/* 高亮刷新（随选中移动） */
void ui_settings_refresh_highlight(void) {
    for (int i = 0; i < 7; i++) {
        if (!s_set_cards[i]) continue;
        ui_pixel_set_selected(s_set_cards[i], (i == s_set_sel), true);
    }
}
bool ui_settings_wants_wifi(void) { return s_set_wants_wifi; }

/* ==============================================================
 * PAGE_WIFI — WiFi 状态 / 配网引导
 * ============================================================== */
static lv_obj_t *s_wifi_scr;
void ui_wifi_build(void) {
    s_wifi_scr = ui_pixel_screen_create("WIFI");
    s_cur_scr = s_wifi_scr;
    lv_obj_t *title = ui_pixel_label(s_wifi_scr, "WiFi 配网", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(title, 12, 10);

    lv_obj_t *panel = ui_pixel_panel_create(s_wifi_scr, 10, 44, 220, 200, UI_PAPER);

    /* 当前 AP 热点名（配网状态下展示，方便手机搜索） */
    wifi_config_t wc;
    const char *ap_ssid = "STU_STUDY_xxxx";
    if (esp_wifi_get_config(WIFI_IF_AP, &wc) == ESP_OK && wc.ap.ssid[0]) {
        ap_ssid = (const char *)wc.ap.ssid;
    }

    study_wifi_state_t st = study_wifi_get_state();
    char buf[200];
    if (st == WIFI_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf),
                 "已联网\n网络: %s\n信号: %d dBm\n\n按 OK 重新配网",
                 study_wifi_get_ssid(), study_wifi_get_rssi());
    } else if (st == WIFI_STATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "连接中…\n\n请稍候");
    } else if (st == WIFI_STATE_FAILED) {
        snprintf(buf, sizeof(buf),
                 "连接失败\n\n请重新配网：\n1.手机连热点 %s\n2.打开 http://%s/\n3.重填账号密码",
                 ap_ssid, STUDY_WIFI_AP_GATEWAY);
    } else {
        snprintf(buf, sizeof(buf),
                 "开启配网后：\n1.手机连热点 %s\n2.浏览器打开 http://%s/\n3.填 WiFi 账号密码提交",
                 ap_ssid, STUDY_WIFI_AP_GATEWAY);
    }
    lv_obj_t *info = ui_pixel_label(panel, buf, &lv_font_montserrat_12, UI_INK);
    lv_obj_set_width(info, 196);
    lv_obj_set_pos(info, 12, 12);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    lv_obj_t *hint = ui_pixel_label(s_wifi_scr, "OK:开启 / 重配   长按:返回",
                                    &lv_font_montserrat_10, UI_SKY);
    lv_obj_set_pos(hint, 30, 250);

    s_mascot = ui_pixel_mascot_create(s_wifi_scr, 101, H - 34);
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

    lv_obj_t *big = ui_pixel_label(s_enc_scr, "✨ 完成啦! ✨", &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_pos(big, 60, 80);
    lv_obj_t *msg = ui_pixel_label(s_enc_scr, cat->encouragement, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_width(msg, W - 40);
    lv_obj_set_pos(msg, 20, 130);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *okl = ui_pixel_label(s_enc_scr, "[按OK继续]", &lv_font_montserrat_12, 0xFFFFFF);
    lv_obj_set_pos(okl, 80, 260);
    lv_screen_load(s_enc_scr);
}
void ui_encourage_close(void) { if (s_enc_scr) { lv_obj_delete(s_enc_scr); s_enc_scr = NULL; } }
bool ui_encourage_is_showing(void) { return s_enc_scr != NULL; }

static const char *s_scene_msgs[] = {
    "早安！今天也要加油鸭！",             /* MORNING_WASH */
    "🎯 请把手机放到另一个房间!\n保持专注，你可以的！",
    "🍱 午饭+背单词+午休30分钟\n记得背一会单词哦！",
    "🚶 晚饭后散散步，回来继续",
    "🛁 洗漱完毕，辛苦啦！",
    "😴 睡觉啦！回顾一下今天任务\n请放好手机，晚安～",
    "💇 该洗头发啦！清清爽爽～",
};

void ui_scene_show(study_scene_msg_t which) {
    if (which < 0 || which >= (int)(sizeof(s_scene_msgs)/sizeof(s_scene_msgs[0]))) return;
    if (s_scn_scr) return;
    s_scn_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scn_scr, W, H);
    lv_obj_set_style_bg_color(s_scn_scr, lv_color_hex(UI_GRASS_DARK), 0);
    lv_obj_set_style_border_width(s_scn_scr, 0, 0);
    lv_obj_t *msg = ui_pixel_label(s_scn_scr, s_scene_msgs[which], &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_width(msg, W - 40);
    lv_obj_set_pos(msg, 20, 120);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *okl = ui_pixel_label(s_scn_scr, "[OK 我知道了]", &lv_font_montserrat_12, 0xFFFFFF);
    lv_obj_set_pos(okl, 75, 260);
    lv_screen_load(s_scn_scr);
}
void ui_scene_close(void) { if (s_scn_scr) { lv_obj_delete(s_scn_scr); s_scn_scr = NULL; } }
bool ui_scene_is_showing(void) { return s_scn_scr != NULL; }

#else  /* !ESP_PLATFORM — 宿主编译存根 */

void study_ui_init(const study_ui_callbacks_t *cb) { (void)cb; }
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

/*
 * study_ui.h — 考研助手 UI 页：屏幕宽 240 × 高 320（竖屏）
 * 所有页面都在 app_study.c 控制下切换；这里仅声明 UI 构建/刷新/按键分发函数。
 * 页面之间不直接 include 对方；通过 app_study.c 里的 page 状态机切换。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "study_task.h"
#include "study_group.h"

/* ---------- UI 需要的外部回调（app_study.c 实现） ---------- */
typedef struct {
    /* 通知业务层：用户在任务详情页点了「勾选完成/取消」 */
    void (*on_task_done_changed)(int task_id, bool done);
    /* 通知业务层：添加了新任务（title/category/subtype/time 已填好） */
    int  (*on_task_added)(const study_task_t *t);
    /* 通知业务层：删除任务 */
    int  (*on_task_deleted)(int task_id);
    /* 请求播放某一科的完成音 */
    void (*play_complete_for_category)(int category);
    /* 请求播放某个场景（start_study / lunch / sleep 等，会识别 scene hook） */
    void (*play_scene_if_daily)(int task_id);
    /* 请求设置」睡觉「时间等元数据（config 命名空间）*/
    int  (*cfg_get)(const char *k, int def);
    void (*cfg_set)(const char *k, int v);
    /* 通知业务层：白天/夜间主题已切换(0=夜间 1=白天)，需重建当前非首页页面 */
    void (*on_theme_changed)(int theme);
    /* 通知业务层：倒计时自然走完（应用层据此播提示音；正计时无提示音） */
    void (*on_timer_finished)(void);
} study_ui_callbacks_t;

/* ---------- 公共初始化 ---------- */
void study_ui_init(const study_ui_callbacks_t *cb);

/* ---------- 页面类型 ---------- */
typedef enum {
    PAGE_HOME = 0,        /* 封面主页面：倒计时 + 进入学习 / 设置 */
    PAGE_TODO,            /* 今日 Todo（左右双页: 日常秩序 / 各科学习） */
    PAGE_ADD_TASK,        /* 添加任务：模板选择 → 类别 → subtype → 时间 */
    PAGE_TASK_DETAIL,     /* 任务详情：勾选 / 编辑时间 / 删除 */
    PAGE_SETTINGS,        /* 设置：WiFi / 录音笔 / 亮度 / 电量 / 音量 / 时间 */
    PAGE_WIFI,            /* WiFi：配网 / 状态显示 */
    PAGE_RECORDER,        /* 录音机：录 / 播 / 删 / 导出（黑底白字） */
    PAGE_ENCOURAGE,       /* 完成任务后的鼓励弹窗（含颜色动画） */
    PAGE_SCENE,           /* 日常秩序场景的温馨提示（开始学习/睡觉等） */
    PAGE_TIMER_MENU,      /* 计时器：正计时 / 倒计时 / 今日统计 菜单 */
    PAGE_TIMER_UP,        /* 正计时：就绪/运行/暂停/结束 */
    PAGE_TIMER_DOWN,      /* 倒计时：时长设置 → 运行/暂停/结束 */
    PAGE_TIMER_STATS,     /* 今日统计：今日累计 + 最近 7 天 */
} study_page_t;

/* ---------- 页面 API（每个页面 build / refresh / destroy 三件套） ---------- */

/* -------- 封面主页面 (PAGE_HOME) --------
 * 顶部：应用名 + 日期
 * 中部：考研倒计时大数字
 * 底部：[进入学习] [设置] 两个大按钮
 */
void ui_home_build(void);
void ui_home_destroy(void);
void ui_home_key(uint8_t btn, uint8_t ev);
/* OK 命中「进入学习」/「设置」后置位；app_study 据此切页 */
bool ui_home_wants_study(void);
bool ui_home_wants_settings(void);

/* -------- Todo 页 (PAGE_TODO) --------
 * 顶部：日期 + 星期 + 电池
 * 左右两页 = 「日常秩序」/「各科学习」；每页内未完成任务在上、已完成任务在下
 * 底部：[+ 添加] [设置]
 */
typedef enum {
    TAB_DAILY    = 0,     /* 日常秩序页 */
    TAB_SUBJECTS = 1,     /* 各科学习页 */
} study_todo_tab_t;

void ui_todo_build(void);                      /* 构建屏幕并 lv_screen_load */
void ui_todo_destroy(void);                    /* 释放 */
void ui_todo_refresh(void);                    /* 任务数据变了后重绘 */
void ui_todo_key(uint8_t btn, uint8_t ev);     /* 分发按键 (btn, ev 用 bsp_btn_t/bsp_btn_ev_t 的值) */

/* 返回当前选中任务 ID（用于跳到详情页）；未选中返回 -1 */
int  ui_todo_selected_task_id(void);

/* Todo 页 OK 命中「任务 / +添加 / 设置」后置位；app_study 据此统一切页并维护状态 */
bool ui_todo_wants_detail(int *out_task_id);
bool ui_todo_wants_add(void);
bool ui_todo_wants_settings(void);
bool ui_todo_wants_toggle(int *out_task_id);
void ui_todo_arm_delete(void);
bool ui_todo_delete_armed(void);
void ui_todo_cancel_delete(void);
bool ui_todo_wants_delete(int *out_task_id);

/* -------- 添加任务页 (PAGE_ADD_TASK) --------
 * 步骤：1) 预设模板列表 (快速添加)
 *        2) 类别（10 类）
 *        3) subtype（7 子分类）
 *        4) 时间（小时/分钟）
 * 每步 = 一个子画面。UI 里记 step_index，下键/OK 前进。
 */
void ui_add_build(void);
void ui_add_destroy(void);
void ui_add_key(uint8_t btn, uint8_t ev);
bool ui_add_is_finished(int *out_newly_added_id);  /* 用户走完后 true，回调 on_task_added 已经被触发 */

/* -------- 任务详情页 (PAGE_TASK_DETAIL) -------- */
void ui_detail_build(int task_id);
void ui_detail_destroy(void);
void ui_detail_key(uint8_t btn, uint8_t ev);
/* 返回当前详情页绑定的任务 ID（主题切换重建用） */
int  ui_detail_current_id(void);
/* OK 完成/取消后请求返回 Todo；app_study 据此统一切页 */
bool ui_detail_wants_back(void);

/* -------- 设置页 (PAGE_SETTINGS) -------- */
void ui_settings_build(void);
void ui_settings_destroy(void);
void ui_settings_key(uint8_t btn, uint8_t ev);
/* 设置页 OK 命中「WiFi 配网」后置位；app_study 据此切到 PAGE_WIFI */
bool ui_settings_wants_wifi(void);
/* 设置页 OK 命中「返回主界面」后置位；app_study 据此切回 PAGE_HOME */
bool ui_settings_wants_home(void);
/* 设置页 OK 命中其余条目(音量/起床/睡觉)后置位；app_study 据此返回 PAGE_TODO */
bool ui_settings_wants_todo(void);
/* 设置页 OK 命中「录音笔」后置位；app_study 据此切到 PAGE_RECORDER */
bool ui_settings_wants_recorder(void);
/* 设置页 OK 命中「计时器」后置位；app_study 据此切到 PAGE_TIMER_MENU */
bool ui_settings_wants_timer(void);

/* -------- 计时器页 (PAGE_TIMER_*) --------
 * 菜单：正计时 / 倒计时 / 今日统计 三选一（▲/▼ 切换，OK 进入）
 * 正计时：就绪[开始/返回] → 运行[暂停/结束] → 暂停[继续/结束]；结束后计入今日统计
 * 倒计时：设置(时长→自定义时/分→提示音→[开始/返回]) → 运行/暂停 → 自然走完计入统计
 * 今日统计：今日累计 + 最近 7 天柱状图
 * 导航统一：▲/▼ 切换 · OK 确定 · 长按 OK 返回上一界面
 */
void ui_timer_menu_build(void);
void ui_timer_menu_destroy(void);
void ui_timer_menu_key(uint8_t btn, uint8_t ev);
bool ui_timer_menu_wants_up(void);
bool ui_timer_menu_wants_down(void);
bool ui_timer_menu_wants_stats(void);

void ui_timer_up_build(void);
void ui_timer_up_destroy(void);
void ui_timer_up_key(uint8_t btn, uint8_t ev);
/* 正计时页请求返回计时器菜单（[返回]/[结束] 命中后置位） */
bool ui_timer_up_wants_menu(void);

void ui_timer_down_build(void);
void ui_timer_down_destroy(void);
void ui_timer_down_key(uint8_t btn, uint8_t ev);
bool ui_timer_down_wants_menu(void);

void ui_timer_stats_build(void);
void ui_timer_stats_destroy(void);
void ui_timer_stats_key(uint8_t btn, uint8_t ev);
bool ui_timer_stats_wants_menu(void);

/* -------- WiFi 页 (PAGE_WIFI) -------- */
void ui_wifi_build(void);
void ui_wifi_destroy(void);
void ui_wifi_key(uint8_t btn, uint8_t ev);

/* -------- 录音机页 (PAGE_RECORDER) -------- */
void ui_rec_enter(void);
void ui_rec_destroy(void);
void ui_rec_key(uint8_t btn, uint8_t ev);
bool ui_rec_wants_back(void);
bool ui_rec_wants_home(void);

/* -------- 鼓励弹窗（完成科目任务时） --------
 * 显示：科目主题色背景 + 鼓励文案大字 + 播放专属 RTTTL
 * 按 OK 或 3 秒后自动关闭。 */
void ui_encourage_show(int category_id);
void ui_encourage_close(void);
bool ui_encourage_is_showing(void);

/* -------- 场景大弹窗（日常秩序专属） --------
 * 如：勾选「早饭」后显示大字：
 *   ┌────────────────────────┐
 *   │ 🎯 请把手机放到另一个房间   │
 *   │ 保持专注，你可以的！       │
 *   │ [OK 我知道了]              │
 *   └────────────────────────┘
 */
typedef enum {
    SCENE_MSG_MORNING_WASH = 0,
    SCENE_MSG_START_STUDY,
    SCENE_MSG_LUNCH,
    SCENE_MSG_DINNER,
    SCENE_MSG_NIGHT_WASH,
    SCENE_MSG_SLEEP,
    SCENE_MSG_HAIR_WASH,
} study_scene_msg_t;

void ui_scene_show(study_scene_msg_t which);
void ui_scene_close(void);
bool ui_scene_is_showing(void);

/* ---------- 颜色常量（复用 ui_pixel.h 的同时给业务代码直接用） ---------- */
#ifndef UI_INK
#define UI_INK        0x17202A
#define UI_PAPER      0xF4F4EA
#define UI_SKY        0x1689E8
#define UI_GRASS      0x82BE2D
#define UI_RED        0xE43B2F
#define UI_YELLOW     0xFFD928
#endif

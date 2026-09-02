/*
 * study_category.c — 10 大类别完整数据 + RTTTL 旋律 + 鼓励文案
 * 与《考研日程助手设计方案.md》§二、§3.4 一一对应
 */
#include "study_category.h"
#include <stddef.h>
#include <string.h>

/*
 * RTTTL 旋律创作说明：
 *   格式: <name>:d=<默认时长>,o=<默认八度>,b=<BPM>:<音符列表>
 *   每首 1-2 秒，参考设计文档 §3.4 的风格描述
 *   全部采用可在蜂鸣器/ES8311 上清晰播放的音域（o=4..6，即 C4..B6）
 */
static const study_category_t s_categories[STUDY_CATEGORY_COUNT] = {
    /* ============================================================
     * 0. 日常秩序 — 温暖上行琶音 C→E→G→C，草绿
     * ============================================================ */
    {
        .id = CAT_DAILY,
        .name = "daily",
        .name_cn = "日常秩序",
        .color_hex = 0x82BE2D,
        .encouragement = "早安！崭新的一天开始啦，冲鸭！洗漱完毕立刻把手机放远一点哦，别让消息偷走你的专注时间，今天也一定能超额完成计划的！",
        .rtttl = "DailyCalm:d=8,o=5,b=120:c5,e5,g5,c6,p,c6,g5,e5,c5"
    },

    /* ============================================================
     * 1. 高等数学 — 激昂大调胜利号角，红
     *    特色关键词：洛必达、拉格朗日、极限、中值定理、∫积分、泰勒展开
     * ============================================================ */
    {
        .id = CAT_MATH,
        .name = "math",
        .name_cn = "高等数学",
        .color_hex = 0xE43B2F,
        .encouragement = "漂亮！又一道极限被洛必达法则拿下啦！泰勒展开、中值定理、二重积分，你正在一步步征服整个高数宇宙！再坚持一下，拉格朗日都会为你点赞的！",
        .rtttl = "MathFanfare:d=8,o=5,b=180:c5,e5,g5,c6,g5,c6,e6,c6,g5,e5,c5"
    },

    /* ============================================================
     * 2. 线性代数 — 简洁明快音阶跑动，橙
     * ============================================================ */
    {
        .id = CAT_LINEAR,
        .name = "linear",
        .name_cn = "线性代数",
        .color_hex = 0xFFB23E,
        .encouragement = "矩阵的世界你又前进了一步，向量空间等你征服～",
        .rtttl = "LinearScale:d=16,o=5,b=160:c5,d5,e5,f5,g5,a5,b5,c6,b5,a5,g5,f5,e5,d5,c5"
    },

    /* ============================================================
     * 3. 概率论 — 轻快跳跃断奏，黄
     * ============================================================ */
    {
        .id = CAT_PROB,
        .name = "prob",
        .name_cn = "概率论",
        .color_hex = 0xFFD928,
        .encouragement = "概率题搞定了！运气也是实力的一部分，但你靠的是真本事！",
        .rtttl = "ProbStaccato:d=16,o=5,b=160:g5,p,g5,p,a5,p,b5,p,c6,p,b5,p,a5,p,g5"
    },

    /* ============================================================
     * 4. 数据结构 — 科技感电子音，蓝
     * ============================================================ */
    {
        .id = CAT_DS,
        .name = "ds",
        .name_cn = "数据结构",
        .color_hex = 0x1689E8,
        .encouragement = "链表树图全拿捏，算法小能手就是你！",
        .rtttl = "DSTechno:d=8,o=6,b=140:c6,4g,c6,4g,e6,4g,c6,4g,d6,4f#,e6,4d#,c6"
    },

    /* ============================================================
     * 5. 计算机组成原理 — 沉稳有力低频节奏，紫
     * ============================================================ */
    {
        .id = CAT_CO,
        .name = "co",
        .name_cn = "计算机组成原理",
        .color_hex = 0x9B59B6,
        .encouragement = "CPU 内存总线都搞懂了，你就是行走的计算机组成书！",
        .rtttl = "CODeep:d=4,o=4,b=90:c4,g4,c4,e4,g4,2c4"
    },

    /* ============================================================
     * 6. 操作系统 — 多层节奏模拟调度，棕
     * ============================================================ */
    {
        .id = CAT_OS,
        .name = "os",
        .name_cn = "操作系统",
        .color_hex = 0x8B4513,
        .encouragement = "进程调度内存管理通关，OS 大师指日可待！",
        .rtttl = "OSLayered:d=8,o=5,b=110:4c5,4g4,4c5,4e5,4c5,4g5,4f5,4e5,4d5,4c5"
    },

    /* ============================================================
     * 7. 计算机网络 — 波动起伏波浪音，青
     * ============================================================ */
    {
        .id = CAT_NETWORK,
        .name = "network",
        .name_cn = "计算机网络",
        .color_hex = 0x1ABC9C,
        .encouragement = "七层模型了然于胸，网络世界任你遨游～",
        .rtttl = "NetWave:d=8,o=5,b=120:c5,d5,e5,d5,c5,d5,e5,f5,g5,f5,e5,d5,c5"
    },

    /* ============================================================
     * 8. 英语 — 轻柔优美旋律，粉
     * ============================================================ */
    {
        .id = CAT_ENGLISH,
        .name = "english",
        .name_cn = "英语",
        .color_hex = 0xFF6B9D,
        .encouragement = "单词量 +N！坚持下去，阅读速度会越来越快的！",
        .rtttl = "EngSoft:d=8,o=5,b=90:e5,g5,a5,g5,e5,2c6,g5,e5,d5,c5"
    },

    /* ============================================================
     * 9. 政治 — 庄严稳重进行曲风，深红
     * ============================================================ */
    {
        .id = CAT_POLITICS,
        .name = "politics",
        .name_cn = "政治",
        .color_hex = 0xC0392B,
        .encouragement = "政治知识点又巩固了，马原毛中特全都不在话下！",
        .rtttl = "PolMarch:d=4,o=4,b=80:c4,g4,c4,e4,f4,e4,d4,c4,g4,2c4"
    },
};

const study_category_t *study_category_get(int id) {
    if (id < 0 || id >= STUDY_CATEGORY_COUNT) return NULL;
    return &s_categories[id];
}

const study_category_t *study_category_find_by_name_cn(const char *name_cn) {
    if (!name_cn) return NULL;
    for (int i = 0; i < STUDY_CATEGORY_COUNT; i++) {
        if (strcmp(s_categories[i].name_cn, name_cn) == 0) {
            return &s_categories[i];
        }
    }
    return NULL;
}

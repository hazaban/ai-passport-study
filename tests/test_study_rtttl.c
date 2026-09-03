/*
 * Unit test for study_rtttl — 解析 RTTTL 字符串 → 生成 PCM 16kHz 16bit 方波
 *
 * 编译: gcc -I main/app_study -Wall -Wextra -o tests/test_study_rtttl \
 *              tests/test_study_rtttl.c main/app_study/study_rtttl.c -lm
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include "study_rtttl.h"
#include "study_category.h"

#define TEST_PASS()  do { printf("  PASS: %s\n", __func__); } while(0)

/* ---------- 测试 1: 头部解析 ---------- */
static void test_parse_header_basic(void) {
    /* 默认格式：d=4,o=5,b=140 */
    const char *rtttl = "BitJump:d=4,o=5,b=140:16g5";
    rtttl_defaults_t def;
    const char *notes_ptr = NULL;
    int rc = rtttl_parse_header(rtttl, &def, &notes_ptr);
    assert(rc == 0);
    assert(def.default_duration == 4);     /* 4 = 四分音符 */
    assert(def.default_octave == 5);
    assert(def.bpm == 140);
    assert(notes_ptr != NULL);
    assert(strcmp(notes_ptr, "16g5") == 0);
    TEST_PASS();
}

static void test_parse_header_no_defs(void) {
    /* Nokia 标准默认值：d=4, o=5, b=63 */
    const char *rtttl = "Simple:c,e,g:";   /* defs 段为空时按标准默认 */
    rtttl_defaults_t def;
    const char *np;
    int rc = rtttl_parse_header(rtttl, &def, &np);
    assert(rc == 0);
    assert(def.default_duration == 4);
    assert(def.default_octave == 5);
    assert(def.bpm == 63);
    TEST_PASS();
}

static void test_parse_header_invalid(void) {
    rtttl_defaults_t def;
    const char *np;
    /* 少一个冒号 */
    assert(rtttl_parse_header("Bad:no_notes", &def, &np) != 0);
    /* 完全坏的 */
    assert(rtttl_parse_header("", &def, &np) != 0);
    assert(rtttl_parse_header(NULL, &def, &np) != 0);
    TEST_PASS();
}

/* ---------- 测试 2: 音符频率计算 ---------- */
static void test_note_frequency_table(void) {
    /* 标准频率验证 (A4 = 440 Hz) */
    assert(rtttl_note_freq(4, NOTE_A) == 440);       /* A4 = 440 */
    assert(rtttl_note_freq(5, NOTE_A) == 880);       /* A5 = 880 */
    assert(rtttl_note_freq(4, NOTE_C) == 262);       /* C4 ≈ 261.63，取整 262 */
    assert(rtttl_note_freq(4, NOTE_C_SHARP) == 277); /* C#4 ≈ 277.18 */
    /* 休止符频率 = 0 */
    assert(rtttl_note_freq(4, NOTE_PAUSE) == 0);
    TEST_PASS();
}

/* ---------- 测试 3: 整曲解析 — 迭代每一个音符 ---------- */
static void test_iterate_simple_8bit(void) {
    /* BitJump 设计中的一个短片段：16分音符跑动 */
    const char *rtttl =
        "BitJump:d=4,o=5,b=140:16g5,16c6,16e6,8g6";

    rtttl_iter_t it;
    int rc = rtttl_iter_begin(&it, rtttl);
    assert(rc == 0);

    rtttl_note_t n;

    /* 第 1 个: 16 分 G5 */
    assert(rtttl_iter_next(&it, &n) == true);
    assert(n.octave == 5);
    assert(n.note == NOTE_G);
    assert(n.duration_code == 16);   /* 16 分音符 */
    assert(n.freq == rtttl_note_freq(5, NOTE_G));
    assert(n.ms > 0);
    double ms_per_beat = 60000.0 / 140.0;     /* ≈ 428.57 ms/四分音符 */
    double expected_16 = ms_per_beat / 4;     /* 16 分 = 1/4 个四分 ≈ 107ms */
    assert(fabs((double)n.ms - expected_16) < 2.0);  /* 误差 ≤ 2ms */

    /* 第 2 个: 16 分 C6 */
    assert(rtttl_iter_next(&it, &n) == true);
    assert(n.octave == 6);
    assert(n.note == NOTE_C);
    assert(n.duration_code == 16);

    /* 第 3 个: 16 分 E6 */
    assert(rtttl_iter_next(&it, &n) == true);
    assert(n.note == NOTE_E);
    assert(n.octave == 6);

    /* 第 4 个: 8 分 G6，时间应约为 16 分的 2 倍 */
    assert(rtttl_iter_next(&it, &n) == true);
    assert(n.note == NOTE_G);
    assert(n.octave == 6);
    assert(n.duration_code == 8);
    /* 8分 = 2 x 16分 ≈ 214ms */
    assert(n.ms > 180 && n.ms < 260);

    /* 没有更多音符 */
    assert(rtttl_iter_next(&it, &n) == false);
    TEST_PASS();
}

/* ---------- 测试 4: 休止符 p ---------- */
static void test_iterate_pause(void) {
    const char *rtttl = "Pause:d=8,o=5,b=120:c5,p,e5";
    rtttl_iter_t it;
    rtttl_iter_begin(&it, rtttl);

    rtttl_note_t n;
    assert(rtttl_iter_next(&it, &n) && n.note == NOTE_C);
    assert(rtttl_iter_next(&it, &n));
    assert(n.note == NOTE_PAUSE);
    assert(n.freq == 0);
    assert(n.ms > 0);
    assert(rtttl_iter_next(&it, &n) && n.note == NOTE_E);
    assert(rtttl_iter_next(&it, &n) == false);
    TEST_PASS();
}

/* ---------- 测试 5: PCM 合成（关键！因为要写进 I2S） ---------- */
static void test_render_pcm_sanity(void) {
    /* 一个很短的曲：只有一个 A4 四分音符 + 休止 */
    const char *rtttl = "Tone:d=4,o=4,b=60:a4,p";
    /* b=60 → 四分音符 = 1 秒 = 1000ms */

    /* 先看总时长 */
    int total_ms = rtttl_total_duration_ms(rtttl);
    /* A4 1000ms + pause 1000ms = 2000ms，允许±10ms取整误差 */
    assert(total_ms >= 1980 && total_ms <= 2020);

    /* 渲染 PCM：16000 Hz，16 bit mono，一次取 512 采样 */
    rtttl_player_t p;
    int rc = rtttl_player_begin(&p, rtttl, 16000);
    assert(rc == 0);

    int16_t buf[512];
    int total_samples = 0;
    bool saw_nonzero = false;
    bool saw_zero_region = false;       /* pause 时应该纯 0 */
    int got;
    while ((got = rtttl_player_render(&p, buf, 512)) > 0) {
        for (int i = 0; i < got; i++) {
            if (buf[i] != 0) saw_nonzero = true;
            /* 在 pause 段应当有一大段 0，但我们不知道精确的采样位置。
             * 换个简单断言：方波幅度应该固定（±AMP）或 0 */
            if (buf[i] != 0) {
                int v = buf[i];
                assert(v == 4096 || v == -4096);  /* 方波 ±4096 */
            }
        }
        total_samples += got;
        /* 检查 pause 段：大约 16000 附近之后应该有 0 区 */
        if (total_samples > 16000) {
            bool all_zero = true;
            for (int i = 0; i < got; i++)
                if (buf[i] != 0) { all_zero = false; break; }
            if (all_zero) saw_zero_region = true;
        }
    }

    /* 预期样本数 ≈ 16kHz × 2s = 32000 */
    assert(total_samples >= 31800 && total_samples <= 32200);
    assert(saw_nonzero);
    assert(saw_zero_region);
    TEST_PASS();
}

/* ---------- 测试 6: 类别 RTTTL 全部合法可解析 ---------- */
static void test_all_category_rtttls_parseable(void) {
    for (int i = 0; i < STUDY_CATEGORY_COUNT; i++) {
        const char *r = study_category_get(i)->rtttl;
        rtttl_defaults_t def;
        const char *np;
        assert(rtttl_parse_header(r, &def, &np) == 0);
        assert(def.bpm > 0 && def.bpm < 500);
        assert(def.default_octave >= 4 && def.default_octave <= 7);

        /* 至少能迭代出一个音符 */
        rtttl_iter_t it;
        assert(rtttl_iter_begin(&it, r) == 0);
        rtttl_note_t n;
        bool any = false;
        while (rtttl_iter_next(&it, &n)) any = true;
        assert(any);
    }
    TEST_PASS();
}

int main(void) {
    printf("=== study_rtttl unit tests ===\n");
    test_parse_header_basic();
    test_parse_header_no_defs();
    test_parse_header_invalid();
    test_note_frequency_table();
    test_iterate_simple_8bit();
    test_iterate_pause();
    test_render_pcm_sanity();
    test_all_category_rtttls_parseable();
    printf("All study_rtttl tests PASSED.\n");
    return 0;
}

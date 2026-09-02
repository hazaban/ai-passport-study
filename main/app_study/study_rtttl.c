/*
 * study_rtttl.c — RTTTL 解析 + PCM 方波合成
 */
#include "study_rtttl.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* --------------- 频率表 ---------------
 * A4 = 440 Hz；各半音频率 = 440 × 2^(step/12)，提前算好取整。
 * step: 以 A4 (step=0) 为基准，C4=-9, C#4=-8, ... B5=14, C6=15 ...
 */
static const int16_t s_freq_table[4][13] = {
    /* octave 4: C..B + PAUSE(=0 占位) */
    /*   0     C    C#    D     D#   E     F     F#    G     G#    A     A#    B  */
    {   0,   262,  277,  294,  311,  330,  349,  370,  392,  415,  440,  466,  494 },
    /* octave 5 */
    {   0,   523,  554,  587,  622,  659,  698,  740,  784,  831,  880,  932,  988 },
    /* octave 6 */
    {   0,  1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976 },
    /* octave 7 */
    {   0,  2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951 },
};
#define OCT_MIN 4
#define OCT_MAX 7

int rtttl_note_freq(int octave, rtttl_pitch_t pitch) {
    if (pitch == NOTE_PAUSE) return 0;
    if (pitch < NOTE_C || pitch > NOTE_B) return 0;
    if (octave < OCT_MIN) octave = OCT_MIN;
    if (octave > OCT_MAX) octave = OCT_MAX;
    return s_freq_table[octave - OCT_MIN][pitch];
}

/* --------------- header 解析 --------------- */
static const char *find_nth_colon(const char *s, int n) {
    if (!s) return NULL;
    int count = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ':') {
            if (++count == n) return p;
        }
    }
    return NULL;
}

/* 解析 defs 子串，形如 "d=4,o=5,b=140"（缺省项按 Nokia 标准默认） */
static void parse_defaults(const char *start, const char *end, rtttl_defaults_t *d) {
    /* Nokia 标准默认 */
    d->default_duration = 4;
    d->default_octave   = 5;
    d->bpm              = 63;

    const char *p = start;
    while (p < end) {
        /* skip whitespace / comma */
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t')) p++;
        if (p >= end) break;
        char key = (char)tolower((unsigned char)*p++);
        while (p < end && *p != '=') p++;
        if (*p != '=') break;
        p++;  /* skip '=' */
        int val = 0;
        while (p < end && isdigit((unsigned char)*p)) {
            val = val * 10 + (*p - '0');
            p++;
        }
        switch (key) {
            case 'd': if (val > 0) d->default_duration = val; break;
            case 'o': if (val >= OCT_MIN && val <= OCT_MAX) d->default_octave = val; break;
            case 'b': if (val >= 10 && val <= 500) d->bpm = val; break;
        }
    }
}

int rtttl_parse_header(const char *rtttl, rtttl_defaults_t *out_defs,
                       const char **out_notes_ptr) {
    if (!rtttl || !out_defs) return -1;
    const char *c1 = find_nth_colon(rtttl, 1);
    const char *c2 = find_nth_colon(rtttl, 2);
    if (!c1 || !c2 || c2 <= c1) return -1;

    parse_defaults(c1 + 1, c2, out_defs);
    if (out_notes_ptr) *out_notes_ptr = c2 + 1;
    return 0;
}

/* --------------- 单音符解析 --------------- */
static rtttl_pitch_t char_to_pitch(char c, bool sharp) {
    switch (tolower((unsigned char)c)) {
        case 'c': return sharp ? NOTE_C_SHARP : NOTE_C;
        case 'd': return sharp ? NOTE_D_SHARP : NOTE_D;
        case 'e': return NOTE_E;
        case 'f': return sharp ? NOTE_F_SHARP : NOTE_F;
        case 'g': return sharp ? NOTE_G_SHARP : NOTE_G;
        case 'a': return sharp ? NOTE_A_SHARP : NOTE_A;
        case 'b': return NOTE_B;
        case 'p': return NOTE_PAUSE;
        default:  return NOTE_PAUSE;
    }
}

static int note_ms(int bpm, int duration_code, bool dotted) {
    /* 一个四分音符(四分音符 duration_code=4)的毫秒 = 60000 / bpm */
    double beat_ms = 60000.0 / (double)bpm;
    /* duration_code=1 全音符=4拍, 2二分=2拍, 4四分=1拍, 8八分=0.5拍... */
    double beats = 4.0 / (double)duration_code;
    double ms = beats * beat_ms;
    if (dotted) ms *= 1.5;
    return (int)(ms + 0.5);
}

int rtttl_iter_begin(rtttl_iter_t *it, const char *rtttl) {
    if (!it) return -1;
    memset(it, 0, sizeof(*it));
    const char *np;
    if (rtttl_parse_header(rtttl, &it->defs, &np) != 0) return -1;
    it->cursor = np;
    it->done = false;
    return 0;
}

bool rtttl_iter_next(rtttl_iter_t *it, rtttl_note_t *out) {
    if (!it || it->done || !out) return false;
    const char *p = it->cursor;
    if (!p) goto END_FALSE;

    /* skip leading commas / spaces */
    while (*p == ',' || *p == ' ' || *p == '\t') p++;
    if (*p == '\0') goto END_FALSE;

    /* 1) duration (可选，数字) */
    int dur = 0;
    while (isdigit((unsigned char)*p)) {
        dur = dur * 10 + (*p - '0');
        p++;
    }
    if (dur == 0) dur = it->defs.default_duration;

    /* 2) pitch letter (or p for pause) */
    char pitch_c = *p++;
    bool sharp = false;
    if (*p == '#' || tolower((unsigned char)*p) == 'h') { sharp = true; p++; }

    rtttl_pitch_t pitch;
    if (tolower((unsigned char)pitch_c) == 'p') {
        pitch = NOTE_PAUSE; sharp = false;
    } else {
        pitch = char_to_pitch(pitch_c, sharp);
    }

    /* 3) octave (可选) */
    int oct = 0;
    while (isdigit((unsigned char)*p)) {
        oct = oct * 10 + (*p - '0');
        p++;
    }
    if (oct == 0) oct = it->defs.default_octave;
    if (pitch == NOTE_PAUSE) oct = it->defs.default_octave;  /* 不重要，给个默认 */

    /* 4) dotted */
    bool dotted = false;
    if (*p == '.') { dotted = true; p++; }

    /* 下次起始 = p */
    it->cursor = p;

    out->note = pitch;
    out->octave = oct;
    out->duration_code = dur;
    out->dotted = dotted;
    out->freq = rtttl_note_freq(oct, pitch);
    out->ms = note_ms(it->defs.bpm, dur, dotted);
    return true;

END_FALSE:
    it->done = true;
    return false;
}

/* --------------- 整曲总时长 --------------- */
int rtttl_total_duration_ms(const char *rtttl) {
    rtttl_iter_t it;
    if (rtttl_iter_begin(&it, rtttl) != 0) return -1;
    int total = 0;
    rtttl_note_t n;
    while (rtttl_iter_next(&it, &n)) total += n.ms;
    return total;
}

/* --------------- PCM 流式合成 --------------- */

/* 推进到「下一个音符」。如果已经是最后一个，标记 song_done=true。 */
static bool player_next_note(rtttl_player_t *p) {
    rtttl_note_t n;
    if (!rtttl_iter_next(&p->it, &n)) {
        p->song_done = true;
        return false;
    }
    /* 这个音符的总采样数 */
    int samples_per_sec = p->sample_rate;
    p->remain_samples = (int)((int64_t)n.ms * samples_per_sec / 1000);
    /* 方波半周期 = samples_per_sec / (2 * freq) */
    if (n.freq > 0) {
        p->half_period = samples_per_sec / (2 * n.freq);
        if (p->half_period < 1) p->half_period = 1;
    } else {
        p->half_period = 0;
    }
    p->phase = 0;
    p->current_high = true;   /* 每个音符从高电平开始，听感更干脆 */
    return true;
}

int rtttl_player_begin(rtttl_player_t *p, const char *rtttl, int sample_rate) {
    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    if (rtttl_iter_begin(&p->it, rtttl) != 0) return -1;
    p->sample_rate = sample_rate > 0 ? sample_rate : 16000;
    if (!player_next_note(p)) return -1;     /* 空曲 */
    return 0;
}

int rtttl_player_render(rtttl_player_t *p, int16_t *out_buf, int max_samples) {
    if (!p || !out_buf || max_samples <= 0 || p->song_done) return 0;
    int wrote = 0;

    while (wrote < max_samples) {
        if (p->remain_samples <= 0) {
            /* 当前音符用完，进入下一个 */
            if (!player_next_note(p)) break;   /* 真的结束了 */
            continue;
        }

        int chunk = (max_samples - wrote) < p->remain_samples
                    ? (max_samples - wrote) : p->remain_samples;

        if (p->half_period == 0) {
            /* 休止：全部填 0 */
            for (int i = 0; i < chunk; i++) out_buf[wrote + i] = 0;
        } else {
            /* 方波：电平按 half_period 周期翻转 */
            int16_t amp = RTTTL_PCM_AMPLITUDE;
            bool hi = p->current_high;
            int phase = p->phase;
            int hp = p->half_period;
            for (int i = 0; i < chunk; i++) {
                out_buf[wrote + i] = hi ? amp : (int16_t)-amp;
                if (++phase >= hp) {
                    phase = 0;
                    hi = !hi;
                }
            }
            p->current_high = hi;
            p->phase = phase;
        }
        wrote += chunk;
        p->remain_samples -= chunk;
    }
    return wrote;
}

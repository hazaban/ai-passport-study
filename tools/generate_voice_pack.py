#!/usr/bin/env python3
"""
generate_voice_pack.py — 生成水豚噜噜完整语音包
数据源:  考研日程助手设计方案 §3.2 固定场景 + §二 10 科完成鼓励语
TTS 引擎: Edge-TTS (Microsoft 免费在线服务)
音色:    zh-CN-XiaoxiaoNeural  (温柔女声)，+pitch +25Hz  +rate +10% 模拟水豚的可爱感
输出:    assets/voice_packs/capybara_lulu/*.mp3  (原始 MP3)
         + 若 ffmpeg 可用则同时输出 16kHz 16-bit mono WAV (便于 ADPCM 编码)

用法: python3 tools/generate_voice_pack.py
"""
import asyncio
import json
import os
import sys
import subprocess
from pathlib import Path

try:
    import edge_tts
except ImportError:
    sys.exit("Please `pip install edge-tts` first.")

OUT_DIR = Path(__file__).resolve().parent.parent / "assets" / "voice_packs" / "capybara_lulu"
OUT_DIR.mkdir(parents=True, exist_ok=True)

VOICE = "zh-CN-XiaoxiaoNeural"
PITCH = "+25Hz"    # 提升音调 = 更可爱的水豚感
RATE  = "+10%"     # 稍快 = 活泼
VOLUME = "+0%"

# ---------------------------------------------------------------------------
# 语音清单 — 与设计方案保持一致。
# 键 = SPIFFS 文件名（.mp3 会转成 .adpcm），值 = 朗读文本
# ---------------------------------------------------------------------------
VOICE_LINES = {
    # ---------- 日常秩序固定场景 (§3.2) ----------
    "morning_wash":
        "早安！崭新的一天开始啦，冲鸭！洗漱完毕立刻把手机放远一点哦，别让消息偷走你的专注时间，今天也一定能超额完成计划的！",
    "start_study":
        "学习时间到啦！请把手机放到另一个房间，保持专注，你可以的！",
    "lunch":
        "午饭吃得香，下午才有劲～饭后可以背一会儿单词哦，建议午休三十分钟，养足精神再战！",
    "dinner":
        "晚饭时间到！劳逸结合，吃完出去散散步，回来继续加油～",
    "night_wash":
        "洗漱完毕，一天的辛苦结束啦，准备好休息了吗？",
    "sleep":
        "睡觉啦～先回顾一下今天完成了哪些任务，是不是很有成就感？"
        "请把手机放到另一个房间，好的睡眠才是新一天的开始哦，晚安～",
    "hair_wash":
        "该洗头发啦～清清爽爽，学习效率更高！",
    "hair_remind":
        "洗头发提醒，今天该洗头啦～距离上次已经整整七天啦，"
        "清清爽爽地迎接新的一周，好运自然来哦！",

    # ---------- 10 科完成鼓励语 (§二) ----------
    "complete_daily":
        "生活节奏稳稳的，学习才能更高效哦～",
    "complete_math":
        "漂亮！又一道极限被洛必达法则拿下啦！泰勒展开、中值定理、二重积分，你正在一步步征服整个高数宇宙！再坚持一下，拉格朗日都会为你点赞的！",
    "complete_linear":
        "矩阵的世界你又前进了一步，向量空间等你征服～",
    "complete_prob":
        "概率题搞定了！运气也是实力的一部分，但你靠的是真本事！",
    "complete_ds":
        "链表树图全拿捏，算法小能手就是你！",
    "complete_co":
        "CPU 内存总线都搞懂了，你就是行走的计算机组成书！",
    "complete_os":
        "进程调度内存管理通关，OS 大师指日可待！",
    "complete_network":
        "七层模型了然于胸，网络世界任你遨游～",
    "complete_english":
        "单词量加N！坚持下去，阅读速度会越来越快的！",
    "complete_politics":
        "政治知识点又巩固了，马原毛中特全都不在话下！",

    # ---------- ★ 英语专属：子分类级定制语音 ----------
    "english_recite_words":
        "太棒啦！又记住了一大波单词！每一个拼写都刻进脑子里，"
        "abandon 已经离你远去啦，明天的你词汇量又要破纪录哦！",
    "eng_reading":
        "长难句拆解成功！主旨题、细节题、推理题全部拿下！"
        "多读一篇阅读，考场上就多一份从容，继续刷真题阅读吧，语感正在飞速上涨！",

    # ---------- 自定义预留 ----------
    "custom_1": "自定义提醒响铃啦，请查看你的任务哦～",
    "custom_2": "别忘了该完成的事情呀，加油加油！",
    "custom_3": "叮咚～你设置的提醒时间到啦！",
    "custom_4": "学习真的辛苦啦，休息一下眼睛再继续吧～",
    "custom_5": "打卡成功！离上岸又近了一步呢！",
}


def has_ffmpeg():
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except Exception:
        return False


async def gen_one(key: str, text: str) -> None:
    mp3_path = OUT_DIR / f"{key}.mp3"
    if mp3_path.exists() and mp3_path.stat().st_size > 2048:
        print(f"  skip {key}: already exists")
        return
    print(f"  -> {key} ({len(text)} chars)")
    communicate = edge_tts.Communicate(text, VOICE, rate=RATE, pitch=PITCH, volume=VOLUME)
    await communicate.save(str(mp3_path))
    print(f"     saved: {mp3_path.name} ({mp3_path.stat().st_size} bytes)")

    # 若 ffmpeg 可用，同步转成 16kHz 16-bit mono WAV（后续 ADPCM 编码的原料）
    if has_ffmpeg():
        wav_path = OUT_DIR / f"{key}.wav"
        subprocess.run([
            "ffmpeg", "-y", "-i", str(mp3_path),
            "-ac", "1", "-ar", "16000", "-sample_fmt", "s16",
            str(wav_path)
        ], capture_output=True, check=True)


async def main() -> None:
    print(f"Generating voice pack → {OUT_DIR}")
    print(f"Voice: {VOICE}  pitch={PITCH}  rate={RATE}")
    total = len(VOICE_LINES)
    done = 0
    for k, t in VOICE_LINES.items():
        try:
            await gen_one(k, t)
            done += 1
        except Exception as e:
            print(f"  !! FAIL {k}: {e}")
    print(f"\nDone: {done}/{total} lines.")

    # 写 pack_info.json
    info = {
        "name": "水豚噜噜 (Capybara Lulu)",
        "id": "capybara_lulu",
        "tts_engine": "edge-tts " + VOICE,
        "pitch": PITCH,
        "rate": RATE,
        "version": "1.0.0",
        "lines": VOICE_LINES,
    }
    info_path = OUT_DIR / "pack_info.json"
    info_path.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Pack info → {info_path}")


if __name__ == "__main__":
    asyncio.run(main())

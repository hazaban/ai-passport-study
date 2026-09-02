#!/usr/bin/env python3
"""
build_voice_spiffs.py — 把「水豚噜噜」语音包转成 SPIFFS 镜像所需的文件布局

用途:  考研助手在 ESP32 上通过 SPIFFS 读取 ~/voicepack/<voice_key>.wav 播放人声。
       本脚本把 assets/voice_packs/capybara_lulu/*.mp3 统一转成
       16kHz / 16bit / mono 的 WAV，放到 build/voicepack/。
       之后 `idf.py build` 会自动把该目录打成 voicepack 分区镜像并纳入烧录。

依赖:  ffmpeg 必须在 PATH。
用法:  python3 tools/build_voice_spiffs.py
       可选: --out <dir>  (默认 build/voicepack)
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "assets" / "voice_packs" / "capybara_lulu"
DEF_OUT = ROOT / "build" / "voicepack"

# 每次构建都强制重转，避免改文案后漏更新（转换便宜）。
# 想只补单条时可用 --key wakeup_alarm 只转一条。
def to_wav(mp3: Path, out_dir: Path, key: str) -> None:
    out = out_dir / f"{key}.wav"
    subprocess.run([
        "ffmpeg", "-y", "-i", str(mp3),
        "-ac", "1", "-ar", "16000", "-sample_fmt", "s16",
        str(out),
    ], capture_output=True, check=True)
    print(f"  OK  {key:28s} {out.stat().st_size:8d} B")

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(DEF_OUT), help="WAV 输出目录")
    ap.add_argument("--key", default=None, help="只转换指定 key(缺省全部)")
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not shutil_ffmpeg():
        print("需要 ffmpeg：请先安装并加入 PATH。", file=sys.stderr)
        return 1

    mp3s = sorted(SRC_DIR.glob("*.mp3"))
    if not mp3s:
        print(f"在 {SRC_DIR} 没找到 .mp3，请先跑 tools/generate_voice_pack.py。",
              file=sys.stderr)
        return 1

    n_ok = 0
    for mp3 in mp3s:
        key = mp3.stem
        if args.key and key != args.key:
            continue
        try:
            to_wav(mp3, out_dir, key)
            n_ok += 1
        except subprocess.CalledProcessError as e:
            print(f"  FAIL {mp3.name}: {e.stderr.decode(errors='replace')[-200:]}",
                  file=sys.stderr)
    print(f"\n完成 {n_ok}/{len(mp3s)} 条 → {out_dir}")
    return 0 if n_ok else 1

def shutil_ffmpeg() -> bool:
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except Exception:
        return False

if __name__ == "__main__":
    sys.exit(main())
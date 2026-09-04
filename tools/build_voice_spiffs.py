#!/usr/bin/env python3
"""
build_voice_spiffs.py — 把语音包转成 16kHz/单声道 Opus 裸流（.frc），供设备解码播放。

文件格式与参考「音效钥匙扣」一致：每包 = u16LE(包长) + Opus 帧（20ms，无任何容器头）。
用途：读 assets/voice_packs/capybara_lulu/*.mp3 → 生成 build/voicepack/<key>.frc，
      之后 `idf.py build` 自动打成 voicepack SPIFFS 镜像并纳入烧录。
依赖：ffmpeg（须含 libopus）。
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "assets" / "voice_packs" / "capybara_lulu"
DEF_OUT = ROOT / "build" / "voicepack"

RATE = 16000
BITRATE_K = 20          # 20kbps：语音清晰度足够且体积小


def ogg_to_raw_packets(ogg: bytes) -> bytes:
    """极简 Ogg 解包：把每个音频包还原为 u16LE 长度 + Opus 帧（丢弃 OpusHead）。"""
    out = bytearray()
    i = 0
    while i + 27 <= len(ogg):
        if ogg[i:i + 4] != b"OggS":
            break
        nsegs = ogg[i + 26]
        i += 27
        if i + nsegs > len(ogg):
            break
        lacing = ogg[i:i + nsegs]
        i += nsegs
        pkt = b""
        for lv in lacing:
            pkt += ogg[i:i + lv]
            i += lv
            if lv < 255:
                if (pkt[:8] != b"OpusHead" and pkt[:8] != b"OpusTags"
                        and pkt[:7] != b"\x01vorbis"):
                    out += struct.pack("<H", len(pkt))
                    out += pkt
                pkt = b""
    return bytes(out)


def to_frc(mp3: Path, out_dir: Path, key: str) -> None:
    out = out_dir / f"{key}.frc"
    p = subprocess.run(
        ["ffmpeg", "-y", "-i", str(mp3), "-ac", "1", "-ar", str(RATE),
         "-c:a", "libopus", "-application", "voip", "-b:a", f"{BITRATE_K}k",
         "-frame_duration", "20", "-f", "ogg", "-"],
        capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.decode(errors="replace")[-200:])
    raw = ogg_to_raw_packets(p.stdout)
    if not raw:
        raise RuntimeError("no opus packets")
    out.write_bytes(raw)
    print(f"  OK  {key:28s} {out.stat().st_size:8d} B")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(DEF_OUT), help=".frc 输出目录")
    ap.add_argument("--key", default=None, help="只转换指定 key(缺省全部)")
    args = ap.parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
    except Exception:
        print("需要 ffmpeg：请先安装并加入 PATH。", file=sys.stderr)
        return 1

    mp3s = sorted(SRC_DIR.glob("*.mp3"))
    if not mp3s:
        print(f"在 {SRC_DIR} 没找到 .mp3。", file=sys.stderr)
        return 1

    n_ok = 0
    for mp3 in mp3s:
        key = mp3.stem
        if args.key and key != args.key:
            continue
        try:
            to_frc(mp3, out_dir, key)
            n_ok += 1
        except Exception as e:  # noqa: BLE001
            print(f"  FAIL {mp3.name}: {e}", file=sys.stderr)
    print(f"\n完成 {n_ok}/{len(mp3s)} 条 → {out_dir}")
    return 0 if n_ok else 1


if __name__ == "__main__":
    sys.exit(main())

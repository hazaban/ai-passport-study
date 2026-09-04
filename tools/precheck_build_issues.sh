#!/usr/bin/env bash
# 推送前预检已知会让 CI(-Werror/头文件)失败的问题，命中即退出非 0。
# 用法: bash tools/precheck_build_issues.sh   (在仓库根目录运行)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
files=$(git status --porcelain | awk '{print $2}' | grep -E '\.(c|h)$')
[ -z "$files" ] && echo "no changed C/H files" && exit 0

fail=0
for f in $files; do
  [ -f "$f" ] || continue
  # 1) 不存在的头（esp_vfs_spiffs.h 不是公开头，应用 esp_spiffs.h）
  if grep -n 'esp_vfs_spiffs\.h' "$f"; then echo "!! $f 用了 esp_vfs_spiffs.h（应改 esp_spiffs.h）"; fail=1; fi
  # 2) 未初始化 int r; 等（CI -Werror=maybe-uninitialized）
  if grep -nE '^[[:space:]]*int (r|ret|n)[[:space:]]*;' "$f" | grep -v '= '; then echo "!! $f 疑似未初始化 int（maybe-uninitialized）"; fail=1; fi
  # 3) 单行 if 后紧跟另一条 if 同一行（misleading-indentation）
  if grep -nE 'if \([^)]*\) [^;]+; if \(' "$f"; then echo "!! $f 同行两个 if（misleading-indentation）"; fail=1; fi
  # 4) 小缓冲(<16) + 数字格式化：%d%% / %02d:%02d / %lu（format-truncation 出现过 2 次）
  if grep -nE 'char [a-z_]+\[([0-9]|1[0-5])\]' "$f" >/dev/null && \
     grep -nE 'snprintf\([^;]*(%d%%|%02?d:|%lu)' "$f" >/dev/null; then
     echo "!! $f 小缓冲 + 数字格式化 可能 format-truncation"; fail=1
  fi
done
# 5) use-before-declaration（study_voice：s_stop/s_playing 须在 play_wav_blocking 前声明）
vf=main/app_study/study_voice.c
if [ -f "$vf" ]; then
  dl=$(grep -nE 'static volatile bool s_(stop|playing)' "$vf" | head -1 | cut -d: -f1)
  ul=$(grep -n 'play_wav_blocking' "$vf" | head -1 | cut -d: -f1)
  if [ -n "$dl" ] && [ -n "$ul" ] && [ "$dl" -gt "$ul" ]; then
     echo "!! $vf: s_stop/s_playing 声明在 play_wav_blocking 之后(use-before-declaration)"; fail=1
  fi
fi
if [ $fail -eq 0 ]; then echo "precheck OK"; else echo "precheck FAILED（见上）"; fi
exit $fail

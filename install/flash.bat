@echo off
REM ============================================================
REM  考研日程助手 · ESP32-C3  一键烧录脚本 (Windows)
REM  用法: 把本文件夹里所有文件拷到 D:\Aa考研\esp 后双击本脚本
REM  首次需安装 esptool (只需一次):  pip install esptool
REM  设备连接 USB, 按住 BOOT 键再点击复位进入下载模式
REM ============================================================
set PORT=COM5
if not "%1"=="" set PORT=%1

echo 使用端口: %PORT%
echo 开始烧录...(首次会连刷 4 个分区, 含语音包)
esptool --chip esp32c3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m ^
  0x0 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 FoloToy-AI-Passport.bin ^
  0x35a000 voicepack.bin

if errorlevel 1 (
  echo.
  echo 烧录失败。请检查:
  echo  1) 端口号是否正确(可用 flash.bat COM5 指定)
  echo  2) 是否已装 esptool (pip install esptool)
  echo  3) 设备是否已进入下载模式
) else (
  echo.
  echo 烧录成功！设备已重启, 将直接进入考研日程助手。
)
pause
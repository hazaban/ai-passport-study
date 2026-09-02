idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         # ----- 考研助手 · 新增 (app_study/ 子模块) -----
         "app_study/app_study.c"
         "app_study/study_ui.c"
         "app_study/study_task.c"
         "app_study/study_task_nvs.c"
         "app_study/study_scheduler.c"
         "app_study/study_rtttl.c"
         "app_study/study_voice.c"
         "app_study/study_group.c"
         "app_study/study_category.c"
         "app_study/study_time.c"        # SNTP 时间源（闹钟/到点到点用）
         "app_study/study_wifi.c"        # WiFi 连接 + 本地网页配网 (SoftAP+HTTP)
         "app_study/study_font.c"        # 内嵌中文字库（LVGL 字体子集）
    INCLUDE_DIRS "."
                 "app_study"
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi
             nvs_flash spiffs lwip esp_http_server json
)

# ----- 考研助手：把 build/voicepack 目录打成 voicepack 分区的 SPIFFS 镜像并纳入烧录 -----
# 先用 python tools/build_voice_spiffs.py 生成 *.wav 到 build/voicepack，然后 idf.py build
# 会在编译期自动生成 voicepack.bin 镜像，idf.py flash 时一并写进 0x35A000 分区。
if(EXISTS ${CMAKE_SOURCE_DIR}/build/voicepack)
    spiffs_create_partition_image(voicepack ${CMAKE_SOURCE_DIR}/build/voicepack FLASH_IN_PROJECT)
endif()

# ============================================================================
# src/CMakeLists.txt - 应用源文件与组件依赖声明
#
#   源文件：自动收编 src/ 下全部 .c 与 .S（新增源文件无需改这里）
#   组件依赖 REQUIRES：spiffs(名片存储)、console(串口命令)、esp_wifi/esp_netif/
#     esp_event/esp_timer(ESP-NOW)、nvs_flash(档案与冷却)
#
# 可调整内容：
#   - 新增 IDF 组件在这里追加 REQUIRES（SD 卡已接入：fatfs+sdmmc+esp_driver_sdspi）。
#   - GLOB 只匹配 .c/.S：资源文件（mjpeg/图片）不会被误当源码编译。
# ============================================================================
# This file was automatically generated for projects
# without default 'CMakeLists.txt' file. (touch: uname_18/14)

# 只匹配 .c/.S 源文件（避免把 resources 下的资源当作源码编译）
# 注意：GLOB 在 CMake 配置时求值；gen_fonts 新生成字库后，需让 CMake
# 重新配置（如触摸本文件）才会收编，否则报 undefined reference。
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
FILE(GLOB_RECURSE app_asm ${CMAKE_SOURCE_DIR}/src/*.S)

idf_component_register(
    SRCS ${app_sources} ${app_asm}
    INCLUDE_DIRS "." "nfc" "ui" "bsp" "mjpeg_player"
    REQUIRES spiffs console esp_wifi nvs_flash esp_netif esp_event esp_timer fatfs sdmmc esp_driver_sdspi
)
# ip_config.c GLOB 收录

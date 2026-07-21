# ============================================================
#   Collect source files
# ============================================================

file(GLOB_RECURSE ui_srcs       "ui/*.c")
file(GLOB_RECURSE usb_srcs      "usb/*.c")
file(GLOB_RECURSE ble_srcs      "ble/*.c")
file(GLOB_RECURSE fs_manager    "fs_manager/*.c")
file(GLOB_RECURSE anim_srcs     "anim/*.c")
file(GLOB_RECURSE nvs_manager   "nvs_manager/*.c")
file(GLOB_RECURSE ota_manager   "ota_manager/*.c")

# ============================================================
#   Component sources
# ============================================================

set(COMPONENT_SRCS
    "main.c"
    "lvgl_port.c"
    "waveshare_rgb_lcd_port.c"

    ${ui_srcs}
    ${usb_srcs}
    ${ble_srcs}
    ${fs_manager}
    ${anim_srcs}
    ${nvs_manager}
    ${ota_manager}
)

# ============================================================
#   Include directories
# ============================================================

set(COMPONENT_ADD_INCLUDEDIRS
    .
    "ui"
    "usb"
    "ble"
    "fs_manager"
    "anim"
    "nvs_manager"
    "ota_manager"
)

# ============================================================
#   Register component
# ============================================================

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS ${COMPONENT_ADD_INCLUDEDIRS}
    REQUIRES
        esp_lcd
        fatfs
        sdmmc
        driver
        spi_flash
        esp_partition
        wear_levelling
        espressif__esp_tinyusb
        espressif__esp_new_jpeg
        json
        nvs_flash
        app_update
        bt
)

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)

idf_component_get_property(tusb_lib espressif__esp_tinyusb COMPONENT_LIB)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_tinyusb/include_private"
)
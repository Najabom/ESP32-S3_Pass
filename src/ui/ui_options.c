/* ============================================================================
 * ui_options.c - 界面显示开关（NVS 持久化位掩码）
 * ==========================================================================*/
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "ui_options.h"

static const char *TAG = "ui_options";

#define NVS_NS_UIOPT   "uiopt"
#define NVS_KEY_MASK   "m"

static bool   s_loaded = false;
static uint8_t s_mask = UIOPT_ALL;

static void ensure_loaded(void) {
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_UIOPT, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* 首次启动：无记录 = 全部开启，保持默认 */
        ESP_LOGI(TAG, "no saved mask (%s), defaults = all on", esp_err_to_name(err));
        return;
    }
    uint8_t v = UIOPT_ALL;
    err = nvs_get_u8(h, NVS_KEY_MASK, &v);
    if (err == ESP_OK) {
        s_mask = v & UIOPT_ALL;
        ESP_LOGI(TAG, "mask loaded = 0x%02X", s_mask);
    } else {
        ESP_LOGW(TAG, "no key \"m\": %s, defaults = all on", esp_err_to_name(err));
    }
    nvs_close(h);
}

bool ui_options_on(uiopt_bit_t bit) {
    ensure_loaded();
    return (s_mask & (uint8_t)bit) != 0;
}

uint8_t ui_options_get(void) {
    ensure_loaded();
    return s_mask;
}

void ui_options_set(uint8_t mask) {
    ensure_loaded();
    s_mask = mask & UIOPT_ALL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_UIOPT, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, NVS_KEY_MASK, s_mask);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "mask = 0x%02X (%s)", s_mask,
             err == ESP_OK ? "saved" : esp_err_to_name(err));
}

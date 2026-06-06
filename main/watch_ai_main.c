#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/param.h>
#include <time.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

LV_FONT_DECLARE(watch_ai_cn_14);

#define TAG "watch_ai"

#define APP_NS "watch_ai"
#define APP_CONFIG_MAGIC 0x57414931u
#define APP_CONFIG_VERSION 4u

#define TTS_DEFAULT_MODEL "gpt-4o-mini-tts"
#define TTS_DEFAULT_VOICE "alloy"
#define TTS_DEFAULT_FORMAT "wav"
#define TTS_AZURE_DEFAULT_VOICE "zh-CN-XiaoxiaoNeural"
#define TTS_AZURE_OUTPUT_FORMAT "riff-24khz-16bit-mono-pcm"
#define TTS_DEFAULT_SPEED 1.0f
#define TTS_DEFAULT_VOLUME 75
#define STT_DEFAULT_MODEL "whisper-1"
#define TTS_MAX_TEXT_BYTES 1024
#define TTS_MAX_AUDIO_BYTES (1024 * 1024)
#define TTS_AUDIO_GROW_STEP (16 * 1024)
#define TTS_REPLY_TRIM_BYTES 768
#define VOICE_SAMPLE_RATE 22050
#define VOICE_CHANNELS 1
#define VOICE_BITS_PER_SAMPLE 16
#define VOICE_MAX_SECONDS 8
#define VOICE_MAX_IDLE_MS 2000
#define VOICE_SILENCE_MS 900
#define VOICE_SPEECH_THRESHOLD 900
#define VOICE_READ_CHUNK_BYTES 4096
#define CRYPTO_REFRESH_INTERVAL_MS 60000
#define CRYPTO_OFFLINE_RETRY_MS 10000
#define CRYPTO_IDS "bitcoin,ethereum,solana"
#define BTC_CHART_POINT_COUNT 24
#define BTC_CHART_SOURCE_LIMIT 128
#define AUTO_SLEEP_DEFAULT_SECONDS 60
#define AUTO_SLEEP_MAX_SECONDS 86400

#define UI_SAFE_X 28
#define UI_SAFE_Y 24
#define UI_SAFE_W (BSP_LCD_H_RES - (UI_SAFE_X * 2))
#define UI_SAFE_TEXT_X (UI_SAFE_X + 8)
#define UI_SAFE_TEXT_W (UI_SAFE_W - 16)
#define UI_CARD_PAD 14
#define UI_CARD_CONTENT_W (UI_SAFE_W - (UI_CARD_PAD * 2))
#define UI_PAIR_BTN_W ((UI_CARD_CONTENT_W - 10) / 2)
#define UI_CHAT_BTN_W ((UI_CARD_CONTENT_W - 20) / 3)
#define UI_KEYBOARD_W UI_SAFE_W
#define UI_KEYBOARD_H 160

#define MAX_CHAT_MESSAGES 12
#define MAX_HISTORY_MESSAGES 8
#define MAX_MESSAGE_LEN 1024
#define MAX_REPLY_LEN 1536
#define MAX_ERROR_LEN 256
#define MAX_JSON_BODY 16384
#define MAX_HTTP_RESPONSE 8192
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RESTART_DELAY_MS 400
#define UI_AUTO_SLEEP_TIMEOUT_MS 60000

typedef struct {
    uint32_t magic;
    uint32_t version;
    char ap_ssid[32];
    char ap_password[64];
    bool ap_enabled;
    char wifi_ssid[32];
    char wifi_password[64];
    char api_url[192];
    char api_key[192];
    char tts_api_key[192];
    char model[64];
    char system_prompt[512];
    int timeout_ms;
    int max_tokens;
    float temperature;
    bool tts_enabled;
    char tts_url[192];
    char tts_model[64];
    char tts_voice[32];
    char tts_response_format[16];
    float tts_speed;
    int auto_sleep_timeout_s;
} watch_ai_config_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char ap_ssid[32];
    char ap_password[64];
    bool ap_enabled;
    char wifi_ssid[32];
    char wifi_password[64];
    char api_url[192];
    char api_key[192];
    char model[64];
    char system_prompt[512];
    int timeout_ms;
    int max_tokens;
    float temperature;
} watch_ai_config_v1_t;

typedef struct {
    char role[16];
    char *content;
} chat_message_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool truncated;
} binary_buffer_t;

typedef struct {
    const char *id;
    const char *symbol;
    const char *name;
    double price_usd;
    double change_24h;
} crypto_quote_t;

typedef struct {
    int32_t values[BTC_CHART_POINT_COUNT];
    size_t count;
    int32_t min_value;
    int32_t max_value;
    bool valid;
} btc_chart_data_t;

typedef struct {
    size_t data_offset;
    size_t data_len;
    esp_codec_dev_sample_info_t fs;
} wav_audio_info_t;

typedef struct {
    char message[MAX_MESSAGE_LEN];
    watch_ai_config_t cfg;
} chat_async_job_t;

typedef struct {
    char reply[MAX_REPLY_LEN];
    watch_ai_config_t cfg;
} tts_async_job_t;

typedef struct {
    watch_ai_config_t cfg;
} voice_async_job_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool truncated;
} http_buffer_t;

typedef struct {
    watch_ai_config_t config;
    bool config_loaded;

    bool sta_connected;
    bool sta_connecting;
    bool ap_running;
    bool ap_forced;
    bool wifi_restart_force_ap;
    bool wifi_restarting;
    bool wifi_restart_pending;
    bool sntp_started;
    bool time_synced;
    bool chat_busy;
    bool tts_busy;
    bool voice_busy;
    bool crypto_busy;
    bool screen_sleeping;
    int64_t last_activity_us;

    char sta_ip[16];
    char ap_ip[16];
    char last_error[MAX_ERROR_LEN];
    char last_reply[MAX_REPLY_LEN];
    char last_user[MAX_MESSAGE_LEN];
    char status_line[160];
    char qr_payload[192];
    char crypto_status[96];
    char crypto_summary[320];
    char crypto_updated[64];

    chat_message_t history[MAX_CHAT_MESSAGES];
    size_t history_count;

    SemaphoreHandle_t mutex;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    httpd_handle_t httpd;

    lv_display_t *display;
    lv_obj_t *screen;
    lv_obj_t *home_status_bar;
    lv_obj_t *home_wifi_label;
    lv_obj_t *home_time_label;
    lv_obj_t *home_battery_box;
    lv_obj_t *home_battery_icon;
    lv_obj_t *home_battery_label;
    lv_obj_t *home_title_label;
    lv_obj_t *home_subtitle_label;
    lv_obj_t *title_label;
    lv_obj_t *subtitle_label;
    lv_obj_t *status_label;
    lv_obj_t *crypto_card;
    lv_obj_t *crypto_title_label;
    lv_obj_t *crypto_status_label;
    lv_obj_t *crypto_summary_label;
    lv_obj_t *crypto_updated_label;
    lv_obj_t *crypto_chart;
    lv_chart_series_t *crypto_chart_series;
    int32_t crypto_chart_values[BTC_CHART_POINT_COUNT];
    size_t crypto_chart_point_count;
    int32_t crypto_chart_min_value;
    int32_t crypto_chart_max_value;
    uint32_t crypto_chart_generation;
    uint32_t crypto_chart_applied_generation;
    bool crypto_chart_dirty;
    lv_obj_t *voice_card;
    lv_obj_t *voice_title_label;
    lv_obj_t *voice_desc_label;
    lv_obj_t *settings_btn;
    lv_obj_t *settings_label;
    lv_obj_t *home_hint_label;
    lv_obj_t *settings_menu_screen;
    lv_obj_t *settings_menu_title_label;
    lv_obj_t *settings_menu_hint_label;
    lv_obj_t *settings_menu_status_label;
    lv_obj_t *settings_menu_wifi_btn;
    lv_obj_t *settings_menu_wifi_label;
    lv_obj_t *settings_menu_ap_btn;
    lv_obj_t *settings_menu_ap_label;
    lv_obj_t *settings_menu_restart_btn;
    lv_obj_t *settings_menu_restart_label;
    lv_obj_t *settings_menu_sleep_btn;
    lv_obj_t *settings_menu_sleep_label;
    lv_obj_t *settings_menu_auto_sleep_btn;
    lv_obj_t *settings_menu_auto_sleep_label;
    lv_obj_t *settings_menu_back_btn;
    lv_obj_t *settings_menu_back_label;
    lv_obj_t *settings_menu_keyboard;
    lv_obj_t *settings_screen;
    lv_obj_t *settings_title_label;
    lv_obj_t *settings_hint_label;
    lv_obj_t *settings_status_label;
    lv_obj_t *settings_wifi_card;
    lv_obj_t *settings_wifi_ssid_label;
    lv_obj_t *settings_wifi_ssid_ta;
    lv_obj_t *settings_wifi_password_label;
    lv_obj_t *settings_wifi_password_ta;
    lv_obj_t *settings_save_btn;
    lv_obj_t *settings_forget_btn;
    lv_obj_t *settings_back_btn;
    lv_obj_t *settings_keyboard;
    lv_obj_t *sleep_settings_screen;
    lv_obj_t *sleep_settings_title_label;
    lv_obj_t *sleep_settings_hint_label;
    lv_obj_t *sleep_settings_status_label;
    lv_obj_t *sleep_settings_timeout_label;
    lv_obj_t *sleep_settings_timeout_ta;
    lv_obj_t *sleep_settings_save_btn;
    lv_obj_t *sleep_settings_back_btn;
    lv_obj_t *sleep_settings_keyboard;
    lv_obj_t *chat_screen;
    lv_obj_t *chat_title_label;
    lv_obj_t *chat_status_label;
    lv_obj_t *chat_voice_label;
    lv_obj_t *chat_history_container;
    lv_obj_t *chat_history_label;
    lv_obj_t *chat_input_ta;
    lv_obj_t *chat_send_btn;
    lv_obj_t *chat_clear_btn;
    lv_obj_t *chat_back_btn;
    lv_obj_t *chat_keyboard;
    lv_timer_t *ui_timer;
    esp_codec_dev_handle_t speaker_dev;
} app_state_t;

typedef enum {
    UI_ACTION_TOGGLE_AP = 1,
    UI_ACTION_RESTART_WIFI = 2,
    UI_ACTION_OPEN_SETTINGS_MENU = 3,
    UI_ACTION_OPEN_WIFI_SETTINGS = 4,
    UI_ACTION_TOGGLE_SLEEP = 5,
    UI_ACTION_WIFI_SAVE = 6,
    UI_ACTION_WIFI_FORGET = 7,
    UI_ACTION_WIFI_BACK = 8,
    UI_ACTION_START_VOICE_CHAT = 9,
    UI_ACTION_OPEN_CHAT = 10,
    UI_ACTION_CHAT_SEND = 11,
    UI_ACTION_CHAT_CLEAR = 12,
    UI_ACTION_CHAT_BACK = 13,
    UI_ACTION_OPEN_SLEEP_SETTINGS = 14,
    UI_ACTION_SLEEP_SAVE = 15,
    UI_ACTION_SLEEP_BACK = 16,
} ui_action_t;

static app_state_t g_app = {0};

static void schedule_wifi_restart(bool force_ap);
static void wifi_restart_task(void *arg);
static void wifi_watchdog_task(void *arg);
static void app_start_audio(void);
static void ui_mark_activity(void);
static void ui_set_screen_sleep(bool sleeping, const char *reason);
static void ui_refresh_sleep_button(void);
static esp_err_t ui_enter_light_sleep(void);
static void ui_show_main_screen(void);
static void ui_show_settings_menu(void);
static void ui_show_wifi_settings(void);
static void ui_show_sleep_settings(void);
static void ui_show_chat_screen(void);
static void ui_start_voice_chat(void);
static void voice_chat_task(void *arg);
static void crypto_refresh_task(void *arg);
static void ui_refresh_timer(lv_timer_t *timer);
static void ui_refresh_home_status_bar(void);
static void format_local_time_string(char *buf, size_t buf_len);
static void ui_settings_refresh_form(void);
static void ui_settings_menu_refresh_form(void);
static void ui_settings_apply_wifi(bool forget_wifi);
static void ui_create_settings_menu_screen(void);
static void ui_create_wifi_settings_screen(void);
static void ui_create_sleep_settings_screen(void);
static void ui_create_chat_screen(void);
static void ui_apply_cn_font_tree(lv_obj_t *obj);
static void ui_chat_refresh_form(void);
static void ui_chat_set_status(const char *text);
static void ui_chat_submit_text(const char *text);
static void ui_chat_clear_history(void);
static void ui_refresh_chat_history_text(const char *history_text);
static void ui_sleep_settings_refresh_form(void);
static void ui_sleep_apply(void);
static void schedule_tts_reply(const watch_ai_config_t *cfg, const char *text);
static esp_err_t binary_buffer_append(binary_buffer_t *buffer, const uint8_t *data, size_t len);
static esp_err_t binary_buffer_append_format(binary_buffer_t *buffer, const char *fmt, ...);
static void binary_buffer_free(binary_buffer_t *buffer);
static void build_wifi_qr_payload(char *out, size_t out_len, const watch_ai_config_t *cfg);
static esp_err_t http_client_event_handler(esp_http_client_event_t *evt);
static esp_err_t perform_stt_request(const watch_ai_config_t *cfg, const uint8_t *wav_data, size_t wav_len, char *text_out, size_t text_out_len, char *error_out, size_t error_out_len);
static void tts_play_task(void *arg);
static esp_err_t run_chat_turn(const watch_ai_config_t *cfg, const char *user_prompt, char *reply_out, size_t reply_out_len, char *error_out, size_t error_out_len);
static void chat_turn_task(void *arg);
static void ui_sleep_textarea_event_cb(lv_event_t *e);
static void ui_sleep_keyboard_event_cb(lv_event_t *e);

static void state_lock(void)
{
    if (g_app.mutex) {
        xSemaphoreTake(g_app.mutex, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (g_app.mutex) {
        xSemaphoreGive(g_app.mutex);
    }
}

static void ui_apply_cn_font_tree(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }

    lv_obj_set_style_text_font(obj, &watch_ai_cn_14, LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, &watch_ai_cn_14, LV_PART_ITEMS);
    lv_obj_set_style_text_font(obj, &watch_ai_cn_14, LV_PART_TEXTAREA_PLACEHOLDER);

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        ui_apply_cn_font_tree(lv_obj_get_child(obj, i));
    }
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    snprintf(dst, dst_size, "%s", src);
}

static bool string_has_value(const char *s)
{
    return s && s[0] != '\0';
}

static bool string_equal(const char *a, const char *b)
{
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    return strcmp(a, b) == 0;
}

static bool system_time_valid(void)
{
    time_t now = 0;
    struct tm tm_now = {0};
    time(&now);
    localtime_r(&now, &tm_now);
    return tm_now.tm_year >= (2024 - 1900);
}

static void set_last_error_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_app.last_error, sizeof(g_app.last_error), fmt, args);
    va_end(args);
    ESP_LOGW(TAG, "%s", g_app.last_error);
}

static void clear_last_error_locked(void)
{
    g_app.last_error[0] = '\0';
}

static void set_last_reply_locked(const char *text)
{
    copy_string(g_app.last_reply, sizeof(g_app.last_reply), text);
}

static void set_last_user_locked(const char *text)
{
    copy_string(g_app.last_user, sizeof(g_app.last_user), text);
}

static void set_status_line_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_app.status_line, sizeof(g_app.status_line), fmt, args);
    va_end(args);
}

static void default_device_identity(watch_ai_config_t *cfg)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "WatchAI-%02X%02X", mac[4], mac[5]);
    snprintf(cfg->ap_password, sizeof(cfg->ap_password), "watchai%02X%02X", mac[4], mac[5]);
}

static void config_terminate_strings(watch_ai_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->ap_ssid[sizeof(cfg->ap_ssid) - 1] = '\0';
    cfg->ap_password[sizeof(cfg->ap_password) - 1] = '\0';
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->api_url[sizeof(cfg->api_url) - 1] = '\0';
    cfg->api_key[sizeof(cfg->api_key) - 1] = '\0';
    cfg->tts_api_key[sizeof(cfg->tts_api_key) - 1] = '\0';
    cfg->model[sizeof(cfg->model) - 1] = '\0';
    cfg->system_prompt[sizeof(cfg->system_prompt) - 1] = '\0';
    cfg->tts_url[sizeof(cfg->tts_url) - 1] = '\0';
    cfg->tts_model[sizeof(cfg->tts_model) - 1] = '\0';
    cfg->tts_voice[sizeof(cfg->tts_voice) - 1] = '\0';
    cfg->tts_response_format[sizeof(cfg->tts_response_format) - 1] = '\0';
}

static bool string_contains_case_insensitive(const char *haystack, const char *needle)
{
    if (!string_has_value(haystack) || !string_has_value(needle)) {
        return false;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return false;
    }

    for (const char *p = haystack; *p != '\0'; ++p) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool tts_uses_azure_speech_endpoint(const char *url)
{
    return string_contains_case_insensitive(url, "speech.microsoft.com") ||
           string_contains_case_insensitive(url, "cognitive.microsoft.com") ||
           string_contains_case_insensitive(url, "cognitiveservices.azure.com");
}

static bool tts_voice_is_openai_default(const char *voice)
{
    return string_equal(voice, TTS_DEFAULT_VOICE) ||
           string_equal(voice, "nova") ||
           string_equal(voice, "echo") ||
           string_equal(voice, "fable") ||
           string_equal(voice, "shimmer") ||
           string_equal(voice, "onyx");
}

static void azure_voice_locale_from_name(const char *voice, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    copy_string(out, out_len, "zh-CN");
    if (!string_has_value(voice)) {
        return;
    }

    const char *first_dash = strchr(voice, '-');
    if (!first_dash) {
        return;
    }

    const char *second_dash = strchr(first_dash + 1, '-');
    if (!second_dash || second_dash <= voice) {
        return;
    }

    size_t len = (size_t)(second_dash - voice);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, voice, len);
    out[len] = '\0';
}

static const char *effective_tts_api_key(const watch_ai_config_t *cfg)
{
    if (!cfg) {
        return "";
    }
    if (string_has_value(cfg->tts_api_key)) {
        return cfg->tts_api_key;
    }
    if (string_has_value(cfg->api_key)) {
        return cfg->api_key;
    }
    return "";
}

static esp_err_t binary_buffer_append_cstr(binary_buffer_t *buffer, const char *text)
{
    if (!buffer || !text) {
        return ESP_OK;
    }
    return binary_buffer_append(buffer, (const uint8_t *)text, strlen(text));
}

static esp_err_t binary_buffer_append_escaped_xml(binary_buffer_t *buffer, const char *text)
{
    if (!buffer || !text) {
        return ESP_OK;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        esp_err_t err = ESP_OK;
        switch ((unsigned char)text[i]) {
            case '&':
                err = binary_buffer_append_cstr(buffer, "&amp;");
                break;
            case '<':
                err = binary_buffer_append_cstr(buffer, "&lt;");
                break;
            case '>':
                err = binary_buffer_append_cstr(buffer, "&gt;");
                break;
            case '"':
                err = binary_buffer_append_cstr(buffer, "&quot;");
                break;
            case '\'':
                err = binary_buffer_append_cstr(buffer, "&apos;");
                break;
            default:
                err = binary_buffer_append(buffer, (const uint8_t *)&text[i], 1);
                break;
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t build_azure_tts_payload(const char *text, const char *voice, float speed, binary_buffer_t *payload)
{
    if (!payload || !string_has_value(text)) {
        return ESP_ERR_INVALID_ARG;
    }

    char locale[16] = {0};
    azure_voice_locale_from_name(voice, locale, sizeof(locale));

    esp_err_t err = binary_buffer_append_format(
        payload,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"%s\">"
        "<voice name=\"%s\" xml:lang=\"%s\"><prosody rate=\"%.2f\">",
        locale,
        string_has_value(voice) ? voice : TTS_AZURE_DEFAULT_VOICE,
        locale,
        (double)speed);
    if (err != ESP_OK) {
        return err;
    }

    err = binary_buffer_append_escaped_xml(payload, text);
    if (err != ESP_OK) {
        return err;
    }

    return binary_buffer_append_cstr(payload, "</prosody></voice></speak>");
}

static void derive_openai_endpoint_url_from_api_url(const char *api_url, const char *endpoint_path, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!string_has_value(api_url) || !string_has_value(endpoint_path)) {
        return;
    }

    const char *v1 = strstr(api_url, "/v1/");
    if (v1) {
        size_t prefix_len = (size_t)(v1 - api_url);
        snprintf(out, out_len, "%.*s/v1/%s", (int)prefix_len, api_url, endpoint_path);
        return;
    }

    size_t len = strlen(api_url);
    if (len > 0 && api_url[len - 1] == '/') {
        snprintf(out, out_len, "%sv1/%s", api_url, endpoint_path);
    } else {
        snprintf(out, out_len, "%s/v1/%s", api_url, endpoint_path);
    }
}

static void derive_tts_url_from_api_url(const char *api_url, char *out, size_t out_len)
{
    derive_openai_endpoint_url_from_api_url(api_url, "audio/speech", out, out_len);
}

static void derive_stt_url_from_api_url(const char *api_url, char *out, size_t out_len)
{
    derive_openai_endpoint_url_from_api_url(api_url, "audio/transcriptions", out, out_len);
}

static bool tts_response_format_supported(const char *fmt)
{
    return string_equal(fmt, "wav");
}

static size_t utf8_safe_copy(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return 0;
    }
    dst[0] = '\0';
    if (!src) {
        return 0;
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != '\0';) {
        unsigned char c = (unsigned char)src[si];
        size_t char_len = 1;
        if ((c & 0x80u) == 0x00u) {
            char_len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            char_len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            char_len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            char_len = 4;
        }
        if (di + char_len >= dst_len) {
            break;
        }
        memcpy(dst + di, src + si, char_len);
        di += char_len;
        si += char_len;
    }
    dst[di] = '\0';
    return di;
}

static void normalize_text_for_tts(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    size_t di = 0;
    bool in_space = true;
    for (size_t si = 0; src[si] != '\0';) {
        unsigned char c = (unsigned char)src[si];
        if (c == '`') {
            ++si;
            continue;
        }
        if (isspace(c)) {
            if (!in_space && di + 1 < dst_len) {
                dst[di++] = ' ';
                in_space = true;
            }
            ++si;
            continue;
        }

        size_t char_len = 1;
        if ((c & 0x80u) == 0x00u) {
            char_len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            char_len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            char_len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            char_len = 4;
        }
        if (di + char_len >= dst_len) {
            break;
        }
        memcpy(dst + di, src + si, char_len);
        di += char_len;
        si += char_len;
        in_space = false;
    }

    while (di > 0 && dst[di - 1] == ' ') {
        --di;
    }
    dst[di] = '\0';
}

static void binary_buffer_free(binary_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
    buffer->truncated = false;
}

static esp_err_t binary_buffer_reserve(binary_buffer_t *buffer, size_t needed)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (needed <= buffer->cap) {
        return ESP_OK;
    }
    size_t new_cap = buffer->cap ? buffer->cap : TTS_AUDIO_GROW_STEP;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap > TTS_MAX_AUDIO_BYTES) {
            new_cap = TTS_MAX_AUDIO_BYTES;
            break;
        }
    }
    if (new_cap < needed) {
        buffer->truncated = true;
        return ESP_ERR_NO_MEM;
    }

    uint8_t *new_data = realloc(buffer->data, new_cap);
    if (!new_data) {
        buffer->truncated = true;
        return ESP_ERR_NO_MEM;
    }
    buffer->data = new_data;
    buffer->cap = new_cap;
    return ESP_OK;
}

static esp_err_t binary_buffer_append(binary_buffer_t *buffer, const uint8_t *data, size_t len)
{
    if (!buffer || !data || len == 0) {
        return ESP_OK;
    }

    if (buffer->len + len > TTS_MAX_AUDIO_BYTES) {
        len = (buffer->len < TTS_MAX_AUDIO_BYTES) ? (TTS_MAX_AUDIO_BYTES - buffer->len) : 0;
        buffer->truncated = true;
    }
    if (len == 0) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = binary_buffer_reserve(buffer, buffer->len + len);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return ESP_OK;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static esp_err_t binary_buffer_append_format(binary_buffer_t *buffer, const char *fmt, ...)
{
    if (!buffer || !fmt) {
        return ESP_ERR_INVALID_ARG;
    }

    char temp[512];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);
    if (written < 0) {
        return ESP_FAIL;
    }
    if ((size_t)written >= sizeof(temp)) {
        return ESP_ERR_NO_MEM;
    }
    return binary_buffer_append(buffer, (const uint8_t *)temp, (size_t)written);
}

static esp_err_t build_wav_header(uint8_t *dst, size_t pcm_bytes, const esp_codec_dev_sample_info_t *fs)
{
    if (!dst || !fs || fs->bits_per_sample == 0 || fs->channel == 0 || fs->sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t block_align = (uint16_t)(fs->channel * (fs->bits_per_sample / 8));
    const uint32_t byte_rate = fs->sample_rate * block_align;
    memcpy(dst + 0, "RIFF", 4);
    write_le32(dst + 4, (uint32_t)(36 + pcm_bytes));
    memcpy(dst + 8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    write_le32(dst + 16, 16);
    write_le16(dst + 20, 1);
    write_le16(dst + 22, fs->channel);
    write_le32(dst + 24, fs->sample_rate);
    write_le32(dst + 28, byte_rate);
    write_le16(dst + 32, block_align);
    write_le16(dst + 34, fs->bits_per_sample);
    memcpy(dst + 36, "data", 4);
    write_le32(dst + 40, (uint32_t)pcm_bytes);
    return ESP_OK;
}

static int64_t pcm_chunk_energy_16bit(const int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0) {
        return 0;
    }

    int64_t total = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t sample = samples[i];
        total += (sample < 0) ? -(int64_t)sample : (int64_t)sample;
    }
    return total / (int64_t)sample_count;
}

static bool parse_wav_audio(const uint8_t *data, size_t len, wav_audio_info_t *out)
{
    if (!data || !out || len < 44) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t pos = 12;
    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;

    while (pos + 8 <= len) {
        const uint8_t *chunk = data + pos;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t payload = pos + 8;
        if (payload > len) {
            return false;
        }
        if (payload + chunk_size > len) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return false;
            }
            audio_format = read_le16(data + payload + 0);
            out->fs.channel = (uint8_t)read_le16(data + payload + 2);
            out->fs.sample_rate = read_le32(data + payload + 4);
            out->fs.bits_per_sample = (uint8_t)read_le16(data + payload + 14);
            out->fs.channel_mask = 0;
            out->fs.mclk_multiple = 256;
            found_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            out->data_offset = payload;
            out->data_len = chunk_size;
            found_data = true;
        }

        pos = payload + chunk_size;
        if (chunk_size & 1u) {
            ++pos;
        }
    }

    if (!found_fmt || !found_data) {
        return false;
    }
    if (audio_format != 1 || out->fs.bits_per_sample != 16 || out->fs.channel == 0 || out->fs.sample_rate == 0) {
        return false;
    }
    if (out->data_offset + out->data_len > len) {
        return false;
    }
    return true;
}

static void build_wav_sample_info(const wav_audio_info_t *wav, esp_codec_dev_sample_info_t *fs)
{
    if (!wav || !fs) {
        return;
    }
    *fs = wav->fs;
    if (fs->mclk_multiple <= 0) {
        fs->mclk_multiple = 256;
    }
    if (fs->channel_mask == 0) {
        fs->channel_mask = (fs->channel > 1) ? ((1u << fs->channel) - 1u) : 1u;
    }
}

static void config_set_defaults(watch_ai_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = APP_CONFIG_MAGIC;
    cfg->version = APP_CONFIG_VERSION;
    default_device_identity(cfg);
    cfg->ap_enabled = true;
    copy_string(cfg->api_url, sizeof(cfg->api_url), "https://subrouter.ai/v1/chat/completions");
    copy_string(cfg->model, sizeof(cfg->model), "gpt-5.5");
    copy_string(cfg->system_prompt, sizeof(cfg->system_prompt), "你是一个戴在手腕上的AI助手。回答尽量简短、清晰，优先使用中文。");
    cfg->timeout_ms = 60000;
    cfg->max_tokens = 512;
    cfg->temperature = 0.7f;
    cfg->tts_enabled = true;
    copy_string(cfg->tts_model, sizeof(cfg->tts_model), TTS_DEFAULT_MODEL);
    copy_string(cfg->tts_voice, sizeof(cfg->tts_voice), TTS_DEFAULT_VOICE);
    copy_string(cfg->tts_response_format, sizeof(cfg->tts_response_format), TTS_DEFAULT_FORMAT);
    cfg->tts_speed = TTS_DEFAULT_SPEED;
    cfg->auto_sleep_timeout_s = AUTO_SLEEP_DEFAULT_SECONDS;
    derive_tts_url_from_api_url(cfg->api_url, cfg->tts_url, sizeof(cfg->tts_url));
}

static esp_err_t config_load(watch_ai_config_t *cfg, bool *needs_save)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (needs_save) {
        *needs_save = false;
    }

    config_set_defaults(cfg);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(*cfg);
    err = nvs_get_blob(handle, "config", cfg, &size);
    nvs_close(handle);

    if (err != ESP_OK) {
        config_set_defaults(cfg);
        return err;
    }

    if (size == sizeof(watch_ai_config_v1_t) && cfg->magic == APP_CONFIG_MAGIC && cfg->version == 1u) {
        cfg->version = APP_CONFIG_VERSION;
        if (needs_save) {
            *needs_save = true;
        }
    } else if (size == offsetof(watch_ai_config_t, auto_sleep_timeout_s) &&
               cfg->magic == APP_CONFIG_MAGIC &&
               cfg->version == 2u) {
        cfg->version = APP_CONFIG_VERSION;
        if (needs_save) {
            *needs_save = true;
        }
    } else if (size == offsetof(watch_ai_config_t, tts_api_key) &&
               cfg->magic == APP_CONFIG_MAGIC &&
               cfg->version == 3u) {
        cfg->version = APP_CONFIG_VERSION;
        if (needs_save) {
            *needs_save = true;
        }
    } else if (size != sizeof(*cfg) || cfg->magic != APP_CONFIG_MAGIC || cfg->version != APP_CONFIG_VERSION) {
        config_set_defaults(cfg);
        return ESP_ERR_INVALID_VERSION;
    }

    config_terminate_strings(cfg);
    return ESP_OK;
}

static esp_err_t config_save(const watch_ai_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, "config", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t config_validate(const watch_ai_config_t *cfg, char *errbuf, size_t errbuf_len)
{
    if (!cfg) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "配置为空");
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (!string_has_value(cfg->api_url)) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "API URL 不能为空");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!string_has_value(cfg->model)) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "模型名不能为空");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!string_has_value(cfg->ap_ssid)) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "热点名称不能为空");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(cfg->ap_password) > 0 && strlen(cfg->ap_password) < 8) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "热点密码至少 8 位");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (string_has_value(cfg->wifi_ssid) && strlen(cfg->wifi_password) > 0 && strlen(cfg->wifi_password) < 8) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "Wi-Fi 密码至少 8 位，或者留空表示开放网络");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->timeout_ms < 5000 || cfg->timeout_ms > 180000) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "超时时间需要在 5000-180000ms 之间");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->max_tokens < 1 || cfg->max_tokens > 8192) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "max_tokens 需要在 1-8192 之间");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->temperature < 0.0f || cfg->temperature > 2.0f) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "temperature 需要在 0.0-2.0 之间");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->auto_sleep_timeout_s < 0 || cfg->auto_sleep_timeout_s > AUTO_SLEEP_MAX_SECONDS) {
        if (errbuf && errbuf_len) {
            snprintf(errbuf, errbuf_len, "自动待机需要在 0-%d 秒之间", AUTO_SLEEP_MAX_SECONDS);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->tts_enabled) {
        bool azure_tts = tts_uses_azure_speech_endpoint(cfg->tts_url);

        if (string_has_value(cfg->tts_url) &&
            strncasecmp(cfg->tts_url, "http://", 7) != 0 &&
            strncasecmp(cfg->tts_url, "https://", 8) != 0) {
            if (errbuf && errbuf_len) {
                snprintf(errbuf, errbuf_len, "TTS URL 需要以 http:// 或 https:// 开头");
            }
            return ESP_ERR_INVALID_ARG;
        }
        if (azure_tts && !string_has_value(cfg->tts_api_key)) {
            if (errbuf && errbuf_len) {
                snprintf(errbuf, errbuf_len, "微软 TTS 需要填写 TTS Key");
            }
            return ESP_ERR_INVALID_ARG;
        }
        if (!azure_tts && string_has_value(cfg->tts_response_format) &&
            !tts_response_format_supported(cfg->tts_response_format)) {
            if (errbuf && errbuf_len) {
                snprintf(errbuf, errbuf_len, "TTS 格式目前只支持 wav");
            }
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->tts_speed < 0.5f || cfg->tts_speed > 2.0f) {
            if (errbuf && errbuf_len) {
                snprintf(errbuf, errbuf_len, "朗读速度需要在 0.5-2.0 之间");
            }
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static void history_free_locked(void)
{
    for (size_t i = 0; i < g_app.history_count; ++i) {
        free(g_app.history[i].content);
        g_app.history[i].content = NULL;
        g_app.history[i].role[0] = '\0';
    }
    g_app.history_count = 0;
}

static esp_err_t history_append_locked(const char *role, const char *content)
{
    if (!role || !content) {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_app.history_count >= MAX_CHAT_MESSAGES) {
        free(g_app.history[0].content);
        memmove(&g_app.history[0], &g_app.history[1], sizeof(g_app.history[0]) * (MAX_CHAT_MESSAGES - 1));
        g_app.history_count = MAX_CHAT_MESSAGES - 1;
    }

    chat_message_t *msg = &g_app.history[g_app.history_count++];
    memset(msg, 0, sizeof(*msg));
    snprintf(msg->role, sizeof(msg->role), "%s", role);
    size_t len = strlen(content);
    if (len > MAX_MESSAGE_LEN - 1) {
        len = MAX_MESSAGE_LEN - 1;
    }
    msg->content = calloc(1, len + 1);
    if (!msg->content) {
        g_app.history_count--;
        return ESP_ERR_NO_MEM;
    }
    memcpy(msg->content, content, len);
    msg->content[len] = '\0';
    return ESP_OK;
}

static void history_clear(void)
{
    state_lock();
    history_free_locked();
    clear_last_error_locked();
    g_app.last_reply[0] = '\0';
    g_app.last_user[0] = '\0';
    state_unlock();
}

static void generate_ap_identity_from_cfg(watch_ai_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "WatchAI-%02X%02X", mac[4], mac[5]);
    snprintf(cfg->ap_password, sizeof(cfg->ap_password), "watchai%02X%02X", mac[4], mac[5]);
}

static void config_prepare_runtime_defaults(watch_ai_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->magic = APP_CONFIG_MAGIC;
    cfg->version = APP_CONFIG_VERSION;
    if (!string_has_value(cfg->ap_ssid) || !string_has_value(cfg->ap_password)) {
        generate_ap_identity_from_cfg(cfg);
    }
    if (!string_has_value(cfg->api_url)) {
        copy_string(cfg->api_url, sizeof(cfg->api_url), "https://subrouter.ai/v1/chat/completions");
    }
    if (!string_has_value(cfg->model)) {
        copy_string(cfg->model, sizeof(cfg->model), "gpt-5.5");
    }
    if (!string_has_value(cfg->system_prompt)) {
        copy_string(cfg->system_prompt, sizeof(cfg->system_prompt), "你是一个戴在手腕上的AI助手。回答尽量简短、清晰，优先使用中文。");
    }
    if (cfg->timeout_ms <= 0) {
        cfg->timeout_ms = 60000;
    }
    if (cfg->max_tokens <= 0) {
        cfg->max_tokens = 512;
    }
    if (cfg->temperature <= 0.0f) {
        cfg->temperature = 0.7f;
    }
    if (!string_has_value(cfg->tts_model)) {
        copy_string(cfg->tts_model, sizeof(cfg->tts_model), TTS_DEFAULT_MODEL);
    }
    if (!string_has_value(cfg->tts_voice) ||
        (tts_uses_azure_speech_endpoint(cfg->tts_url) && tts_voice_is_openai_default(cfg->tts_voice))) {
        if (tts_uses_azure_speech_endpoint(cfg->tts_url)) {
            copy_string(cfg->tts_voice, sizeof(cfg->tts_voice), TTS_AZURE_DEFAULT_VOICE);
        } else {
            copy_string(cfg->tts_voice, sizeof(cfg->tts_voice), TTS_DEFAULT_VOICE);
        }
    }
    if (!string_has_value(cfg->tts_response_format)) {
        copy_string(cfg->tts_response_format, sizeof(cfg->tts_response_format), TTS_DEFAULT_FORMAT);
    }
    if (cfg->tts_speed <= 0.0f) {
        cfg->tts_speed = TTS_DEFAULT_SPEED;
    }
    if (cfg->auto_sleep_timeout_s < 0 || cfg->auto_sleep_timeout_s > AUTO_SLEEP_MAX_SECONDS) {
        cfg->auto_sleep_timeout_s = AUTO_SLEEP_DEFAULT_SECONDS;
    }
    if (!string_has_value(cfg->tts_url)) {
        derive_tts_url_from_api_url(cfg->api_url, cfg->tts_url, sizeof(cfg->tts_url));
    }
}

static esp_err_t app_commit_config(const watch_ai_config_t *new_cfg, bool restart_wifi)
{
    char validation_error[MAX_ERROR_LEN] = {0};
    esp_err_t err = config_validate(new_cfg, validation_error, sizeof(validation_error));
    if (err != ESP_OK) {
        state_lock();
        set_last_error_locked("%s", validation_error[0] ? validation_error : "配置校验失败");
        state_unlock();
        return err;
    }

    err = config_save(new_cfg);
    if (err != ESP_OK) {
        state_lock();
        set_last_error_locked("保存配置失败: %s", esp_err_to_name(err));
        state_unlock();
        return err;
    }

    state_lock();
    g_app.config = *new_cfg;
    build_wifi_qr_payload(g_app.qr_payload, sizeof(g_app.qr_payload), &g_app.config);
    clear_last_error_locked();
    state_unlock();

    if (restart_wifi) {
        schedule_wifi_restart(false);
    }

    return ESP_OK;
}

static void add_json_string_if_present(cJSON *obj, const char *name, const char *value)
{
    cJSON_AddStringToObject(obj, name, value ? value : "");
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *obj, int status_code, const char *status_text)
{
    char *json = cJSON_PrintUnformatted(obj);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON 序列化失败\"}");
        return ESP_ERR_NO_MEM;
    }

    if (status_code != 200 && status_text) {
        httpd_resp_set_status(req, status_text);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t read_request_body(httpd_req_t *req, char **body_out, size_t *body_len_out)
{
    if (!req || !body_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *body_out = NULL;
    if (body_len_out) {
        *body_len_out = 0;
    }

    if (req->content_len <= 0 || req->content_len > MAX_JSON_BODY) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    *body_out = buf;
    if (body_len_out) {
        *body_len_out = received;
    }
    return ESP_OK;
}

static void json_config_snapshot(cJSON *root)
{
    state_lock();
    const watch_ai_config_t *cfg = &g_app.config;

    cJSON_AddBoolToObject(root, "ap_enabled", cfg->ap_enabled);
    add_json_string_if_present(root, "ap_ssid", cfg->ap_ssid);
    add_json_string_if_present(root, "wifi_ssid", cfg->wifi_ssid);
    add_json_string_if_present(root, "api_url", cfg->api_url);
    add_json_string_if_present(root, "model", cfg->model);
    add_json_string_if_present(root, "system_prompt", cfg->system_prompt);
    cJSON_AddNumberToObject(root, "timeout_ms", cfg->timeout_ms);
    cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(root, "tts_enabled", cfg->tts_enabled);
    add_json_string_if_present(root, "tts_url", cfg->tts_url);
    add_json_string_if_present(root, "tts_model", cfg->tts_model);
    add_json_string_if_present(root, "tts_voice", cfg->tts_voice);
    add_json_string_if_present(root, "tts_response_format", cfg->tts_response_format);
    cJSON_AddNumberToObject(root, "tts_speed", cfg->tts_speed);
    cJSON_AddNumberToObject(root, "auto_sleep_timeout_s", cfg->auto_sleep_timeout_s);
    cJSON_AddBoolToObject(root, "has_api_key", string_has_value(cfg->api_key));
    cJSON_AddBoolToObject(root, "has_tts_api_key", string_has_value(cfg->tts_api_key));
    cJSON_AddBoolToObject(root, "has_wifi_password", string_has_value(cfg->wifi_password));
    cJSON_AddBoolToObject(root, "has_ap_password", string_has_value(cfg->ap_password));
    cJSON_AddBoolToObject(root, "config_loaded", g_app.config_loaded);
    cJSON_AddBoolToObject(root, "ap_running", g_app.ap_running);
    cJSON_AddBoolToObject(root, "ap_forced", g_app.ap_forced);
    cJSON_AddBoolToObject(root, "sta_connected", g_app.sta_connected);
    cJSON_AddBoolToObject(root, "sta_connecting", g_app.sta_connecting);
    cJSON_AddBoolToObject(root, "chat_busy", g_app.chat_busy);
    cJSON_AddBoolToObject(root, "tts_busy", g_app.tts_busy);
    cJSON_AddBoolToObject(root, "voice_busy", g_app.voice_busy);
    cJSON_AddBoolToObject(root, "crypto_busy", g_app.crypto_busy);
    add_json_string_if_present(root, "sta_ip", g_app.sta_ip);
    add_json_string_if_present(root, "ap_ip", g_app.ap_ip);
    add_json_string_if_present(root, "status_line", g_app.status_line);
    add_json_string_if_present(root, "last_error", g_app.last_error);
    add_json_string_if_present(root, "last_reply", g_app.last_reply);
    add_json_string_if_present(root, "last_user", g_app.last_user);
    add_json_string_if_present(root, "crypto_status", g_app.crypto_status);
    add_json_string_if_present(root, "crypto_summary", g_app.crypto_summary);
    add_json_string_if_present(root, "crypto_updated", g_app.crypto_updated);
    state_unlock();
}

static void json_status_snapshot(cJSON *root)
{
    state_lock();
    const watch_ai_config_t *cfg = &g_app.config;
    const char *mode = "offline";
    if (g_app.ap_running && g_app.sta_connected) {
        mode = "apsta";
    } else if (g_app.ap_running) {
        mode = "ap";
    } else if (g_app.sta_connected || g_app.sta_connecting) {
        mode = "sta";
    }
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddBoolToObject(root, "ap_enabled", cfg->ap_enabled);
    cJSON_AddBoolToObject(root, "ap_running", g_app.ap_running);
    cJSON_AddBoolToObject(root, "ap_forced", g_app.ap_forced);
    cJSON_AddBoolToObject(root, "sta_connected", g_app.sta_connected);
    cJSON_AddBoolToObject(root, "sta_connecting", g_app.sta_connecting);
    cJSON_AddBoolToObject(root, "chat_busy", g_app.chat_busy);
    cJSON_AddBoolToObject(root, "tts_busy", g_app.tts_busy);
    cJSON_AddBoolToObject(root, "voice_busy", g_app.voice_busy);
    cJSON_AddBoolToObject(root, "crypto_busy", g_app.crypto_busy);
    cJSON_AddBoolToObject(root, "time_synced", g_app.time_synced);
    cJSON_AddBoolToObject(root, "wifi_restarting", g_app.wifi_restarting);
    cJSON_AddBoolToObject(root, "config_loaded", g_app.config_loaded);
    add_json_string_if_present(root, "ap_ssid", cfg->ap_ssid);
    add_json_string_if_present(root, "wifi_ssid", cfg->wifi_ssid);
    add_json_string_if_present(root, "api_url", cfg->api_url);
    add_json_string_if_present(root, "model", cfg->model);
    add_json_string_if_present(root, "system_prompt", cfg->system_prompt);
    cJSON_AddBoolToObject(root, "tts_enabled", cfg->tts_enabled);
    add_json_string_if_present(root, "tts_url", cfg->tts_url);
    add_json_string_if_present(root, "tts_model", cfg->tts_model);
    add_json_string_if_present(root, "tts_voice", cfg->tts_voice);
    add_json_string_if_present(root, "tts_response_format", cfg->tts_response_format);
    cJSON_AddNumberToObject(root, "tts_speed", cfg->tts_speed);
    cJSON_AddNumberToObject(root, "auto_sleep_timeout_s", cfg->auto_sleep_timeout_s);
    cJSON_AddBoolToObject(root, "has_tts_api_key", string_has_value(cfg->tts_api_key));
    add_json_string_if_present(root, "sta_ip", g_app.sta_ip);
    add_json_string_if_present(root, "ap_ip", g_app.ap_ip);
    add_json_string_if_present(root, "status_line", g_app.status_line);
    add_json_string_if_present(root, "last_error", g_app.last_error);
    add_json_string_if_present(root, "last_reply", g_app.last_reply);
    add_json_string_if_present(root, "last_user", g_app.last_user);
    add_json_string_if_present(root, "crypto_status", g_app.crypto_status);
    add_json_string_if_present(root, "crypto_summary", g_app.crypto_summary);
    add_json_string_if_present(root, "crypto_updated", g_app.crypto_updated);
    cJSON_AddNumberToObject(root, "timeout_ms", cfg->timeout_ms);
    cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    state_unlock();
}

static void json_history_snapshot(cJSON *root)
{
    cJSON *items = cJSON_AddArrayToObject(root, "messages");
    state_lock();
    for (size_t i = 0; i < g_app.history_count; ++i) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", g_app.history[i].role);
        cJSON_AddStringToObject(msg, "content", g_app.history[i].content ? g_app.history[i].content : "");
        cJSON_AddItemToArray(items, msg);
    }
    state_unlock();
}

static bool parse_bool_field(cJSON *root, const char *name, bool *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) {
        return false;
    }
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    return false;
}

static bool parse_int_field(cJSON *root, const char *name, int *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valueint;
        return true;
    }
    return false;
}

static bool parse_float_field(cJSON *root, const char *name, float *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *out = (float)item->valuedouble;
        return true;
    }
    return false;
}

static bool parse_string_field(cJSON *root, const char *name, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    copy_string(dst, dst_size, item->valuestring);
    return true;
}

static bool parse_secret_field(cJSON *root, const char *name, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    if (item->valuestring[0] == '\0') {
        return false;
    }
    copy_string(dst, dst_size, item->valuestring);
    return true;
}

static void wifi_update_ip_strings(void)
{
    char sta_ip[16] = "";
    char ap_ip[16] = "";

    if (g_app.sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(g_app.sta_netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, sta_ip, sizeof(sta_ip));
        }
    }

    if (g_app.ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(g_app.ap_netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, ap_ip, sizeof(ap_ip));
        }
    }

    state_lock();
    copy_string(g_app.sta_ip, sizeof(g_app.sta_ip), sta_ip);
    copy_string(g_app.ap_ip, sizeof(g_app.ap_ip), ap_ip);
    state_unlock();
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    state_lock();
    g_app.time_synced = true;
    clear_last_error_locked();
    set_status_line_locked("时间已同步");
    state_unlock();
    ESP_LOGI(TAG, "SNTP time synchronized");
}

static void start_sntp_if_needed(void)
{
    state_lock();
    if (g_app.sntp_started) {
        state_unlock();
        esp_netif_sntp_start();
        return;
    }
    g_app.sntp_started = true;
    state_unlock();

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.sync_cb = sntp_sync_cb;
    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        state_lock();
        set_last_error_locked("SNTP 初始化失败: %s", esp_err_to_name(err));
        state_unlock();
    }
}

static void wait_for_time_if_https(const watch_ai_config_t *cfg)
{
    if (!cfg || strncasecmp(cfg->api_url, "https://", 8) != 0) {
        return;
    }

    int waited = 0;
    while (!system_time_valid() && waited < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
}

static void escape_qr_text(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;
    if (!src || !dst || dst_len == 0) {
        return;
    }
    for (size_t i = 0; src[i] != '\0' && di + 2 < dst_len; ++i) {
        char c = src[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':') {
            dst[di++] = '\\';
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
}

static void build_wifi_qr_payload(char *out, size_t out_len, const watch_ai_config_t *cfg)
{
    if (!out || out_len == 0 || !cfg) {
        return;
    }

    char ssid_esc[64] = {0};
    char pass_esc[96] = {0};
    escape_qr_text(cfg->ap_ssid, ssid_esc, sizeof(ssid_esc));
    escape_qr_text(cfg->ap_password, pass_esc, sizeof(pass_esc));
    if (cfg->ap_password[0] == '\0') {
        snprintf(out, out_len, "WIFI:T:nopass;S:%s;H:false;;", ssid_esc);
    } else {
        snprintf(out, out_len, "WIFI:T:WPA;S:%s;P:%s;H:false;;", ssid_esc, pass_esc);
    }
}

static const crypto_quote_t k_crypto_assets[] = {
    { "bitcoin", "BTC", "Bitcoin", 0.0, 0.0 },
    { "ethereum", "ETH", "Ethereum", 0.0, 0.0 },
    { "solana", "SOL", "Solana", 0.0, 0.0 },
};

static void set_crypto_state_locked(const char *status, const char *summary, const char *updated)
{
    copy_string(g_app.crypto_status, sizeof(g_app.crypto_status), status);
    copy_string(g_app.crypto_summary, sizeof(g_app.crypto_summary), summary);
    copy_string(g_app.crypto_updated, sizeof(g_app.crypto_updated), updated);
}

static void format_crypto_price_line(char *dst, size_t dst_len, const crypto_quote_t *quote)
{
    if (!dst || dst_len == 0 || !quote) {
        return;
    }

    char change_sign = (quote->change_24h >= 0.0) ? '+' : '-';
    double change_abs = (quote->change_24h >= 0.0) ? quote->change_24h : -quote->change_24h;
    if (quote->price_usd > 1000.0) {
        snprintf(dst, dst_len, "%s  $%.2f  %c%.2f%%", quote->symbol, quote->price_usd, change_sign, change_abs);
    } else if (quote->price_usd > 10.0) {
        snprintf(dst, dst_len, "%s  $%.3f  %c%.2f%%", quote->symbol, quote->price_usd, change_sign, change_abs);
    } else {
        snprintf(dst, dst_len, "%s  $%.6f  %c%.2f%%", quote->symbol, quote->price_usd, change_sign, change_abs);
    }
}

static esp_err_t fetch_http_response(const char *url, http_buffer_t *response, char *error_out, size_t error_out_len)
{
    if (!url || !response || !response->buf) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t client_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_client_event_handler,
        .user_data = response,
        .user_agent = "WatchAI/1.0",
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .disable_auto_redirect = false,
        .skip_cert_common_name_check = false,
    };
    client_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&client_cfg);
    if (!client) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "创建 HTTP 客户端失败");
        }
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "HTTP 请求失败: %s", esp_err_to_name(err));
        }
        return err;
    }
    if (status < 200 || status >= 300) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "HTTP %d", status);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void build_placeholder_btc_chart(btc_chart_data_t *chart)
{
    if (!chart) {
        return;
    }

    memset(chart, 0, sizeof(*chart));
    chart->count = BTC_CHART_POINT_COUNT;
    chart->min_value = INT32_MAX;
    chart->max_value = INT32_MIN;

    for (size_t i = 0; i < BTC_CHART_POINT_COUNT; ++i) {
        int32_t wave = (((int32_t)((i * 7u) % 9u)) - 4) * 120;
        int32_t value = 65000 + (int32_t)(i * 55u) + wave;
        chart->values[i] = value;
        chart->min_value = MIN(chart->min_value, value);
        chart->max_value = MAX(chart->max_value, value);
    }

    if (chart->max_value <= chart->min_value) {
        chart->max_value = chart->min_value + 1;
    }

    int32_t padding = MAX(1, (chart->max_value - chart->min_value) / 10);
    chart->min_value = MAX(0, chart->min_value - padding);
    chart->max_value += padding;
    chart->valid = false;
}

static void resample_btc_chart_values(const double *source, size_t source_count, btc_chart_data_t *chart)
{
    if (!source || !chart || source_count == 0) {
        build_placeholder_btc_chart(chart);
        return;
    }

    memset(chart, 0, sizeof(*chart));
    chart->count = BTC_CHART_POINT_COUNT;

    int32_t min_value = INT32_MAX;
    int32_t max_value = INT32_MIN;

    for (size_t i = 0; i < BTC_CHART_POINT_COUNT; ++i) {
        double position = 0.0;
        if (BTC_CHART_POINT_COUNT > 1 && source_count > 1) {
            position = ((double)i * (double)(source_count - 1)) / (double)(BTC_CHART_POINT_COUNT - 1);
        }
        size_t lower = (size_t)position;
        if (lower >= source_count) {
            lower = source_count - 1;
        }
        size_t upper = (lower + 1 < source_count) ? (lower + 1) : lower;
        double fraction = position - (double)lower;
        double value = source[lower];
        if (upper != lower) {
            value = source[lower] + (source[upper] - source[lower]) * fraction;
        }

        int32_t rounded = (int32_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
        chart->values[i] = rounded;
        min_value = MIN(min_value, rounded);
        max_value = MAX(max_value, rounded);
    }

    if (max_value <= min_value) {
        max_value = min_value + 1;
    }

    int32_t padding = MAX(1, (max_value - min_value) / 8);
    chart->min_value = MAX(0, min_value - padding);
    chart->max_value = max_value + padding;
    chart->valid = true;
}

static bool parse_btc_chart_json(const cJSON *root, btc_chart_data_t *chart_out)
{
    if (!root || !chart_out) {
        return false;
    }

    const cJSON *prices = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "prices");
    if (!cJSON_IsArray(prices)) {
        return false;
    }

    double source[BTC_CHART_SOURCE_LIMIT] = {0};
    size_t source_count = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, prices) {
        if (!cJSON_IsArray(entry)) {
            continue;
        }
        const cJSON *price_item = cJSON_GetArrayItem(entry, 1);
        if (!cJSON_IsNumber(price_item)) {
            continue;
        }
        if (source_count >= BTC_CHART_SOURCE_LIMIT) {
            break;
        }
        source[source_count++] = price_item->valuedouble;
    }

    if (source_count == 0) {
        return false;
    }

    resample_btc_chart_values(source, source_count, chart_out);
    return chart_out->valid;
}

static esp_err_t perform_crypto_refresh(const watch_ai_config_t *cfg, char *status_out, size_t status_len, char *summary_out, size_t summary_len, char *updated_out, size_t updated_len, btc_chart_data_t *chart_out, char *error_out, size_t error_out_len)
{
    (void)cfg;
    if (!status_out || status_len == 0 || !summary_out || summary_len == 0 || !updated_out || updated_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chart_out) {
        memset(chart_out, 0, sizeof(*chart_out));
    }

    status_out[0] = '\0';
    summary_out[0] = '\0';
    updated_out[0] = '\0';
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    int waited = 0;
    while (!system_time_valid() && waited < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }

    http_buffer_t response = {
        .buf = calloc(1, MAX_HTTP_RESPONSE),
        .len = 0,
        .cap = MAX_HTTP_RESPONSE,
        .truncated = false,
    };
    if (!response.buf) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "分配行情响应缓冲失败");
        }
        return ESP_ERR_NO_MEM;
    }

    char price_url[256];
    snprintf(price_url, sizeof(price_url), "https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=usd&include_24hr_change=true", CRYPTO_IDS);

    esp_err_t err = fetch_http_response(price_url, &response, error_out, error_out_len);
    if (err != ESP_OK) {
        free(response.buf);
        return err;
    }

    cJSON *root = cJSON_Parse(response.buf ? response.buf : "");
    if (!root) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "行情响应不是有效 JSON");
        }
        free(response.buf);
        return ESP_FAIL;
    }

    char summary[320] = {0};
    for (size_t i = 0; i < sizeof(k_crypto_assets) / sizeof(k_crypto_assets[0]); ++i) {
        const crypto_quote_t *meta = &k_crypto_assets[i];
        const cJSON *coin = cJSON_GetObjectItemCaseSensitive(root, meta->id);
        double price = 0.0;
        double change = 0.0;
        if (cJSON_IsObject(coin)) {
            const cJSON *usd = cJSON_GetObjectItemCaseSensitive((cJSON *)coin, "usd");
            const cJSON *chg = cJSON_GetObjectItemCaseSensitive((cJSON *)coin, "usd_24h_change");
            if (cJSON_IsNumber(usd)) {
                price = usd->valuedouble;
            }
            if (cJSON_IsNumber(chg)) {
                change = chg->valuedouble;
            }
        }

        crypto_quote_t quote = *meta;
        quote.price_usd = price;
        quote.change_24h = change;

        char line[96] = {0};
        format_crypto_price_line(line, sizeof(line), &quote);
        if (summary[0]) {
            strncat(summary, "\n", sizeof(summary) - strlen(summary) - 1);
        }
        strncat(summary, line, sizeof(summary) - strlen(summary) - 1);
    }

    cJSON_Delete(root);
    free(response.buf);

    if (!summary[0]) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "行情解析失败");
        }
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    char updated[64] = {0};
    strftime(updated, sizeof(updated), "更新时间 %H:%M:%S", &tm_now);

    copy_string(status_out, status_len, "CoinGecko · 实时");
    copy_string(summary_out, summary_len, summary);
    copy_string(updated_out, updated_len, updated);

    if (chart_out) {
        http_buffer_t chart_response = {
            .buf = calloc(1, MAX_HTTP_RESPONSE),
            .len = 0,
            .cap = MAX_HTTP_RESPONSE,
            .truncated = false,
        };
        if (chart_response.buf) {
            char chart_url[256];
            snprintf(chart_url, sizeof(chart_url),
                     "https://api.coingecko.com/api/v3/coins/bitcoin/market_chart?vs_currency=usd&days=1&interval=hourly");
            if (fetch_http_response(chart_url, &chart_response, NULL, 0) == ESP_OK) {
                cJSON *chart_root = cJSON_Parse(chart_response.buf ? chart_response.buf : "");
                if (chart_root) {
                    bool parsed = parse_btc_chart_json(chart_root, chart_out);
                    if (!parsed) {
                        build_placeholder_btc_chart(chart_out);
                    }
                    cJSON_Delete(chart_root);
                } else {
                    build_placeholder_btc_chart(chart_out);
                }
            }
            free(chart_response.buf);
        }
        if (!chart_out->count) {
            build_placeholder_btc_chart(chart_out);
        }
    }
    return ESP_OK;
}

typedef struct {
    char url[256];
    char api_key[192];
    char model[64];
    char system_prompt[512];
    float temperature;
    int max_tokens;
    int timeout_ms;
} chat_request_context_t;

static esp_err_t http_client_event_handler(esp_http_client_event_t *evt)
{
    http_buffer_t *out = (http_buffer_t *)evt->user_data;
    if (!out) {
        return ESP_OK;
    }

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len > 0 && out->buf && out->cap > 0) {
                size_t available = (out->cap > out->len) ? (out->cap - out->len - 1) : 0;
                size_t copy_len = MIN((size_t)evt->data_len, available);
                if (copy_len > 0) {
                    memcpy(out->buf + out->len, evt->data, copy_len);
                    out->len += copy_len;
                    out->buf[out->len] = '\0';
                }
                if (copy_len < (size_t)evt->data_len) {
                    out->truncated = true;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t http_binary_event_handler(esp_http_client_event_t *evt)
{
    binary_buffer_t *out = (binary_buffer_t *)evt->user_data;
    if (!out) {
        return ESP_OK;
    }

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len > 0) {
                esp_err_t err = binary_buffer_append(out, (const uint8_t *)evt->data, (size_t)evt->data_len);
                if (err != ESP_OK) {
                    return err;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static bool extract_reply_string_from_json(cJSON *root, char *reply, size_t reply_len)
{
    if (!root || !reply || reply_len == 0) {
        return false;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize((cJSON *)choices) > 0) {
        const cJSON *choice0 = cJSON_GetArrayItem((cJSON *)choices, 0);
        const cJSON *message = choice0 ? cJSON_GetObjectItemCaseSensitive((cJSON *)choice0, "message") : NULL;
        const cJSON *content = message ? cJSON_GetObjectItemCaseSensitive((cJSON *)message, "content") : NULL;
        const cJSON *text = choice0 ? cJSON_GetObjectItemCaseSensitive((cJSON *)choice0, "text") : NULL;

        if (cJSON_IsString(content) && content->valuestring) {
            copy_string(reply, reply_len, content->valuestring);
            return true;
        }
        if (cJSON_IsString(text) && text->valuestring) {
            copy_string(reply, reply_len, text->valuestring);
            return true;
        }
    }

    const cJSON *output_text = cJSON_GetObjectItemCaseSensitive(root, "output_text");
    if (cJSON_IsString(output_text) && output_text->valuestring) {
        copy_string(reply, reply_len, output_text->valuestring);
        return true;
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        copy_string(reply, reply_len, text->valuestring);
        return true;
    }

    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(message) && message->valuestring) {
        copy_string(reply, reply_len, message->valuestring);
        return true;
    }

    return false;
}

static esp_err_t perform_chat_request(const watch_ai_config_t *cfg, const char *user_prompt, char *reply_out, size_t reply_out_len, char *error_out, size_t error_out_len)
{
    if (!cfg || !user_prompt || !reply_out || reply_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    reply_out[0] = '\0';
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    wait_for_time_if_https(cfg);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "创建请求 JSON 失败");
        }
        return ESP_ERR_NO_MEM;
    }
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (!messages) {
        cJSON_Delete(root);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "创建请求 JSON 失败");
        }
        return ESP_ERR_NO_MEM;
    }

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", cfg->system_prompt);
    cJSON_AddItemToArray(messages, sys);

    state_lock();
    size_t history_count = g_app.history_count;
    size_t start = 0;
    size_t keep = (history_count > MAX_HISTORY_MESSAGES) ? MAX_HISTORY_MESSAGES : history_count;
    if (history_count > MAX_HISTORY_MESSAGES) {
        start = history_count - MAX_HISTORY_MESSAGES;
    }
    if (history_count > 0) {
        const chat_message_t *last = &g_app.history[history_count - 1];
        if (last->content && strcmp(last->role, "user") == 0 && strcmp(last->content, user_prompt) == 0) {
            history_count--;
            if (keep > history_count) {
                keep = history_count;
                start = (history_count > MAX_HISTORY_MESSAGES) ? (history_count - MAX_HISTORY_MESSAGES) : 0;
            }
        }
    }
    for (size_t i = start; i < start + keep && i < history_count; ++i) {
        if (!g_app.history[i].content) {
            continue;
        }
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", g_app.history[i].role);
        cJSON_AddStringToObject(msg, "content", g_app.history[i].content);
        cJSON_AddItemToArray(messages, msg);
    }
    state_unlock();

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_prompt);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(root, "stream", false);
    if (cfg->max_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_tokens", cfg->max_tokens);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "序列化请求失败");
        }
        return ESP_ERR_NO_MEM;
    }

    http_buffer_t response = {
        .buf = calloc(1, MAX_HTTP_RESPONSE),
        .len = 0,
        .cap = MAX_HTTP_RESPONSE,
        .truncated = false,
    };
    if (!response.buf) {
        free(payload);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "分配响应缓冲失败");
        }
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t client_cfg = {
        .url = cfg->api_url,
        .timeout_ms = cfg->timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_client_event_handler,
        .user_data = &response,
        .user_agent = "WatchAI/1.0",
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .disable_auto_redirect = false,
        .skip_cert_common_name_check = false,
    };

    if (strncasecmp(cfg->api_url, "https://", 8) == 0) {
        client_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&client_cfg);
    if (!client) {
        free(payload);
        free(response.buf);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "创建 HTTP 客户端失败");
        }
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (string_has_value(cfg->api_key)) {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", cfg->api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);

    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "请求失败: %s", esp_err_to_name(err));
        }
        free(response.buf);
        return err;
    }

    if (status < 200 || status >= 300) {
        char temp[MAX_ERROR_LEN] = {0};
        cJSON *resp_json = cJSON_Parse(response.buf ? response.buf : "");
        if (resp_json) {
            const cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(resp_json, "error");
            const cJSON *message = NULL;
            if (cJSON_IsObject(error_obj)) {
                message = cJSON_GetObjectItemCaseSensitive((cJSON *)error_obj, "message");
            }
            if (cJSON_IsString(message) && message->valuestring) {
                copy_string(temp, sizeof(temp), message->valuestring);
            } else {
                extract_reply_string_from_json(resp_json, temp, sizeof(temp));
            }
            cJSON_Delete(resp_json);
        }
        if (!temp[0]) {
            snprintf(temp, sizeof(temp), "HTTP %d", status);
        }
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "%s", temp);
        }
        free(response.buf);
        return ESP_FAIL;
    }

    cJSON *resp_json = cJSON_Parse(response.buf ? response.buf : "");
    if (!resp_json) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "响应不是有效 JSON");
        }
        free(response.buf);
        return ESP_FAIL;
    }

    if (!extract_reply_string_from_json(resp_json, reply_out, reply_out_len)) {
        const cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(resp_json, "error");
        const cJSON *message = NULL;
        if (cJSON_IsObject(error_obj)) {
            message = cJSON_GetObjectItemCaseSensitive((cJSON *)error_obj, "message");
        }
        if (cJSON_IsString(message) && message->valuestring) {
            copy_string(reply_out, reply_out_len, message->valuestring);
        } else {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "找不到回复内容");
            }
            cJSON_Delete(resp_json);
            free(response.buf);
            return ESP_FAIL;
        }
    }

    if (response.truncated && error_out && error_out_len) {
        snprintf(error_out, error_out_len, "响应已截断，回复可能不完整");
    }

    cJSON_Delete(resp_json);
    free(response.buf);
    return ESP_OK;
}

static bool extract_error_string_from_json(cJSON *root, char *error_out, size_t error_out_len)
{
    if (!root || !error_out || error_out_len == 0) {
        return false;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive((cJSON *)error, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            copy_string(error_out, error_out_len, message->valuestring);
            return true;
        }
        const cJSON *detail = cJSON_GetObjectItemCaseSensitive((cJSON *)error, "detail");
        if (cJSON_IsString(detail) && detail->valuestring) {
            copy_string(error_out, error_out_len, detail->valuestring);
            return true;
        }
    } else if (cJSON_IsString(error) && error->valuestring) {
        copy_string(error_out, error_out_len, error->valuestring);
        return true;
    }

    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(message) && message->valuestring) {
        copy_string(error_out, error_out_len, message->valuestring);
        return true;
    }

    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
    if (cJSON_IsString(detail) && detail->valuestring) {
        copy_string(error_out, error_out_len, detail->valuestring);
        return true;
    }

    return false;
}

static esp_err_t perform_tts_request(const watch_ai_config_t *cfg, const char *text, binary_buffer_t *audio_out, char *error_out, size_t error_out_len)
{
    if (!cfg || !text || !audio_out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(audio_out, 0, sizeof(*audio_out));
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    wait_for_time_if_https(cfg);

    const bool azure_tts = tts_uses_azure_speech_endpoint(cfg->tts_url);
    const char *tts_key = azure_tts ? cfg->tts_api_key : effective_tts_api_key(cfg);
    binary_buffer_t request_body = {0};
    char *payload = NULL;
    const char *post_field = NULL;
    int post_len = 0;

    if (azure_tts) {
        esp_err_t err = build_azure_tts_payload(text, cfg->tts_voice, cfg->tts_speed, &request_body);
        if (err != ESP_OK) {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "构建微软语音请求失败");
            }
            binary_buffer_free(&request_body);
            return err;
        }
        post_field = (const char *)request_body.data;
        post_len = (int)request_body.len;
    } else {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "创建 TTS 请求 JSON 失败");
            }
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(root, "model", cfg->tts_model);
        cJSON_AddStringToObject(root, "input", text);
        cJSON_AddStringToObject(root, "voice", cfg->tts_voice);
        cJSON_AddStringToObject(root, "response_format", cfg->tts_response_format);
        cJSON_AddNumberToObject(root, "speed", cfg->tts_speed);

        payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!payload) {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "序列化 TTS 请求失败");
            }
            return ESP_ERR_NO_MEM;
        }
        post_field = payload;
        post_len = (int)strlen(payload);
    }

    binary_buffer_t response = {0};
    esp_http_client_config_t client_cfg = {
        .url = cfg->tts_url,
        .timeout_ms = cfg->timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_binary_event_handler,
        .user_data = &response,
        .user_agent = "WatchAI/1.0",
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .disable_auto_redirect = false,
        .skip_cert_common_name_check = false,
    };

    if (strncasecmp(cfg->tts_url, "https://", 8) == 0) {
        client_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&client_cfg);
    if (!client) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "创建 TTS HTTP 客户端失败");
        }
        free(payload);
        binary_buffer_free(&request_body);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (azure_tts) {
        esp_http_client_set_header(client, "Content-Type", "application/ssml+xml");
        esp_http_client_set_header(client, "X-Microsoft-OutputFormat", TTS_AZURE_OUTPUT_FORMAT);
        if (string_has_value(tts_key)) {
            esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", tts_key);
        }
    } else {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (string_has_value(tts_key)) {
            char auth_header[256];
            snprintf(auth_header, sizeof(auth_header), "Bearer %s", tts_key);
            esp_http_client_set_header(client, "Authorization", auth_header);
        }
    }
    esp_http_client_set_post_field(client, post_field, post_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    client = NULL;
    free(payload);
    payload = NULL;
    binary_buffer_free(&request_body);

    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "TTS 请求失败: %s", esp_err_to_name(err));
        }
        binary_buffer_free(&response);
        return err;
    }

    if (status < 200 || status >= 300) {
        char temp[MAX_ERROR_LEN] = {0};
        if (response.data && response.len > 0) {
            char *body = calloc(1, response.len + 1);
            if (body) {
                memcpy(body, response.data, response.len);
                cJSON *resp_json = cJSON_Parse(body);
                if (resp_json) {
                    if (!extract_error_string_from_json(resp_json, temp, sizeof(temp))) {
                        extract_reply_string_from_json(resp_json, temp, sizeof(temp));
                    }
                    cJSON_Delete(resp_json);
                } else {
                    size_t copy_len = response.len;
                    if (copy_len >= sizeof(temp)) {
                        copy_len = sizeof(temp) - 1;
                    }
                    memcpy(temp, response.data, copy_len);
                    temp[copy_len] = '\0';
                    for (size_t i = 0; i < copy_len; ++i) {
                        if (temp[i] == '\r' || temp[i] == '\n' || temp[i] == '\t') {
                            temp[i] = ' ';
                        }
                    }
                }
                free(body);
            }
        }
        if (!temp[0]) {
            snprintf(temp, sizeof(temp), "HTTP %d", status);
        }
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "%s", temp);
        }
        binary_buffer_free(&response);
        return ESP_FAIL;
    }

    if (response.truncated) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "TTS 音频返回过大，已截断");
        }
        binary_buffer_free(&response);
        return ESP_ERR_NO_MEM;
    }

    if (!response.data || response.len == 0) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "TTS 返回为空");
        }
        binary_buffer_free(&response);
        return ESP_FAIL;
    }

    *audio_out = response;
    return ESP_OK;
}

static esp_err_t build_stt_multipart_body(const uint8_t *wav_data, size_t wav_len, binary_buffer_t *body)
{
    if (!wav_data || wav_len == 0 || !body) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *boundary = "----watchai-stt-boundary";
    esp_err_t err = binary_buffer_append_format(body, "--%s\r\n", boundary);
    if (err != ESP_OK) {
        return err;
    }

    err = binary_buffer_append_format(
        body,
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n",
        STT_DEFAULT_MODEL);
    if (err != ESP_OK) {
        return err;
    }

    err = binary_buffer_append_format(body, "--%s\r\n", boundary);
    if (err != ESP_OK) {
        return err;
    }

    err = binary_buffer_append_format(
        body,
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n");
    if (err != ESP_OK) {
        return err;
    }

    err = binary_buffer_append(body, wav_data, wav_len);
    if (err != ESP_OK) {
        return err;
    }

    return binary_buffer_append_format(body, "\r\n--%s--\r\n", boundary);
}

static esp_err_t perform_stt_request(const watch_ai_config_t *cfg, const uint8_t *wav_data, size_t wav_len, char *text_out, size_t text_out_len, char *error_out, size_t error_out_len)
{
    if (!cfg || !wav_data || wav_len == 0 || !text_out || text_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    text_out[0] = '\0';
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    wait_for_time_if_https(cfg);

    char stt_url[256] = {0};
    derive_stt_url_from_api_url(cfg->api_url, stt_url, sizeof(stt_url));
    if (!string_has_value(stt_url)) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "无法从 API URL 派生语音转写地址");
        }
        return ESP_ERR_INVALID_ARG;
    }

    binary_buffer_t body = {0};
    esp_err_t err = build_stt_multipart_body(wav_data, wav_len, &body);
    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "构建语音转写请求失败");
        }
        binary_buffer_free(&body);
        return err;
    }

    http_buffer_t response = {
        .buf = calloc(1, MAX_HTTP_RESPONSE),
        .len = 0,
        .cap = MAX_HTTP_RESPONSE,
        .truncated = false,
    };
    if (!response.buf) {
        binary_buffer_free(&body);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "分配转写响应缓冲失败");
        }
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t client_cfg = {
        .url = stt_url,
        .timeout_ms = cfg->timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_client_event_handler,
        .user_data = &response,
        .user_agent = "WatchAI/1.0",
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .disable_auto_redirect = false,
        .skip_cert_common_name_check = false,
    };

    if (strncasecmp(stt_url, "https://", 8) == 0) {
        client_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&client_cfg);
    if (!client) {
        binary_buffer_free(&body);
        free(response.buf);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "创建语音转写 HTTP 客户端失败");
        }
        return ESP_FAIL;
    }

    const char *boundary = "----watchai-stt-boundary";
    char content_type[128] = {0};
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    if (string_has_value(cfg->api_key)) {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", cfg->api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, (const char *)body.data, (int)body.len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    binary_buffer_free(&body);

    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "语音转写请求失败: %s", esp_err_to_name(err));
        }
        free(response.buf);
        return err;
    }

    if (status < 200 || status >= 300) {
        char temp[MAX_ERROR_LEN] = {0};
        cJSON *resp_json = cJSON_Parse(response.buf ? response.buf : "");
        if (resp_json) {
            if (!extract_reply_string_from_json(resp_json, temp, sizeof(temp))) {
                const cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(resp_json, "error");
                const cJSON *message = NULL;
                if (cJSON_IsObject(error_obj)) {
                    message = cJSON_GetObjectItemCaseSensitive((cJSON *)error_obj, "message");
                }
                if (cJSON_IsString(message) && message->valuestring) {
                    copy_string(temp, sizeof(temp), message->valuestring);
                }
            }
            cJSON_Delete(resp_json);
        }
        if (!temp[0]) {
            snprintf(temp, sizeof(temp), "HTTP %d", status);
        }
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "%s", temp);
        }
        free(response.buf);
        return ESP_FAIL;
    }

    cJSON *resp_json = cJSON_Parse(response.buf ? response.buf : "");
    if (!resp_json) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "转写响应不是有效 JSON");
        }
        free(response.buf);
        return ESP_FAIL;
    }

    if (!extract_reply_string_from_json(resp_json, text_out, text_out_len)) {
        const cJSON *error_obj = cJSON_GetObjectItemCaseSensitive(resp_json, "error");
        const cJSON *message = NULL;
        if (cJSON_IsObject(error_obj)) {
            message = cJSON_GetObjectItemCaseSensitive((cJSON *)error_obj, "message");
        }
        if (cJSON_IsString(message) && message->valuestring) {
            copy_string(text_out, text_out_len, message->valuestring);
        } else {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "找不到转写文本");
            }
            cJSON_Delete(resp_json);
            free(response.buf);
            return ESP_FAIL;
        }
    }

    cJSON_Delete(resp_json);
    free(response.buf);
    return ESP_OK;
}

static esp_err_t record_voice_wav(binary_buffer_t *wav_out, char *error_out, size_t error_out_len)
{
    if (!wav_out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(wav_out, 0, sizeof(*wav_out));
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    size_t max_pcm_bytes = (size_t)VOICE_SAMPLE_RATE * VOICE_CHANNELS * (VOICE_BITS_PER_SAMPLE / 8) * VOICE_MAX_SECONDS;
    size_t total_bytes = 44 + max_pcm_bytes;
    uint8_t *data = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_malloc(total_bytes, MALLOC_CAP_8BIT);
    }
    if (!data) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "分配录音缓冲失败");
        }
        return ESP_ERR_NO_MEM;
    }
    memset(data, 0, total_bytes);

    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) {
        free(data);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "麦克风初始化失败");
        }
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = VOICE_BITS_PER_SAMPLE,
        .channel = VOICE_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = VOICE_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    int open_ret = esp_codec_dev_open(mic, &fs);
    if (open_ret != ESP_CODEC_DEV_OK) {
        free(data);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "打开麦克风失败(%d)", open_ret);
        }
        return ESP_FAIL;
    }

    size_t pcm_bytes = 0;
    bool speech_started = false;
    int idle_ms = 0;
    int total_ms = 0;
    size_t chunk_bytes = VOICE_READ_CHUNK_BYTES;
    if (chunk_bytes > max_pcm_bytes) {
        chunk_bytes = max_pcm_bytes;
    }
    if (chunk_bytes < 512) {
        chunk_bytes = 512;
    }
    if ((chunk_bytes & 1u) != 0u) {
        ++chunk_bytes;
    }

    while (pcm_bytes < max_pcm_bytes) {
        size_t request = MIN(chunk_bytes, max_pcm_bytes - pcm_bytes);
        int got = esp_codec_dev_read(mic, data + 44 + pcm_bytes, (int)request);
        if (got < 0) {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "录音失败(%d)", got);
            }
            break;
        }
        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if ((got & 1) != 0) {
            --got;
        }
        if (got <= 0) {
            continue;
        }

        size_t got_bytes = (size_t)got;
        if (pcm_bytes + got_bytes > max_pcm_bytes) {
            got_bytes = max_pcm_bytes - pcm_bytes;
        }
        if (got_bytes == 0) {
            break;
        }

        pcm_bytes += got_bytes;
        size_t sample_count = got_bytes / 2;
        int64_t energy = pcm_chunk_energy_16bit((const int16_t *)(data + 44 + pcm_bytes - got_bytes), sample_count);
        int chunk_ms = (int)((got_bytes * 1000u) / (VOICE_SAMPLE_RATE * VOICE_CHANNELS * (VOICE_BITS_PER_SAMPLE / 8)));
        if (chunk_ms <= 0) {
            chunk_ms = 1;
        }
        total_ms += chunk_ms;

        if (energy > VOICE_SPEECH_THRESHOLD) {
            speech_started = true;
            idle_ms = 0;
        } else if (speech_started) {
            idle_ms += chunk_ms;
        }

        if (!speech_started && total_ms >= VOICE_MAX_IDLE_MS) {
            break;
        }
        if (speech_started && idle_ms >= VOICE_SILENCE_MS) {
            break;
        }
    }

    esp_codec_dev_close(mic);

    if (!speech_started) {
        free(data);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "没有检测到语音，请再靠近一点试试");
        }
        return ESP_ERR_NOT_FOUND;
    }

    if (pcm_bytes == 0) {
        free(data);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "录音为空");
        }
        return ESP_FAIL;
    }

    esp_err_t header_err = build_wav_header(data, pcm_bytes, &fs);
    if (header_err != ESP_OK) {
        free(data);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "构建 WAV 头失败");
        }
        return header_err;
    }

    wav_out->data = data;
    wav_out->len = pcm_bytes + 44;
    wav_out->cap = total_bytes;
    wav_out->truncated = false;
    return ESP_OK;
}

static void app_start_audio(void)
{
    state_lock();
    if (g_app.speaker_dev) {
        state_unlock();
        return;
    }
    state_unlock();

    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    if (!speaker) {
        state_lock();
        set_last_error_locked("音频输出设备初始化失败");
        state_unlock();
        return;
    }

    state_lock();
    if (!g_app.speaker_dev) {
        g_app.speaker_dev = speaker;
        set_status_line_locked("音频输出已就绪");
    }
    state_unlock();
}

static void schedule_tts_reply(const watch_ai_config_t *cfg, const char *text)
{
    if (!cfg || !cfg->tts_enabled || !string_has_value(text)) {
        return;
    }

    char trimmed[TTS_REPLY_TRIM_BYTES + 1] = {0};
    char normalized[TTS_REPLY_TRIM_BYTES + 1] = {0};
    utf8_safe_copy(text, trimmed, sizeof(trimmed));
    normalize_text_for_tts(trimmed, normalized, sizeof(normalized));
    if (!string_has_value(normalized)) {
        return;
    }

    state_lock();
    if (g_app.tts_busy) {
        state_unlock();
        ESP_LOGI(TAG, "TTS busy, skip this reply");
        return;
    }
    g_app.tts_busy = true;
    set_status_line_locked("正在朗读自然人声...");
    state_unlock();

    app_start_audio();

    state_lock();
    if (!g_app.speaker_dev) {
        g_app.tts_busy = false;
        set_last_error_locked("语音输出设备初始化失败");
        set_status_line_locked("语音播放失败");
        state_unlock();
        return;
    }
    state_unlock();

    tts_async_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        state_lock();
        g_app.tts_busy = false;
        set_last_error_locked("创建语音任务失败");
        set_status_line_locked("语音播放失败");
        state_unlock();
        return;
    }

    job->cfg = *cfg;
    copy_string(job->reply, sizeof(job->reply), normalized);

    BaseType_t created = xTaskCreate(
        tts_play_task,
        "tts_play",
        12288,
        job,
        5,
        NULL);
    if (created != pdPASS) {
        free(job);
        state_lock();
        g_app.tts_busy = false;
        set_last_error_locked("创建语音任务失败");
        set_status_line_locked("语音播放失败");
        state_unlock();
    }
}

static esp_err_t run_chat_turn(const watch_ai_config_t *cfg, const char *user_prompt, char *reply_out, size_t reply_out_len, char *error_out, size_t error_out_len)
{
    if (!cfg || !user_prompt || !reply_out || reply_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    reply_out[0] = '\0';
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    state_lock();
    if (g_app.chat_busy) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "已有任务在处理，请稍等");
        }
        state_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    g_app.chat_busy = true;
    clear_last_error_locked();
    set_last_user_locked(user_prompt);
    set_status_line_locked("AI 正在思考...");
    (void)history_append_locked("user", user_prompt);
    state_unlock();

    char reply[MAX_REPLY_LEN] = {0};
    char local_error[MAX_ERROR_LEN] = {0};
    esp_err_t err = perform_chat_request(cfg, user_prompt, reply, sizeof(reply), local_error, sizeof(local_error));

    state_lock();
    g_app.chat_busy = false;
    if (err == ESP_OK) {
        set_last_reply_locked(reply);
        clear_last_error_locked();
        if (history_append_locked("assistant", reply) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to append assistant reply to history");
        }
        set_status_line_locked("收到回复");
    } else {
        if (local_error[0]) {
            set_last_error_locked("%s", local_error);
        } else {
            set_last_error_locked("聊天请求失败: %s", esp_err_to_name(err));
        }
        set_status_line_locked("聊天失败");
    }
    state_unlock();

    if (err == ESP_OK) {
        copy_string(reply_out, reply_out_len, reply);
        schedule_tts_reply(cfg, reply);
    } else if (error_out && error_out_len) {
        copy_string(error_out, error_out_len, local_error[0] ? local_error : "聊天失败");
    }

    return err;
}

static void tts_play_task(void *arg)
{
    tts_async_job_t *job = (tts_async_job_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }

    char error_text[MAX_ERROR_LEN] = {0};
    binary_buffer_t audio = {0};
    esp_err_t err = perform_tts_request(&job->cfg, job->reply, &audio, error_text, sizeof(error_text));

    if (err == ESP_OK) {
        wav_audio_info_t wav = {0};
        if (!parse_wav_audio(audio.data, audio.len, &wav)) {
            err = ESP_FAIL;
            copy_string(error_text, sizeof(error_text), "TTS 返回的不是 16-bit WAV");
        } else {
            app_start_audio();
            state_lock();
            esp_codec_dev_handle_t speaker = g_app.speaker_dev;
            state_unlock();

            if (!speaker) {
                err = ESP_FAIL;
                copy_string(error_text, sizeof(error_text), "音频输出设备不可用");
            } else {
                esp_codec_dev_sample_info_t fs = {0};
                build_wav_sample_info(&wav, &fs);
                int open_ret = esp_codec_dev_open(speaker, &fs);
                if (open_ret != ESP_CODEC_DEV_OK) {
                    err = ESP_FAIL;
                    snprintf(error_text, sizeof(error_text), "打开扬声器失败(%d)", open_ret);
                } else {
                    int vol_ret = esp_codec_dev_set_out_vol(speaker, TTS_DEFAULT_VOLUME);
                    if (vol_ret != ESP_CODEC_DEV_OK) {
                        ESP_LOGW(TAG, "Failed to set speaker volume: %d", vol_ret);
                    }

                    size_t offset = wav.data_offset;
                    size_t end = wav.data_offset + wav.data_len;
                    while (offset < end) {
                        size_t chunk = MIN((size_t)4096, end - offset);
                        int written = esp_codec_dev_write(speaker, audio.data + offset, (int)chunk);
                        if (written <= 0) {
                            err = ESP_FAIL;
                            snprintf(error_text, sizeof(error_text), "播放失败(%d)", written);
                            break;
                        }
                        offset += (size_t)written;
                    }
                    esp_codec_dev_close(speaker);
                }
            }
        }
    }

    binary_buffer_free(&audio);

    state_lock();
    g_app.tts_busy = false;
    if (err == ESP_OK) {
        clear_last_error_locked();
        set_status_line_locked("自然人声朗读完成");
    } else {
        if (error_text[0]) {
            set_last_error_locked("语音播放失败: %s", error_text);
        } else {
            set_last_error_locked("语音播放失败");
        }
        set_status_line_locked("语音播放失败");
    }
    state_unlock();

    free(job);
    vTaskDelete(NULL);
}

static void chat_turn_task(void *arg)
{
    chat_async_job_t *job = (chat_async_job_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }

    char reply[MAX_REPLY_LEN] = {0};
    char error_text[MAX_ERROR_LEN] = {0};
    (void)run_chat_turn(&job->cfg, job->message, reply, sizeof(reply), error_text, sizeof(error_text));
    ui_chat_refresh_form();
    free(job);
    vTaskDelete(NULL);
}

static void ui_start_voice_chat(void)
{
    if (!g_app.display) {
        return;
    }

    watch_ai_config_t cfg;
    bool voice_busy = false;
    bool chat_busy = false;
    bool tts_busy = false;

    state_lock();
    cfg = g_app.config;
    voice_busy = g_app.voice_busy;
    chat_busy = g_app.chat_busy;
    tts_busy = g_app.tts_busy;
    state_unlock();

    if (voice_busy || chat_busy || tts_busy) {
        state_lock();
        set_last_error_locked("已有任务在处理中，请稍等");
        set_status_line_locked("已有任务在处理中，请稍等");
        state_unlock();
        ui_chat_set_status("已有任务在处理中，请稍等");
        ui_refresh_timer(NULL);
        return;
    }

    voice_async_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        state_lock();
        set_last_error_locked("创建语音任务失败");
        set_status_line_locked("语音聊天失败");
        state_unlock();
        ui_chat_set_status("创建语音任务失败");
        ui_refresh_timer(NULL);
        return;
    }
    job->cfg = cfg;

    state_lock();
    g_app.voice_busy = true;
    clear_last_error_locked();
    set_status_line_locked("正在录音，请直接说话...");
    state_unlock();

    ui_show_chat_screen();
    ui_chat_set_status("正在录音，请直接说话...");

    BaseType_t created = xTaskCreate(
        voice_chat_task,
        "voice_chat",
        16384,
        job,
        5,
        NULL);
    if (created != pdPASS) {
        state_lock();
        g_app.voice_busy = false;
        set_last_error_locked("创建语音任务失败");
        set_status_line_locked("语音聊天失败");
        state_unlock();
        ui_chat_set_status("创建语音任务失败");
        ui_chat_refresh_form();
        ui_refresh_timer(NULL);
        free(job);
    }
}

static void voice_chat_task(void *arg)
{
    voice_async_job_t *job = (voice_async_job_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }

    char error_text[MAX_ERROR_LEN] = {0};
    char transcript[MAX_MESSAGE_LEN] = {0};
    char reply[MAX_REPLY_LEN] = {0};
    binary_buffer_t wav = {0};
    esp_err_t err = ESP_OK;

    ui_chat_set_status("正在录音，请直接说话...");

    err = record_voice_wav(&wav, error_text, sizeof(error_text));
    if (err == ESP_OK) {
        ui_chat_set_status("正在转写语音...");
        err = perform_stt_request(
            &job->cfg,
            wav.data,
            wav.len,
            transcript,
            sizeof(transcript),
            error_text,
            sizeof(error_text));
    }
    binary_buffer_free(&wav);

    if (err == ESP_OK) {
        char *trim = transcript;
        while (*trim && isspace((unsigned char)*trim)) {
            ++trim;
        }
        char *end = trim + strlen(trim);
        while (end > trim && isspace((unsigned char)end[-1])) {
            --end;
        }
        *end = '\0';

        if (!string_has_value(trim)) {
            err = ESP_ERR_NOT_FOUND;
            copy_string(error_text, sizeof(error_text), "没有识别到有效语音");
        } else {
            ui_chat_set_status("语音已识别，正在请求 AI...");
            err = run_chat_turn(&job->cfg, trim, reply, sizeof(reply), error_text, sizeof(error_text));
        }
    }

    state_lock();
    g_app.voice_busy = false;
    if (err == ESP_OK) {
        clear_last_error_locked();
        set_status_line_locked("语音聊天完成");
    } else {
        if (error_text[0]) {
            set_last_error_locked("语音聊天失败: %s", error_text);
        } else {
            set_last_error_locked("语音聊天失败");
        }
        set_status_line_locked("语音聊天失败");
    }
    state_unlock();

    if (err == ESP_OK) {
        ui_chat_set_status("语音聊天完成");
    } else {
        ui_chat_set_status(error_text[0] ? error_text : "语音聊天失败");
    }
    ui_chat_refresh_form();
    ui_refresh_timer(NULL);

    free(job);
    vTaskDelete(NULL);
}

static void crypto_refresh_task(void *arg)
{
    (void)arg;

    while (true) {
        watch_ai_config_t cfg;
        bool already_busy = false;
        bool sta_connected = false;

        state_lock();
        already_busy = g_app.crypto_busy;
        if (!already_busy) {
            g_app.crypto_busy = true;
            clear_last_error_locked();
            set_status_line_locked("正在刷新加密货币行情...");
        }
        cfg = g_app.config;
        sta_connected = g_app.sta_connected;
        state_unlock();

        if (already_busy) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!sta_connected) {
            state_lock();
            g_app.crypto_busy = false;
            set_crypto_state_locked(
                "CoinGecko · 等待 Wi-Fi",
                "连接 Wi-Fi 后自动刷新 BTC / ETH / SOL。",
                "离线");
            set_status_line_locked("等待 Wi-Fi 后刷新行情");
            state_unlock();
            ui_refresh_timer(NULL);
            vTaskDelay(pdMS_TO_TICKS(CRYPTO_OFFLINE_RETRY_MS));
            continue;
        }

        char status_text[96] = {0};
        char summary_text[320] = {0};
        char updated_text[64] = {0};
        btc_chart_data_t chart_data = {0};
        char error_text[MAX_ERROR_LEN] = {0};
        esp_err_t err = perform_crypto_refresh(
            &cfg,
            status_text,
            sizeof(status_text),
            summary_text,
            sizeof(summary_text),
            updated_text,
            sizeof(updated_text),
            &chart_data,
            error_text,
            sizeof(error_text));

        state_lock();
        g_app.crypto_busy = false;
        if (err == ESP_OK) {
            set_crypto_state_locked(status_text, summary_text, updated_text);
            if (chart_data.valid && chart_data.count > 0) {
                g_app.crypto_chart_point_count = chart_data.count;
                memcpy(g_app.crypto_chart_values, chart_data.values, sizeof(chart_data.values));
                g_app.crypto_chart_min_value = chart_data.min_value;
                g_app.crypto_chart_max_value = chart_data.max_value;
                g_app.crypto_chart_generation++;
                g_app.crypto_chart_dirty = true;
            }
            clear_last_error_locked();
            set_status_line_locked("行情已更新");
        } else {
            if (!string_has_value(g_app.crypto_summary)) {
                copy_string(g_app.crypto_summary, sizeof(g_app.crypto_summary), "BTC / ETH / SOL 行情稍后显示。");
            }
            copy_string(g_app.crypto_status, sizeof(g_app.crypto_status), "CoinGecko · 失败");
            copy_string(g_app.crypto_updated, sizeof(g_app.crypto_updated), "稍后重试");
            g_app.crypto_chart_dirty = false;
            if (error_text[0]) {
                set_last_error_locked("行情刷新失败: %s", error_text);
            } else {
                set_last_error_locked("行情刷新失败");
            }
            set_status_line_locked("行情刷新失败");
        }
        state_unlock();

        ui_refresh_timer(NULL);
        vTaskDelay(pdMS_TO_TICKS(CRYPTO_REFRESH_INTERVAL_MS));
    }
}

static void schedule_wifi_restart(bool force_ap)
{
    state_lock();
    if (force_ap) {
        g_app.wifi_restart_force_ap = true;
    }
    if (g_app.wifi_restart_pending) {
        state_unlock();
        return;
    }
    g_app.wifi_restart_pending = true;
    state_unlock();

    BaseType_t created = xTaskCreate(
        wifi_restart_task,
        "wifi_restart",
        4096,
        (void *)(intptr_t)(force_ap ? 1 : 0),
        5,
        NULL);
    if (created != pdPASS) {
        state_lock();
        g_app.wifi_restart_pending = false;
        set_last_error_locked("创建 Wi-Fi 重启任务失败");
        state_unlock();
    }
}

static void wifi_watchdog_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    state_lock();
    bool connected = g_app.sta_connected;
    bool connecting = g_app.sta_connecting;
    bool ap_running = g_app.ap_running;
    state_unlock();
    if (!connected && connecting && !ap_running) {
        ESP_LOGW(TAG, "STA connect timeout, forcing AP mode");
        schedule_wifi_restart(true);
    }
    vTaskDelete(NULL);
}

static void wifi_restart_task(void *arg)
{
    bool force_ap = ((intptr_t)arg) != 0;
    vTaskDelay(pdMS_TO_TICKS(WIFI_RESTART_DELAY_MS));

    state_lock();
    force_ap = force_ap || g_app.wifi_restart_force_ap;
    g_app.wifi_restart_force_ap = false;
    g_app.wifi_restarting = true;
    g_app.wifi_restart_pending = false;
    state_unlock();

    watch_ai_config_t cfg;
    state_lock();
    cfg = g_app.config;
    state_unlock();

    bool has_sta = string_has_value(cfg.wifi_ssid);
    bool want_ap = force_ap || cfg.ap_enabled || !has_sta;
    wifi_mode_t mode = WIFI_MODE_STA;
    if (want_ap && has_sta) {
        mode = WIFI_MODE_APSTA;
    } else if (want_ap) {
        mode = WIFI_MODE_AP;
    }

    state_lock();
    set_status_line_locked("Wi-Fi 重启中...");
    clear_last_error_locked();
    state_unlock();

    esp_wifi_stop();
    esp_wifi_set_mode(mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_config_t sta_cfg = {0};
        copy_string((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg.wifi_ssid);
        copy_string((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg.wifi_password);
        sta_cfg.sta.threshold.authmode = string_has_value(cfg.wifi_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            state_lock();
            set_last_error_locked("设置 STA 失败: %s", esp_err_to_name(err));
            state_unlock();
        }
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t ap_cfg = {0};
        copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), cfg.ap_ssid);
        copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), cfg.ap_password);
        ap_cfg.ap.ssid_len = strlen(cfg.ap_ssid);
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.beacon_interval = 100;
        ap_cfg.ap.ssid_hidden = 0;
        ap_cfg.ap.authmode = string_has_value(cfg.ap_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ap_cfg.ap.pmf_cfg.required = false;
        ap_cfg.ap.pmf_cfg.capable = true;
        esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (err != ESP_OK) {
            state_lock();
            set_last_error_locked("设置 AP 失败: %s", esp_err_to_name(err));
            state_unlock();
        }
    }

    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        state_lock();
        set_last_error_locked("Wi-Fi 启动失败: %s", esp_err_to_name(start_err));
        g_app.wifi_restarting = false;
        state_unlock();
        vTaskDelete(NULL);
        return;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    state_lock();
    g_app.sta_connected = false;
    g_app.sta_connecting = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) && string_has_value(cfg.wifi_ssid);
    g_app.ap_running = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    g_app.ap_forced = force_ap || (!cfg.ap_enabled && !string_has_value(cfg.wifi_ssid));
    g_app.wifi_restarting = false;
    g_app.sta_ip[0] = '\0';
    if (!g_app.ap_running) {
        g_app.ap_ip[0] = '\0';
    }
    if (g_app.sta_connecting && string_has_value(cfg.wifi_ssid)) {
        set_status_line_locked("正在连接 Wi-Fi: %s", cfg.wifi_ssid);
    } else if (g_app.ap_running) {
        set_status_line_locked("热点已启动: %s", cfg.ap_ssid);
    } else {
        set_status_line_locked("Wi-Fi 已启动");
    }
    state_unlock();

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        esp_wifi_connect();
        xTaskCreate(
            wifi_watchdog_task,
            "wifi_watchdog",
            4096,
            NULL,
            4,
            NULL);
    }

    wifi_update_ip_strings();
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        state_lock();
        bool restarting = g_app.wifi_restarting;
        bool has_sta = string_has_value(g_app.config.wifi_ssid);
        state_unlock();
        if (!restarting && has_sta) {
            esp_wifi_connect();
            state_lock();
            g_app.sta_connecting = true;
            set_status_line_locked("正在连接 Wi-Fi...");
            state_unlock();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bool restarting = false;
        bool has_sta = false;
        state_lock();
        restarting = g_app.wifi_restarting;
        has_sta = string_has_value(g_app.config.wifi_ssid);
        g_app.sta_connected = false;
        g_app.sta_connecting = has_sta;
        g_app.sta_ip[0] = '\0';
        if (!restarting) {
            set_last_error_locked("Wi-Fi 已断开");
            set_status_line_locked("Wi-Fi 已断开，正在重连");
        }
        state_unlock();

        if (!restarting && has_sta) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        state_lock();
        g_app.ap_running = true;
        set_status_line_locked(g_app.sta_connected ? "AP+STA 已就绪" : "热点已启动");
        clear_last_error_locked();
        state_unlock();
        wifi_update_ip_strings();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        state_lock();
        g_app.ap_running = false;
        g_app.ap_ip[0] = '\0';
        state_unlock();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16] = {0};
        if (event) {
            esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        }
        state_lock();
        g_app.sta_connected = true;
        g_app.sta_connecting = false;
        copy_string(g_app.sta_ip, sizeof(g_app.sta_ip), ip_str);
        clear_last_error_locked();
        set_status_line_locked(g_app.ap_running ? "AP+STA 已就绪" : "Wi-Fi 已连接");
        state_unlock();
        wifi_update_ip_strings();
        start_sntp_if_needed();
    }
}

static esp_err_t wifi_init_and_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_app.sta_netif = esp_netif_create_default_wifi_sta();
    g_app.ap_netif = esp_netif_create_default_wifi_ap();
    if (!g_app.sta_netif || !g_app.ap_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    watch_ai_config_t cfg_snapshot;
    state_lock();
    cfg_snapshot = g_app.config;
    state_unlock();

    bool has_sta = string_has_value(cfg_snapshot.wifi_ssid);
    bool want_ap = cfg_snapshot.ap_enabled || !has_sta;
    wifi_mode_t mode = WIFI_MODE_STA;
    if (want_ap && has_sta) {
        mode = WIFI_MODE_APSTA;
    } else if (want_ap) {
        mode = WIFI_MODE_AP;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_config_t sta_cfg = {0};
        copy_string((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg_snapshot.wifi_ssid);
        copy_string((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg_snapshot.wifi_password);
        sta_cfg.sta.threshold.authmode = string_has_value(cfg_snapshot.wifi_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t ap_cfg = {0};
        copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), cfg_snapshot.ap_ssid);
        copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), cfg_snapshot.ap_password);
        ap_cfg.ap.ssid_len = strlen(cfg_snapshot.ap_ssid);
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.beacon_interval = 100;
        ap_cfg.ap.ssid_hidden = 0;
        ap_cfg.ap.authmode = string_has_value(cfg_snapshot.ap_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ap_cfg.ap.pmf_cfg.capable = true;
        ap_cfg.ap.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    state_lock();
    g_app.ap_running = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    g_app.ap_forced = !cfg_snapshot.ap_enabled && !has_sta;
    g_app.sta_connecting = has_sta && !g_app.ap_running;
    g_app.sta_connected = false;
    if (g_app.ap_running) {
        set_status_line_locked("热点已启动");
    } else if (g_app.sta_connecting) {
        set_status_line_locked("正在连接 Wi-Fi: %s", cfg_snapshot.wifi_ssid);
    } else {
        set_status_line_locked("网络已就绪");
    }
    state_unlock();

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        esp_wifi_connect();
    }
    wifi_update_ip_strings();
    return ESP_OK;
}

static const char INDEX_HTML[] = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>WatchAI 控制台</title>
<style>
:root {
  --bg0: #07111f;
  --bg1: #0f1d33;
  --panel: rgba(13, 23, 39, 0.82);
  --panel2: rgba(22, 34, 56, 0.88);
  --line: rgba(255,255,255,0.08);
  --text: #e8f1ff;
  --muted: #90a3c3;
  --accent: #45d6c2;
  --accent2: #ffb86c;
  --danger: #ff6f7d;
  --shadow: 0 24px 60px rgba(0,0,0,.34);
  --radius: 24px;
}
* { box-sizing: border-box; }
html, body { height: 100%; }
html {
  background: linear-gradient(180deg, var(--bg0), var(--bg1));
}
body {
  margin: 0;
  color: var(--text);
  font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  background:
    radial-gradient(1200px 700px at 15% 10%, rgba(69,214,194,.20), transparent 50%),
    radial-gradient(900px 600px at 90% 20%, rgba(255,184,108,.12), transparent 48%),
    radial-gradient(900px 800px at 50% 100%, rgba(108,139,255,.12), transparent 55%),
    linear-gradient(180deg, var(--bg0), var(--bg1));
  overflow-x: hidden;
}
body::before {
  content: "";
  position: fixed;
  inset: 0;
  pointer-events: none;
  background-image:
    linear-gradient(rgba(255,255,255,.02) 1px, transparent 1px),
    linear-gradient(90deg, rgba(255,255,255,.02) 1px, transparent 1px);
  background-size: 48px 48px;
  mask-image: radial-gradient(circle at center, black 30%, transparent 100%);
  opacity: .6;
}
.wrap {
  max-width: 1200px;
  margin: 0 auto;
  padding: 18px;
  padding-bottom: 42px;
}
.hero {
  display: grid;
  grid-template-columns: 1.18fr .82fr;
  gap: 16px;
  align-items: stretch;
}
.card {
  position: relative;
  overflow: hidden;
  border-radius: var(--radius);
  background: linear-gradient(180deg, var(--panel), rgba(8, 14, 26, .86));
  border: 1px solid var(--line);
  box-shadow: var(--shadow);
  backdrop-filter: blur(18px);
  animation: rise .55s ease both;
}
.card::after {
  content: "";
  position: absolute;
  inset: auto -20% -55% auto;
  width: 220px;
  height: 220px;
  border-radius: 50%;
  background: radial-gradient(circle, rgba(69,214,194,.18), transparent 68%);
  pointer-events: none;
}
.hero-main {
  padding: 22px;
  min-height: 220px;
}
.brandline {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  align-items: center;
  margin-bottom: 14px;
}
.pill {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  border-radius: 999px;
  border: 1px solid var(--line);
  background: rgba(255,255,255,.05);
  color: var(--muted);
  font-size: 12px;
  letter-spacing: .02em;
}
.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--accent);
  box-shadow: 0 0 0 6px rgba(69,214,194,.12);
}
h1 {
  margin: 0;
  font-size: clamp(30px, 5vw, 54px);
  line-height: 1;
  letter-spacing: -.05em;
}
.sub {
  margin: 14px 0 0;
  max-width: 58ch;
  color: var(--muted);
  line-height: 1.6;
}
.hero-meta {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
  margin-top: 20px;
}
.meta {
  padding: 14px;
  border-radius: 18px;
  background: rgba(255,255,255,.04);
  border: 1px solid var(--line);
}
.meta .k { color: var(--muted); font-size: 12px; }
.meta .v { margin-top: 8px; font-size: 16px; font-weight: 700; word-break: break-all; }
.hero-side {
  padding: 18px;
  min-height: 220px;
  display: flex;
  align-items: stretch;
}
.qr-shell {
  width: 100%;
  display: grid;
  grid-template-rows: auto 1fr auto;
  gap: 12px;
}
.qr-title {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  align-items: center;
}
.qr-box {
  display: grid;
  place-items: center;
  padding: 14px;
  border-radius: 22px;
  background: rgba(255,255,255,.05);
  border: 1px solid var(--line);
  min-height: 170px;
}
.qr-hint {
  color: var(--muted);
  font-size: 13px;
  line-height: 1.6;
}
.grid {
  margin-top: 16px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}
.section {
  padding: 18px;
}
.section h2 {
  margin: 0 0 12px;
  font-size: 18px;
  letter-spacing: -.02em;
}
.hint {
  margin: 0 0 16px;
  color: var(--muted);
  font-size: 13px;
  line-height: 1.55;
}
.form {
  display: grid;
  gap: 12px;
}
.field {
  display: grid;
  gap: 8px;
}
.field label {
  color: var(--muted);
  font-size: 12px;
  letter-spacing: .03em;
  text-transform: uppercase;
}
input, textarea, select {
  width: 100%;
  border: 1px solid var(--line);
  background: rgba(255,255,255,.04);
  color: var(--text);
  border-radius: 16px;
  padding: 14px 14px;
  outline: none;
  transition: border-color .15s ease, transform .15s ease, background .15s ease;
}
textarea {
  min-height: 132px;
  resize: vertical;
  line-height: 1.55;
}
input:focus, textarea:focus, select:focus {
  border-color: rgba(69,214,194,.45);
  background: rgba(255,255,255,.06);
}
.help {
  color: var(--muted);
  font-size: 12px;
  line-height: 1.45;
}
.row {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}
.buttons {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-top: 4px;
}
button {
  appearance: none;
  border: none;
  cursor: pointer;
  color: #08111f;
  font-weight: 800;
  letter-spacing: .01em;
  border-radius: 999px;
  padding: 13px 16px;
  background: linear-gradient(135deg, var(--accent), #7cf0dc);
  box-shadow: 0 14px 26px rgba(69,214,194,.18);
}
button.secondary {
  color: var(--text);
  background: rgba(255,255,255,.06);
  border: 1px solid var(--line);
  box-shadow: none;
}
button.warn {
  color: #1d1006;
  background: linear-gradient(135deg, var(--accent2), #ffd7a0);
}
button.danger {
  color: white;
  background: linear-gradient(135deg, #ff6f7d, #ff97a1);
}
button:disabled {
  cursor: not-allowed;
  opacity: .55;
}
.statusbar {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 8px;
}
.badge {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  border-radius: 999px;
  border: 1px solid var(--line);
  background: rgba(255,255,255,.05);
  color: var(--muted);
  font-size: 12px;
}
.badge b { color: var(--text); font-weight: 700; }
.chatlog {
  display: grid;
  gap: 12px;
  max-height: 580px;
  overflow: auto;
  padding-right: 2px;
}
.bubble {
  padding: 14px 14px 12px;
  border-radius: 18px;
  border: 1px solid var(--line);
  background: rgba(255,255,255,.04);
  white-space: pre-wrap;
  line-height: 1.6;
  overflow-wrap: anywhere;
}
.bubble.user {
  background: linear-gradient(180deg, rgba(69,214,194,.14), rgba(255,255,255,.04));
  border-color: rgba(69,214,194,.18);
}
.bubble.assistant {
  background: linear-gradient(180deg, rgba(255,255,255,.05), rgba(255,255,255,.02));
}
.bubble .role {
  display: block;
  margin-bottom: 8px;
  font-size: 12px;
  letter-spacing: .03em;
  color: var(--muted);
  text-transform: uppercase;
}
.toast {
  position: fixed;
  left: 50%;
  bottom: 18px;
  transform: translateX(-50%);
  background: rgba(10, 18, 31, .94);
  border: 1px solid var(--line);
  color: var(--text);
  padding: 12px 16px;
  border-radius: 999px;
  box-shadow: var(--shadow);
  opacity: 0;
  pointer-events: none;
  transition: opacity .18s ease, transform .18s ease;
  max-width: calc(100vw - 24px);
}
.toast.show {
  opacity: 1;
  transform: translateX(-50%) translateY(-4px);
}
.toast.ok { border-color: rgba(69,214,194,.32); }
.toast.err { border-color: rgba(255,111,125,.32); }
@keyframes rise {
  from { opacity: 0; transform: translateY(16px); }
  to { opacity: 1; transform: translateY(0); }
}
@media (max-width: 980px) {
  .hero, .grid { grid-template-columns: 1fr; }
  .hero-meta { grid-template-columns: 1fr; }
}
</style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <section class="card hero-main">
        <div class="brandline">
          <span class="pill"><span class="dot"></span><span id="chipMode">INIT</span></span>
          <span class="pill" id="chipTime">时间同步中</span>
          <span class="pill">WatchAI 控制台</span>
        </div>
        <h1>手机改 API，手表直接聊天。</h1>
        <p class="sub">
          这块表会同时提供一个配置热点和一个网页控制台。你可以在手机上随时换
          <b>API 地址</b>、<b>API Key</b>、<b>模型</b>，然后直接在页面里发起聊天。
        </p>
        <div class="hero-meta">
          <div class="meta">
            <div class="k">热点</div>
            <div class="v" id="metaAp">-</div>
          </div>
          <div class="meta">
            <div class="k">局域网 IP</div>
            <div class="v" id="metaIp">-</div>
          </div>
          <div class="meta">
            <div class="k">接口地址</div>
            <div class="v" id="metaApi">-</div>
          </div>
        </div>
      </section>
      <section class="card hero-side">
        <div class="qr-shell">
          <div class="qr-title">
            <div>
              <div style="font-size:14px;color:var(--muted);">扫一扫连接热点</div>
              <div style="font-size:16px;font-weight:700;" id="qrTitle">WIFI QR</div>
            </div>
            <div class="badge"><b id="badgeAp">AP</b><span id="badgeApState">ON</span></div>
          </div>
          <div class="qr-box" id="qrBox"></div>
          <div class="qr-hint" id="qrHint">手机先扫这个二维码连上热点，再打开页面继续配置。</div>
        </div>
      </section>
    </div>

    <div class="grid">
      <section class="card section">
        <h2>连接与配置</h2>
        <p class="hint">白色字段会直接保存，密钥字段留空表示保持现有值不变。</p>
        <div class="form">
          <div class="row">
            <div class="field">
              <label>热点名称</label>
              <input id="ap_ssid" placeholder="WatchAI-XXXX">
            </div>
            <div class="field">
              <label>热点密码</label>
              <input id="ap_password" type="password" placeholder="留空则保持已保存值">
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label>Wi-Fi 名称</label>
              <input id="wifi_ssid" placeholder="路由器 SSID">
            </div>
            <div class="field">
              <label>Wi-Fi 密码</label>
              <input id="wifi_password" type="password" placeholder="留空表示不改">
            </div>
          </div>
          <div class="field">
            <label>API 地址</label>
            <input id="api_url" placeholder="https://subrouter.ai/v1/chat/completions">
            <div class="help">支持任何 OpenAI 兼容接口。你也可以直接改成别的中转地址。</div>
          </div>
          <div class="row">
            <div class="field">
              <label>API Key</label>
              <input id="api_key" type="password" placeholder="留空则保持已保存值">
            </div>
            <div class="field">
              <label>模型</label>
              <input id="model" placeholder="gpt-5.5">
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label>TTS Key</label>
              <input id="tts_api_key" type="password" placeholder="留空则复用 API Key">
              <div class="help">微软 Azure Speech 建议单独填这里；如果不填，就继续使用上面的 API Key。</div>
            </div>
            <div class="field">
              <label>微软语音提示</label>
              <div class="help">Azure 地址示例：`https://eastus.tts.speech.microsoft.com/cognitiveservices/v1`。声音名可填 `zh-CN-XiaoxiaoNeural`。</div>
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label>超时(ms)</label>
              <input id="timeout_ms" type="number" min="5000" max="180000" step="1000">
            </div>
            <div class="field">
              <label>temperature</label>
              <input id="temperature" type="number" min="0" max="2" step="0.1">
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label>自动待机(秒)</label>
              <input id="auto_sleep_timeout_s" type="number" min="0" max="86400" step="1">
              <div class="help">0 表示关闭自动待机。建议 30-300 秒，省电和体验比较平衡。</div>
            </div>
            <div class="field">
              <label>省电建议</label>
              <div class="help">你可以按使用场景随时改这个值，手机网页和手表菜单会同步保存。</div>
            </div>
          </div>
          <div class="field">
            <label>自然人声</label>
            <label style="display:flex;align-items:center;gap:10px;padding:14px 14px;border:1px solid var(--line);background:rgba(255,255,255,.04);border-radius:16px;">
              <input id="tts_enabled" type="checkbox" style="width:auto;min-width:18px;height:18px;accent-color:var(--accent);">
              <span>启用手表朗读回复</span>
            </label>
            <div class="help">推荐开启。手机端可分别改聊天中转和 TTS 地址，手表会自动用更自然的人声朗读。</div>
          </div>
          <div class="row">
            <div class="field">
              <label>TTS URL</label>
              <input id="tts_url" placeholder="https://eastus.tts.speech.microsoft.com/cognitiveservices/v1">
              <div class="help">支持 OpenAI-compatible TTS 或 Azure Speech。OpenAI 兼容时可以留空自动推导；Azure Speech 需要手动填写完整地址。</div>
            </div>
            <div class="field">
              <label>TTS 模型</label>
              <input id="tts_model" placeholder="gpt-4o-mini-tts">
              <div class="help">OpenAI 兼容 TTS 才需要这个字段；Azure Speech 会忽略。</div>
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label>人声音色</label>
              <input id="tts_voice" list="ttsVoices" placeholder="alloy">
              <datalist id="ttsVoices">
                <option value="alloy"></option>
                <option value="nova"></option>
                <option value="echo"></option>
                <option value="fable"></option>
                <option value="shimmer"></option>
                <option value="onyx"></option>
                <option value="zh-CN-XiaoxiaoNeural"></option>
                <option value="zh-CN-YunxiNeural"></option>
                <option value="zh-CN-YunxiaNeural"></option>
                <option value="en-US-JennyNeural"></option>
              </datalist>
              <div class="help">Azure Speech 常用 `zh-CN-XiaoxiaoNeural`；OpenAI 兼容可继续用 `alloy / nova / echo` 这些音色。</div>
            </div>
            <div class="field">
              <label>返回格式</label>
              <select id="tts_response_format">
                <option value="wav">wav</option>
              </select>
              <div class="help">OpenAI 兼容 TTS 用这个；Azure Speech 会自动返回 WAV/PCM，手表可直接播放。</div>
            </div>
          </div>
          <div class="row">
            <div class="field">
              <label>朗读速度</label>
              <input id="tts_speed" type="number" min="0.5" max="2" step="0.1">
            </div>
            <div class="field">
              <label>推荐组合</label>
              <div class="help">OpenAI：`gpt-4o-mini-tts + alloy + wav`。Azure：`zh-CN-XiaoxiaoNeural + riff-24khz-16bit-mono-pcm`。</div>
            </div>
          </div>
          <div class="field">
            <label>系统提示词</label>
            <textarea id="system_prompt"></textarea>
          </div>
          <div class="buttons">
            <button id="btnSave">只保存</button>
            <button id="btnConnect" class="warn">保存并重连 Wi-Fi</button>
            <button id="btnToggleAp" class="secondary">热点开关</button>
          </div>
        </div>
      </section>

      <section class="card section">
        <h2>聊天窗口</h2>
        <p class="hint">发送一条消息就会走你填好的中转 API。回复会保存在手表里，页面刷新也还在。</p>
        <div class="form">
          <div class="field">
            <label>消息</label>
            <textarea id="chat_input" placeholder="在这里输入你要问 AI 的内容，支持多行。"></textarea>
          </div>
          <div class="buttons">
            <button id="btnSend">发送</button>
            <button id="btnReload" class="secondary">刷新状态</button>
            <button id="btnClear" class="danger">清空对话</button>
          </div>
          <div class="statusbar">
            <span class="badge">Wi-Fi <b id="chipWifi">-</b></span>
            <span class="badge">AP <b id="chipAp">-</b></span>
            <span class="badge">聊天 <b id="chipBusy">-</b></span>
            <span class="badge">TTS <b id="chipTts">-</b></span>
            <span class="badge">音色 <b id="chipVoice">-</b></span>
            <span class="badge">朗读 <b id="chipTtsBusy">-</b></span>
            <span class="badge">时间 <b id="chipSync">-</b></span>
          </div>
          <div class="chatlog" id="chatLog"></div>
          <div class="help" id="chatHelp">准备就绪。</div>
        </div>
      </section>
    </div>
  </div>

  <div class="toast" id="toast"></div>

<script>
const state = {
  status: null,
  config: null,
  history: []
};

const $ = (id) => document.getElementById(id);
const toast = (msg, ok=true) => {
  const el = $('toast');
  el.textContent = msg;
  el.className = 'toast show ' + (ok ? 'ok' : 'err');
  clearTimeout(el._timer);
  el._timer = setTimeout(() => el.className = 'toast', 2400);
};

async function api(path, options = {}) {
  const res = await fetch(path, {
    headers: options.body ? {'Content-Type': 'application/json'} : undefined,
    ...options,
  });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch (e) {}
  if (!res.ok) {
    const msg = json && (json.error || json.message) ? (json.error || json.message) : text || ('HTTP ' + res.status);
    throw new Error(msg);
  }
  return json;
}

function shorten(text, n = 72) {
  if (!text) return '-';
  return text.length > n ? text.slice(0, n - 1) + '…' : text;
}

function isAzureTtsUrl(url) {
  if (!url) return false;
  const lower = String(url).toLowerCase();
  return lower.includes('speech.microsoft.com') ||
    lower.includes('cognitive.microsoft.com') ||
    lower.includes('cognitiveservices.azure.com');
}

function renderConfig(cfg) {
  if (!cfg) return;
  $('ap_ssid').value = cfg.ap_ssid || '';
  $('wifi_ssid').value = cfg.wifi_ssid || '';
  $('api_url').value = cfg.api_url || '';
  $('model').value = cfg.model || '';
  $('timeout_ms').value = cfg.timeout_ms ?? 60000;
  $('temperature').value = cfg.temperature ?? 0.7;
  $('auto_sleep_timeout_s').value = cfg.auto_sleep_timeout_s ?? 60;
  $('tts_enabled').checked = !!cfg.tts_enabled;
  $('tts_url').value = cfg.tts_url || '';
  $('tts_api_key').value = '';
  $('tts_model').value = cfg.tts_model || '';
  $('tts_voice').value = cfg.tts_voice || '';
  $('tts_response_format').value = cfg.tts_response_format || 'wav';
  $('tts_speed').value = cfg.tts_speed ?? 1.0;
  $('system_prompt').value = cfg.system_prompt || '';
  $('ap_password').value = '';
  $('wifi_password').value = '';
  $('api_key').value = '';
  $('qrTitle').textContent = cfg.ap_ssid || 'WIFI QR';
  $('badgeApState').textContent = cfg.ap_enabled ? 'ON' : 'OFF';
  $('btnToggleAp').textContent = cfg.ap_enabled ? '关闭热点' : '开启热点';
}

function renderStatus(st) {
  if (!st) return;
  const azureTts = isAzureTtsUrl(st.tts_url || '');
  $('chipMode').textContent = (st.mode || 'offline').toUpperCase();
  $('chipTime').textContent = st.time_synced ? '时间已同步' : '时间未同步';
  $('chipWifi').textContent = st.sta_connected ? (st.sta_ip || '已连接') : (st.sta_connecting ? '连接中' : '未连接');
  $('chipAp').textContent = st.ap_running ? (st.ap_ip || 'ON') : 'OFF';
  $('chipBusy').textContent = st.chat_busy ? '忙碌' : '空闲';
  $('chipTts').textContent = st.tts_enabled ? (azureTts ? 'AZURE' : 'OPENAI') : 'OFF';
  $('chipVoice').textContent = shorten(st.tts_voice || '-', 18);
  $('chipTtsBusy').textContent = st.tts_busy ? '朗读中' : '空闲';
  $('chipSync').textContent = st.time_synced ? 'OK' : 'WAIT';
  $('metaAp').textContent = st.ap_ssid || '-';
  $('metaIp').textContent = st.ap_running ? (st.ap_ip || '-') : (st.sta_ip || '-');
  $('metaApi').textContent = shorten(st.api_url || '-', 38);
  $('badgeAp').textContent = st.ap_running ? 'AP' : 'AP';
  $('badgeApState').textContent = st.ap_running ? (st.ap_forced ? 'FORCED' : 'ON') : 'OFF';
  $('chatHelp').textContent = st.last_error ? ('最后错误: ' + st.last_error) : (st.status_line || '准备就绪。');
  $('btnToggleAp').textContent = st.ap_enabled ? '关闭热点' : '开启热点';
  $('btnSend').disabled = !!st.chat_busy;
}

function renderHistory(history) {
  const box = $('chatLog');
  box.innerHTML = '';
  if (!history || !history.length) {
    const empty = document.createElement('div');
    empty.className = 'bubble assistant';
    empty.innerHTML = '<span class="role">assistant</span>还没有对话，先发一条试试。';
    box.appendChild(empty);
    return;
  }
  history.forEach((msg) => {
    const bubble = document.createElement('div');
    bubble.className = 'bubble ' + (msg.role || 'assistant');
    const role = document.createElement('span');
    role.className = 'role';
    role.textContent = msg.role || 'assistant';
    const body = document.createElement('div');
    body.textContent = msg.content || '';
    bubble.appendChild(role);
    bubble.appendChild(body);
    box.appendChild(bubble);
  });
  box.scrollTop = box.scrollHeight;
}

function wifiQrText(cfg, st) {
  if (!cfg || !st || !st.ap_running) return '';
  const ssid = cfg.ap_ssid || '';
  const pass = cfg.has_ap_password ? (cfg.ap_password_preview || '') : '';
  return `WIFI:T:WPA;S:${ssid.replace(/([\\;,:])/g, '\\$1')};P:${pass.replace(/([\\;,:])/g, '\\$1')};H:false;;`;
}

async function refreshAll() {
  try {
    const [status, config, history] = await Promise.all([
      api('/api/status'),
      api('/api/config'),
      api('/api/history'),
    ]);
    state.status = status;
    state.config = config;
    state.history = history.messages || [];
    renderConfig(config);
    renderStatus(status);
    renderHistory(state.history);
    $('api_url').value = config.api_url || $('api_url').value;
    $('qrHint').textContent = status.ap_running
      ? '手机连上热点后，在浏览器打开 192.168.4.1 就能看到这个页面。'
      : '热点当前关闭，如果你还连着 STA，也可以从局域网 IP 打开。';
    if (status.ap_running) {
      const payload = wifiQrText(config, status);
      $('qrBox').innerHTML = '';
      if (payload) {
        const qr = document.createElement('canvas');
        qr.width = 160;
        qr.height = 160;
        $('qrBox').appendChild(qr);
        try {
          const qrObj = await api('/api/qr?mode=wifi');
        } catch (e) {
          // fallback: just show text if QR endpoint is absent
          $('qrBox').innerHTML = '<div style="padding:12px;color:var(--muted);text-align:center;">热点二维码在手表屏幕上显示，这里只保留控制台。</div>';
        }
      }
    }
  } catch (e) {
    console.warn(e);
  }
}

async function saveConfig(connectNow) {
  const payload = {
    ap_enabled: state.config ? !!state.config.ap_enabled : true,
    ap_ssid: $('ap_ssid').value.trim(),
    wifi_ssid: $('wifi_ssid').value.trim(),
    api_url: $('api_url').value.trim(),
    model: $('model').value.trim(),
    system_prompt: $('system_prompt').value,
    timeout_ms: parseInt($('timeout_ms').value || '60000', 10),
    temperature: parseFloat($('temperature').value || '0.7'),
    auto_sleep_timeout_s: parseInt($('auto_sleep_timeout_s').value || '60', 10),
    tts_enabled: $('tts_enabled').checked,
    tts_url: $('tts_url').value.trim(),
    tts_api_key: $('tts_api_key').value.trim(),
    tts_model: $('tts_model').value.trim(),
    tts_voice: $('tts_voice').value.trim(),
    tts_response_format: $('tts_response_format').value.trim(),
    tts_speed: parseFloat($('tts_speed').value || '1.0'),
    max_tokens: state.config ? (state.config.max_tokens || 512) : 512,
    connect_now: !!connectNow
  };
  const apPassword = $('ap_password').value.trim();
  const wifiPassword = $('wifi_password').value.trim();
  const apiKey = $('api_key').value.trim();
  const ttsApiKey = $('tts_api_key').value.trim();
  if (apPassword) payload.ap_password = apPassword;
  if (wifiPassword) payload.wifi_password = wifiPassword;
  if (apiKey) payload.api_key = apiKey;
  if (ttsApiKey) payload.tts_api_key = ttsApiKey;
  const res = await api('/api/config', { method: 'POST', body: JSON.stringify(payload) });
  toast(res.message || '已保存');
  await refreshAll();
}

async function toggleAp() {
  const enable = !(state.config && state.config.ap_enabled);
  const res = await api(enable ? '/api/ap/start' : '/api/ap/stop', { method: 'POST' });
  toast(res.message || '已切换热点');
  await refreshAll();
}

async function sendChat() {
  const text = $('chat_input').value.trim();
  if (!text) {
    toast('先输入一条消息', false);
    return;
  }
  $('btnSend').disabled = true;
  try {
    const res = await api('/api/chat', {
      method: 'POST',
      body: JSON.stringify({ message: text }),
    });
    $('chat_input').value = '';
    toast('收到回复');
    await refreshAll();
    if (res.reply) {
      $('chatHelp').textContent = '最后回复已更新。';
    }
  } catch (e) {
    toast(e.message || '发送失败', false);
    $('chatHelp').textContent = e.message || '发送失败';
  } finally {
    $('btnSend').disabled = state.status && state.status.chat_busy;
  }
}

async function clearChat() {
  const res = await api('/api/clear', { method: 'POST' });
  toast(res.message || '已清空');
  await refreshAll();
}

function bindEvents() {
  $('btnSave').addEventListener('click', () => saveConfig(false).catch(e => toast(e.message, false)));
  $('btnConnect').addEventListener('click', () => saveConfig(true).catch(e => toast(e.message, false)));
  $('btnToggleAp').addEventListener('click', () => toggleAp().catch(e => toast(e.message, false)));
  $('btnSend').addEventListener('click', () => sendChat().catch(e => toast(e.message, false)));
  $('btnReload').addEventListener('click', () => refreshAll().catch(e => toast(e.message, false)));
  $('btnClear').addEventListener('click', () => clearChat().catch(e => toast(e.message, false)));
  $('chat_input').addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter' && !ev.shiftKey) {
      ev.preventDefault();
      sendChat().catch(e => toast(e.message, false));
    }
  });
}

async function pollStatus() {
  try {
    const st = await api('/api/status');
    state.status = st;
    renderStatus(st);
  } catch (e) {
    // ignore transient disconnects
  }
}

bindEvents();
refreshAll();
setInterval(pollStatus, 2500);
</script>
</body>
</html>
)HTML";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    json_status_snapshot(root);
    esp_err_t err = send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return err;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    json_config_snapshot(root);
    esp_err_t err = send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return err;
}

static esp_err_t history_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    json_history_snapshot(root);
    esp_err_t err = send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return err;
}

static esp_err_t parse_and_commit_config(cJSON *root, bool connect_now, char *error_out, size_t error_out_len)
{
    watch_ai_config_t new_cfg;
    state_lock();
    new_cfg = g_app.config;
    state_unlock();

    char old_api_url[sizeof(new_cfg.api_url)] = {0};
    char old_tts_url[sizeof(new_cfg.tts_url)] = {0};
    copy_string(old_api_url, sizeof(old_api_url), new_cfg.api_url);
    copy_string(old_tts_url, sizeof(old_tts_url), new_cfg.tts_url);

    bool restart_wifi = false;
    bool need_restart_from_connect = connect_now;

    bool ap_enabled = new_cfg.ap_enabled;
    if (parse_bool_field(root, "ap_enabled", &ap_enabled)) {
        if (ap_enabled != new_cfg.ap_enabled) {
            new_cfg.ap_enabled = ap_enabled;
            restart_wifi = true;
        }
    }

    if (parse_string_field(root, "ap_ssid", new_cfg.ap_ssid, sizeof(new_cfg.ap_ssid))) {
        restart_wifi = true;
    }
    if (parse_secret_field(root, "ap_password", new_cfg.ap_password, sizeof(new_cfg.ap_password))) {
        restart_wifi = true;
    }
    if (parse_string_field(root, "wifi_ssid", new_cfg.wifi_ssid, sizeof(new_cfg.wifi_ssid))) {
        restart_wifi = true;
    }
    if (parse_secret_field(root, "wifi_password", new_cfg.wifi_password, sizeof(new_cfg.wifi_password))) {
        restart_wifi = true;
    }
    if (parse_string_field(root, "api_url", new_cfg.api_url, sizeof(new_cfg.api_url))) {
        // no wifi restart required
    }
    if (parse_secret_field(root, "api_key", new_cfg.api_key, sizeof(new_cfg.api_key))) {
        // no wifi restart required
    }
    if (parse_secret_field(root, "tts_api_key", new_cfg.tts_api_key, sizeof(new_cfg.tts_api_key))) {
        // no wifi restart required
    }
    if (parse_string_field(root, "model", new_cfg.model, sizeof(new_cfg.model))) {
        // no wifi restart required
    }
    if (parse_string_field(root, "system_prompt", new_cfg.system_prompt, sizeof(new_cfg.system_prompt))) {
        // no wifi restart required
    }
    if (parse_int_field(root, "timeout_ms", &new_cfg.timeout_ms)) {
        // no wifi restart required
    }
    if (parse_int_field(root, "max_tokens", &new_cfg.max_tokens)) {
        // no wifi restart required
    }
    if (parse_float_field(root, "temperature", &new_cfg.temperature)) {
        // no wifi restart required
    }
    if (parse_bool_field(root, "tts_enabled", &new_cfg.tts_enabled)) {
        // no wifi restart required
    }
    if (parse_string_field(root, "tts_url", new_cfg.tts_url, sizeof(new_cfg.tts_url))) {
        // no wifi restart required
    }
    if (parse_string_field(root, "tts_model", new_cfg.tts_model, sizeof(new_cfg.tts_model))) {
        // no wifi restart required
    }
    if (parse_string_field(root, "tts_voice", new_cfg.tts_voice, sizeof(new_cfg.tts_voice))) {
        // no wifi restart required
    }
    if (parse_string_field(root, "tts_response_format", new_cfg.tts_response_format, sizeof(new_cfg.tts_response_format))) {
        // no wifi restart required
    }
    if (parse_float_field(root, "tts_speed", &new_cfg.tts_speed)) {
        // no wifi restart required
    }
    if (parse_int_field(root, "auto_sleep_timeout_s", &new_cfg.auto_sleep_timeout_s)) {
        // no wifi restart required
    }

    char old_derived_tts_url[sizeof(new_cfg.tts_url)] = {0};
    char new_derived_tts_url[sizeof(new_cfg.tts_url)] = {0};
    derive_tts_url_from_api_url(old_api_url, old_derived_tts_url, sizeof(old_derived_tts_url));
    derive_tts_url_from_api_url(new_cfg.api_url, new_derived_tts_url, sizeof(new_derived_tts_url));
    if (string_equal(new_cfg.tts_url, old_tts_url) && string_equal(old_tts_url, old_derived_tts_url)) {
        copy_string(new_cfg.tts_url, sizeof(new_cfg.tts_url), new_derived_tts_url);
    }
    if (!string_has_value(new_cfg.tts_url)) {
        copy_string(new_cfg.tts_url, sizeof(new_cfg.tts_url), new_derived_tts_url);
    }

    bool old_tts_azure = tts_uses_azure_speech_endpoint(old_tts_url);
    bool new_tts_azure = tts_uses_azure_speech_endpoint(new_cfg.tts_url);
    if (new_tts_azure && (!string_has_value(new_cfg.tts_voice) || tts_voice_is_openai_default(new_cfg.tts_voice))) {
        copy_string(new_cfg.tts_voice, sizeof(new_cfg.tts_voice), TTS_AZURE_DEFAULT_VOICE);
    } else if (!new_tts_azure && old_tts_azure && string_equal(new_cfg.tts_voice, TTS_AZURE_DEFAULT_VOICE)) {
        copy_string(new_cfg.tts_voice, sizeof(new_cfg.tts_voice), TTS_DEFAULT_VOICE);
    }

    cJSON *connect_now_item = cJSON_GetObjectItemCaseSensitive(root, "connect_now");
    if (cJSON_IsBool(connect_now_item)) {
        need_restart_from_connect = cJSON_IsTrue(connect_now_item);
    }

    config_prepare_runtime_defaults(&new_cfg);
    esp_err_t err = app_commit_config(&new_cfg, restart_wifi || need_restart_from_connect);
    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            state_lock();
            copy_string(error_out, error_out_len, g_app.last_error);
            state_unlock();
        }
        return err;
    }
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "请求体读取失败或过大");
        send_json_response(req, root, 400, "400 Bad Request");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "JSON 解析失败");
        send_json_response(req, root, 400, "400 Bad Request");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    char error_text[MAX_ERROR_LEN] = {0};
    err = parse_and_commit_config(json, false, error_text, sizeof(error_text));
    cJSON_Delete(json);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", error_text[0] ? error_text : "保存失败");
        send_json_response(req, root, 400, "400 Bad Request");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "配置已保存");
    json_status_snapshot(root);
    send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t wifi_start_post_handler(httpd_req_t *req)
{
    watch_ai_config_t new_cfg;
    state_lock();
    new_cfg = g_app.config;
    new_cfg.ap_enabled = true;
    state_unlock();
    config_prepare_runtime_defaults(&new_cfg);
    esp_err_t err = app_commit_config(&new_cfg, true);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "开启热点失败");
        send_json_response(req, root, 500, "500 Internal Server Error");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "热点已开启");
    json_status_snapshot(root);
    send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t wifi_stop_post_handler(httpd_req_t *req)
{
    watch_ai_config_t new_cfg;
    state_lock();
    new_cfg = g_app.config;
    new_cfg.ap_enabled = false;
    state_unlock();
    config_prepare_runtime_defaults(&new_cfg);
    esp_err_t err = app_commit_config(&new_cfg, true);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "关闭热点失败");
        send_json_response(req, root, 500, "500 Internal Server Error");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "热点已关闭");
    json_status_snapshot(root);
    send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t clear_post_handler(httpd_req_t *req)
{
    history_clear();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "对话已清空");
    send_json_response(req, root, 200, "200 OK");
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t chat_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_request_body(req, &body, &body_len);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "请求体读取失败");
        send_json_response(req, root, 400, "400 Bad Request");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "JSON 解析失败");
        send_json_response(req, root, 400, "400 Bad Request");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(json, "message");
    if (!cJSON_IsString(msg) || !msg->valuestring || msg->valuestring[0] == '\0') {
        cJSON_Delete(json);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "消息不能为空");
        send_json_response(req, root, 400, "400 Bad Request");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    watch_ai_config_t cfg;
    char user_message[MAX_MESSAGE_LEN] = {0};
    state_lock();
    cfg = g_app.config;
    state_unlock();
    copy_string(user_message, sizeof(user_message), msg->valuestring);
    cJSON_Delete(json);

    char reply[MAX_REPLY_LEN] = {0};
    char error_text[MAX_ERROR_LEN] = {0};
    err = run_chat_turn(&cfg, user_message, reply, sizeof(reply), error_text, sizeof(error_text));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(root, "reply", reply);
        cJSON_AddStringToObject(root, "message", "收到回复");
    } else {
        cJSON_AddStringToObject(root, "error", error_text[0] ? error_text : "聊天失败");
    }
    json_status_snapshot(root);
    int status_code = 500;
    const char *status_text = "500 Internal Server Error";
    if (err == ESP_OK) {
        status_code = 200;
        status_text = "200 OK";
    } else if (err == ESP_ERR_INVALID_STATE) {
        status_code = 429;
        status_text = "429 Too Many Requests";
    } else if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        status_code = 400;
        status_text = "400 Bad Request";
    }
    send_json_response(req, root, status_code, status_text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t uri_not_found_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", "未找到接口");
    send_json_response(req, root, 404, "404 Not Found");
    cJSON_Delete(root);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 12288;
    config.max_uri_handlers = 16;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return NULL;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t config_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t history_uri = {
        .uri = "/api/history",
        .method = HTTP_GET,
        .handler = history_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t chat_uri = {
        .uri = "/api/chat",
        .method = HTTP_POST,
        .handler = chat_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t clear_uri = {
        .uri = "/api/clear",
        .method = HTTP_POST,
        .handler = clear_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wifi_start_uri = {
        .uri = "/api/ap/start",
        .method = HTTP_POST,
        .handler = wifi_start_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wifi_stop_uri = {
        .uri = "/api/ap/stop",
        .method = HTTP_POST,
        .handler = wifi_stop_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t not_found_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = uri_not_found_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &config_uri);
    httpd_register_uri_handler(server, &config_post_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &chat_uri);
    httpd_register_uri_handler(server, &clear_uri);
    httpd_register_uri_handler(server, &wifi_start_uri);
    httpd_register_uri_handler(server, &wifi_stop_uri);
    httpd_register_uri_handler(server, &not_found_uri);
    return server;
}

static void ui_action_handler(lv_event_t *e)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(e);
    switch ((ui_action_t)action) {
    case UI_ACTION_TOGGLE_AP: {
        watch_ai_config_t cfg;
        state_lock();
        cfg = g_app.config;
        state_unlock();
        cfg.ap_enabled = !cfg.ap_enabled;
        config_prepare_runtime_defaults(&cfg);
        (void)app_commit_config(&cfg, true);
        break;
    }
    case UI_ACTION_RESTART_WIFI:
        schedule_wifi_restart(false);
        break;
    case UI_ACTION_OPEN_SETTINGS_MENU:
        ui_show_settings_menu();
        break;
    case UI_ACTION_OPEN_WIFI_SETTINGS:
        ui_show_wifi_settings();
        break;
    case UI_ACTION_OPEN_SLEEP_SETTINGS:
        ui_show_sleep_settings();
        break;
    case UI_ACTION_TOGGLE_SLEEP: {
        bool sleeping;
        state_lock();
        sleeping = !g_app.screen_sleeping;
        state_unlock();
        ui_set_screen_sleep(sleeping, sleeping ? "屏幕已待机，触摸任意位置唤醒" : "屏幕已唤醒");
        break;
    }
    case UI_ACTION_WIFI_SAVE:
        ui_settings_apply_wifi(false);
        break;
    case UI_ACTION_WIFI_FORGET:
        ui_settings_apply_wifi(true);
        break;
    case UI_ACTION_WIFI_BACK:
        ui_show_settings_menu();
        break;
    case UI_ACTION_SLEEP_SAVE:
        ui_sleep_apply();
        break;
    case UI_ACTION_SLEEP_BACK:
        ui_show_settings_menu();
        break;
    case UI_ACTION_START_VOICE_CHAT:
        ui_start_voice_chat();
        break;
    case UI_ACTION_OPEN_CHAT:
        ui_show_chat_screen();
        break;
    case UI_ACTION_CHAT_SEND:
        ui_chat_submit_text(g_app.chat_input_ta ? lv_textarea_get_text(g_app.chat_input_ta) : "");
        break;
    case UI_ACTION_CHAT_CLEAR:
        ui_chat_clear_history();
        break;
    case UI_ACTION_CHAT_BACK:
        ui_show_main_screen();
        break;
    default:
        break;
    }
}

static void ui_refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!g_app.screen) {
        return;
    }

    watch_ai_config_t cfg;
    bool chat_busy = false;
    bool tts_busy = false;
    bool voice_busy = false;
    bool crypto_busy = false;
    bool chart_dirty = false;
    bool chart_applied = false;
    char crypto_status[96] = "";
    char crypto_summary[320] = "";
    char crypto_updated[64] = "";
    char voice_desc[192] = "";
    int32_t chart_values[BTC_CHART_POINT_COUNT] = {0};
    size_t chart_point_count = 0;
    int32_t chart_min_value = 0;
    int32_t chart_max_value = 0;
    uint32_t chart_generation = 0;

    state_lock();
    cfg = g_app.config;
    chat_busy = g_app.chat_busy;
    tts_busy = g_app.tts_busy;
    voice_busy = g_app.voice_busy;
    crypto_busy = g_app.crypto_busy;
    copy_string(crypto_status, sizeof(crypto_status), g_app.crypto_status);
    copy_string(crypto_summary, sizeof(crypto_summary), g_app.crypto_summary);
    copy_string(crypto_updated, sizeof(crypto_updated), g_app.crypto_updated);
    chart_dirty = g_app.crypto_chart_dirty;
    chart_generation = g_app.crypto_chart_generation;
    if (chart_dirty) {
        chart_point_count = MIN(g_app.crypto_chart_point_count, (size_t)BTC_CHART_POINT_COUNT);
        if (chart_point_count > 0) {
            memcpy(chart_values, g_app.crypto_chart_values, sizeof(chart_values));
            chart_min_value = g_app.crypto_chart_min_value;
            chart_max_value = g_app.crypto_chart_max_value;
        }
    }
    state_unlock();

    if (crypto_busy) {
        snprintf(crypto_status, sizeof(crypto_status), "CoinGecko · 刷新中");
    } else if (!crypto_status[0]) {
        snprintf(crypto_status, sizeof(crypto_status), "CoinGecko · 等待刷新");
    }
    if (!crypto_summary[0]) {
        snprintf(crypto_summary, sizeof(crypto_summary), "BTC / ETH / SOL 行情稍后显示。");
    }
    if (!crypto_updated[0]) {
        snprintf(crypto_updated, sizeof(crypto_updated), crypto_busy ? "正在刷新..." : " ");
    }

    if (voice_busy) {
        snprintf(voice_desc, sizeof(voice_desc), "正在录音，请直接说话。");
    } else if (chat_busy) {
        snprintf(voice_desc, sizeof(voice_desc), "AI 正在思考中。");
    } else if (tts_busy) {
        snprintf(voice_desc, sizeof(voice_desc), "AI 已回复，正在朗读自然人声。");
    } else if (cfg.tts_enabled) {
        snprintf(voice_desc, sizeof(voice_desc), "点一下开始语音，全屏聊天。自然人声: %.24s", cfg.tts_voice);
    } else {
        snprintf(voice_desc, sizeof(voice_desc), "点一下开始语音，全屏聊天。语音朗读当前关闭。");
    }

    if (bsp_display_lock(3000)) {
        if (g_app.crypto_status_label) {
            lv_label_set_text(g_app.crypto_status_label, crypto_status);
        }
        if (g_app.crypto_summary_label) {
            lv_label_set_text(g_app.crypto_summary_label, crypto_summary);
        }
        if (g_app.crypto_updated_label) {
            lv_label_set_text(g_app.crypto_updated_label, crypto_updated);
        }
        if (chart_dirty && g_app.crypto_chart && g_app.crypto_chart_series && chart_point_count > 0) {
            lv_chart_set_axis_range(g_app.crypto_chart, LV_CHART_AXIS_PRIMARY_Y, chart_min_value, chart_max_value);
            lv_chart_set_series_values(g_app.crypto_chart, g_app.crypto_chart_series, chart_values, chart_point_count);
            lv_chart_refresh(g_app.crypto_chart);
            chart_applied = true;
        }
        if (g_app.voice_desc_label) {
            lv_label_set_text(g_app.voice_desc_label, voice_desc);
        }
        bsp_display_unlock();
    }
    if (chart_applied) {
        state_lock();
        g_app.crypto_chart_applied_generation = chart_generation;
        g_app.crypto_chart_dirty = (g_app.crypto_chart_generation != g_app.crypto_chart_applied_generation);
        state_unlock();
    }
    if (g_app.settings_menu_screen) {
        ui_settings_menu_refresh_form();
    }
    if (g_app.sleep_settings_screen) {
        ui_sleep_settings_refresh_form();
    }

    if (g_app.chat_screen) {
        ui_chat_refresh_form();
    }

    ui_refresh_home_status_bar();

    state_lock();
    bool should_sleep = !g_app.screen_sleeping && !chat_busy &&
                        !tts_busy &&
                        !voice_busy &&
                        !crypto_busy &&
                        cfg.auto_sleep_timeout_s > 0 &&
                        g_app.last_activity_us > 0 &&
                        (esp_timer_get_time() - g_app.last_activity_us) >= ((int64_t)cfg.auto_sleep_timeout_s * 1000000LL);
    state_unlock();
    if (should_sleep) {
        ui_set_screen_sleep(true, "屏幕已待机，触摸任意位置唤醒");
    }
}

static void format_local_time_string(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, buf_len, "%H:%M", &tm_now);
}

static void ui_refresh_home_status_bar(void)
{
    if (!g_app.screen) {
        return;
    }

    char wifi_text[48];
    char time_text[16];
    char battery_text[32];
    bool sta_connected = false;
    bool sta_connecting = false;
    bool ap_running = false;
    bool time_synced = false;
    bool screen_sleeping = false;

    state_lock();
    sta_connected = g_app.sta_connected;
    sta_connecting = g_app.sta_connecting;
    ap_running = g_app.ap_running;
    time_synced = g_app.time_synced;
    screen_sleeping = g_app.screen_sleeping;
    state_unlock();

    if (sta_connected) {
        snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi");
    } else if (sta_connecting) {
        snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi...");
    } else if (ap_running) {
        snprintf(wifi_text, sizeof(wifi_text), "AP");
    } else {
        snprintf(wifi_text, sizeof(wifi_text), "离线");
    }

    format_local_time_string(time_text, sizeof(time_text));
    if (!time_synced) {
        snprintf(time_text, sizeof(time_text), "--:--");
    }

    snprintf(battery_text, sizeof(battery_text), "88%%");
    if (!screen_sleeping) {
        // Keep the bar compact and phone-like; battery hardware data is not wired in yet.
    }

    if (bsp_display_lock(3000)) {
        if (g_app.home_wifi_label) {
            lv_label_set_text(g_app.home_wifi_label, wifi_text);
        }
        if (g_app.home_time_label) {
            lv_label_set_text(g_app.home_time_label, time_text);
        }
        if (g_app.home_battery_label) {
            lv_label_set_text(g_app.home_battery_label, battery_text);
        }
        bsp_display_unlock();
    }
}

static void ui_refresh_sleep_button(void)
{
    if (!g_app.settings_menu_sleep_label && !g_app.settings_menu_sleep_btn) {
        return;
    }
    if (bsp_display_lock(200)) {
        if (g_app.settings_menu_sleep_label) {
            lv_label_set_text(g_app.settings_menu_sleep_label, g_app.screen_sleeping ? "唤醒" : "关屏");
        }
        if (g_app.settings_menu_sleep_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_sleep_btn,
                                      g_app.screen_sleeping ? lv_color_hex(0x45D6C2) : lv_color_hex(0x56657A),
                                      0);
        }
        bsp_display_unlock();
    }
}

static void ui_set_screen_sleep(bool sleeping, const char *reason)
{
    bool changed = false;

    state_lock();
    changed = g_app.screen_sleeping != sleeping;
    g_app.screen_sleeping = sleeping;
    g_app.last_activity_us = esp_timer_get_time();
    if (reason && reason[0]) {
        set_status_line_locked("%s", reason);
    }
    state_unlock();

    if (!g_app.display || (!changed && !reason)) {
        return;
    }

    if (sleeping) {
        if (ui_enter_light_sleep() != ESP_OK) {
            (void)bsp_display_backlight_off();
        } else {
            state_lock();
            if (g_app.screen_sleeping) {
                g_app.screen_sleeping = false;
                set_status_line_locked("屏幕已唤醒");
            }
            state_unlock();
        }
    } else {
        (void)bsp_display_backlight_on();
    }
    ui_refresh_sleep_button();
}

static esp_err_t ui_enter_light_sleep(void)
{
    const gpio_num_t touch_int_gpio = BSP_LCD_TOUCH_INT;
    if (touch_int_gpio == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Touch interrupt GPIO is not configured");
        return ESP_FAIL;
    }

    (void)bsp_display_backlight_off();

    esp_err_t err = esp_sleep_enable_ext0_wakeup(touch_int_gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable touch wake: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Entering light sleep, wake on touch interrupt GPIO %d", touch_int_gpio);
    err = esp_light_sleep_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Light sleep failed: %s", esp_err_to_name(err));
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        return err;
    }

    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    (void)bsp_display_backlight_on();
    ESP_LOGI(TAG, "Woke from light sleep");
    return ESP_OK;
}

static void ui_mark_activity(void)
{
    bool wake_display = false;

    state_lock();
    g_app.last_activity_us = esp_timer_get_time();
    wake_display = g_app.screen_sleeping;
    if (wake_display) {
        g_app.screen_sleeping = false;
        set_status_line_locked("屏幕已唤醒");
    }
    state_unlock();

    if (wake_display) {
        (void)bsp_display_backlight_on();
    }
}

static void ui_show_main_screen(void)
{
    if (!g_app.screen || !g_app.display) {
        return;
    }

    if (bsp_display_lock(2000)) {
        lv_screen_load(g_app.screen);
        if (g_app.settings_menu_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_menu_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_menu_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        bsp_display_unlock();
    }
    ui_mark_activity();
    ui_refresh_timer(NULL);
}

static void ui_show_settings_menu(void)
{
    if (!g_app.display) {
        return;
    }
    if (!g_app.settings_menu_screen) {
        ui_create_settings_menu_screen();
    }
    if (!g_app.settings_menu_screen) {
        return;
    }

    if (bsp_display_lock(2000)) {
        lv_screen_load(g_app.settings_menu_screen);
        if (g_app.settings_menu_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_menu_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_menu_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        bsp_display_unlock();
    }
    ui_settings_menu_refresh_form();
    ui_mark_activity();
}

static void ui_settings_refresh_form(void)
{
    watch_ai_config_t cfg;
    state_lock();
    cfg = g_app.config;
    state_unlock();

    if (g_app.settings_wifi_ssid_ta) {
        lv_textarea_set_text(g_app.settings_wifi_ssid_ta, cfg.wifi_ssid);
    }
    if (g_app.settings_wifi_password_ta) {
        lv_textarea_set_text(g_app.settings_wifi_password_ta, "");
    }
    if (g_app.settings_status_label) {
        if (string_has_value(cfg.wifi_ssid)) {
            lv_label_set_text(g_app.settings_status_label, "已经保存了 Wi-Fi，改完后点“保存并重连”即可。");
        } else {
            lv_label_set_text(g_app.settings_status_label, "还没有 Wi-Fi，输入路由器名称和密码后点“保存并重连”。");
        }
    }
}

static void ui_show_wifi_settings(void)
{
    if (!g_app.settings_screen || !g_app.display) {
        return;
    }

    if (bsp_display_lock(2000)) {
        ui_settings_refresh_form();
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.settings_menu_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_menu_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_menu_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        lv_screen_load(g_app.settings_screen);
        bsp_display_unlock();
    }
    ui_mark_activity();
}

static void ui_settings_menu_refresh_form(void)
{
    if (!g_app.settings_menu_screen) {
        return;
    }

    watch_ai_config_t cfg;
    bool sta_connected = false;
    bool sta_connecting = false;
    bool ap_running = false;
    bool ap_forced = false;
    bool chat_busy = false;
    bool tts_busy = false;
    bool voice_busy = false;
    bool crypto_busy = false;
    bool time_synced = false;
    bool screen_sleeping = false;
    char sta_ip[16] = "";
    char ap_ip[16] = "";
    char last_error[MAX_ERROR_LEN] = "";
    char status_line[160] = "";

    state_lock();
    cfg = g_app.config;
    sta_connected = g_app.sta_connected;
    sta_connecting = g_app.sta_connecting;
    ap_running = g_app.ap_running;
    ap_forced = g_app.ap_forced;
    chat_busy = g_app.chat_busy;
    tts_busy = g_app.tts_busy;
    voice_busy = g_app.voice_busy;
    crypto_busy = g_app.crypto_busy;
    time_synced = g_app.time_synced;
    screen_sleeping = g_app.screen_sleeping;
    copy_string(sta_ip, sizeof(sta_ip), g_app.sta_ip);
    copy_string(ap_ip, sizeof(ap_ip), g_app.ap_ip);
    copy_string(last_error, sizeof(last_error), g_app.last_error);
    copy_string(status_line, sizeof(status_line), g_app.status_line);
    state_unlock();

    const char *mode = "离线";
    if (ap_running && sta_connected) {
        mode = "AP+STA";
    } else if (ap_running) {
        mode = "AP";
    } else if (sta_connected || sta_connecting) {
        mode = "STA";
    }

    char wifi_line[128] = {0};
    if (string_has_value(cfg.wifi_ssid)) {
        if (sta_connected && string_has_value(sta_ip)) {
            snprintf(wifi_line, sizeof(wifi_line), "%s · %s", cfg.wifi_ssid, sta_ip);
        } else if (sta_connecting) {
            snprintf(wifi_line, sizeof(wifi_line), "%s · 连接中", cfg.wifi_ssid);
        } else {
            snprintf(wifi_line, sizeof(wifi_line), "%s", cfg.wifi_ssid);
        }
    } else {
        snprintf(wifi_line, sizeof(wifi_line), "未配置");
    }

    char ap_line[128] = {0};
    if (cfg.ap_enabled) {
        if (ap_running && string_has_value(ap_ip)) {
            snprintf(ap_line, sizeof(ap_line), "开启 · %s", ap_ip);
        } else if (ap_running) {
            snprintf(ap_line, sizeof(ap_line), "开启 · 启动中");
        } else {
            snprintf(ap_line, sizeof(ap_line), "开启 · 启动中");
        }
    } else if (ap_forced) {
        snprintf(ap_line, sizeof(ap_line), "关闭（强制开启）");
    } else {
        snprintf(ap_line, sizeof(ap_line), "关闭");
    }

    char status_text[512] = {0};
    if (screen_sleeping) {
        snprintf(status_text, sizeof(status_text), "屏幕已待机，触摸任意位置唤醒。");
    } else if (last_error[0]) {
        snprintf(status_text, sizeof(status_text), "%s", last_error);
    } else {
        const char *activity = "准备就绪";
        if (voice_busy) {
            activity = "正在录音";
        } else if (chat_busy) {
            activity = "AI 正在思考";
        } else if (tts_busy) {
            activity = "正在朗读自然人声";
        } else if (crypto_busy) {
            activity = "行情正在刷新";
        } else if (status_line[0]) {
            activity = status_line;
        } else if (!time_synced) {
            activity = "时间未同步";
        }
        snprintf(status_text, sizeof(status_text),
                 "模式: %s\n热点: %s\nWi-Fi: %s\n状态: %s",
                 mode,
                 ap_line,
                 wifi_line,
                 activity);
    }

    if (bsp_display_lock(2000)) {
        if (g_app.settings_menu_title_label) {
            lv_label_set_text(g_app.settings_menu_title_label, "设置");
        }
        if (g_app.settings_menu_hint_label) {
            lv_label_set_text(g_app.settings_menu_hint_label,
                              "首页只保留行情和语音，Wi-Fi / 热点 / 自动待机 / 关屏都收在这里。API 和 Key 继续在手机网页里改。");
        }
        if (g_app.settings_menu_status_label) {
            lv_label_set_text(g_app.settings_menu_status_label, status_text);
        }
        if (g_app.settings_menu_wifi_label) {
            lv_label_set_text(g_app.settings_menu_wifi_label, "Wi-Fi 设置");
        }
        if (g_app.settings_menu_ap_label) {
            lv_label_set_text(g_app.settings_menu_ap_label, cfg.ap_enabled ? "关闭热点" : "开启热点");
        }
        if (g_app.settings_menu_restart_label) {
            lv_label_set_text(g_app.settings_menu_restart_label, "重启 Wi-Fi");
        }
        if (g_app.settings_menu_auto_sleep_label) {
            char auto_sleep_text[48] = {0};
            if (cfg.auto_sleep_timeout_s > 0) {
                snprintf(auto_sleep_text, sizeof(auto_sleep_text), "自动待机 %d秒", cfg.auto_sleep_timeout_s);
            } else {
                copy_string(auto_sleep_text, sizeof(auto_sleep_text), "自动待机 关闭");
            }
            lv_label_set_text(g_app.settings_menu_auto_sleep_label, auto_sleep_text);
        }
        if (g_app.settings_menu_back_label) {
            lv_label_set_text(g_app.settings_menu_back_label, "返回首页");
        }
        if (g_app.settings_menu_wifi_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_wifi_btn, lv_color_hex(0x5B8CFF), 0);
        }
        if (g_app.settings_menu_ap_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_ap_btn,
                                      cfg.ap_enabled ? lv_color_hex(0xFFB86C) : lv_color_hex(0x45D6C2),
                                      0);
        }
        if (g_app.settings_menu_restart_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_restart_btn, lv_color_hex(0xFFB86C), 0);
        }
        if (g_app.settings_menu_auto_sleep_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_auto_sleep_btn, lv_color_hex(0x5B8CFF), 0);
        }
        if (g_app.settings_menu_back_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_back_btn, lv_color_hex(0x56657A), 0);
        }
        if (g_app.settings_menu_sleep_label) {
            lv_label_set_text(g_app.settings_menu_sleep_label, g_app.screen_sleeping ? "唤醒" : "关屏");
        }
        if (g_app.settings_menu_sleep_btn) {
            lv_obj_set_style_bg_color(g_app.settings_menu_sleep_btn,
                                      g_app.screen_sleeping ? lv_color_hex(0x45D6C2) : lv_color_hex(0x56657A),
                                      0);
        }
        bsp_display_unlock();
    }
}

static void ui_settings_apply_wifi(bool forget_wifi)
{
    watch_ai_config_t cfg;
    state_lock();
    cfg = g_app.config;
    state_unlock();

    if (forget_wifi) {
        cfg.wifi_ssid[0] = '\0';
        cfg.wifi_password[0] = '\0';
    } else {
        const char *ssid = g_app.settings_wifi_ssid_ta ? lv_textarea_get_text(g_app.settings_wifi_ssid_ta) : "";
        const char *password = g_app.settings_wifi_password_ta ? lv_textarea_get_text(g_app.settings_wifi_password_ta) : "";
        if (string_has_value(ssid)) {
            copy_string(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), ssid);
        }
        if (string_has_value(password)) {
            copy_string(cfg.wifi_password, sizeof(cfg.wifi_password), password);
        }
    }

    config_prepare_runtime_defaults(&cfg);
    esp_err_t err = app_commit_config(&cfg, true);

    if (bsp_display_lock(2000)) {
        if (g_app.settings_status_label) {
            if (err == ESP_OK) {
                lv_label_set_text(g_app.settings_status_label,
                                  forget_wifi ? "Wi-Fi 已清空，正在重连热点" : "Wi-Fi 已保存，正在重连");
            } else {
                char err_text[MAX_ERROR_LEN] = {0};
                state_lock();
                copy_string(err_text, sizeof(err_text), g_app.last_error);
                state_unlock();
                lv_label_set_text(g_app.settings_status_label, err_text[0] ? err_text : "保存失败");
            }
        }
        bsp_display_unlock();
    }

    if (err == ESP_OK) {
        ui_mark_activity();
        if (bsp_display_lock(2000)) {
            ui_settings_refresh_form();
            bsp_display_unlock();
        }
    }
}

static void ui_show_sleep_settings(void)
{
    if (!g_app.display) {
        return;
    }
    if (!g_app.sleep_settings_screen) {
        ui_create_sleep_settings_screen();
    }
    if (!g_app.sleep_settings_screen) {
        return;
    }

    if (bsp_display_lock(2000)) {
        lv_screen_load(g_app.sleep_settings_screen);
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.settings_menu_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_menu_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_menu_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        bsp_display_unlock();
    }
    ui_sleep_settings_refresh_form();
    ui_mark_activity();
}

static void ui_sleep_settings_refresh_form(void)
{
    if (!g_app.sleep_settings_screen) {
        return;
    }

    watch_ai_config_t cfg;
    bool screen_sleeping = false;
    state_lock();
    cfg = g_app.config;
    screen_sleeping = g_app.screen_sleeping;
    state_unlock();

    char timeout_text[32] = {0};
    if (cfg.auto_sleep_timeout_s <= 0) {
        copy_string(timeout_text, sizeof(timeout_text), "0");
    } else {
        snprintf(timeout_text, sizeof(timeout_text), "%d", cfg.auto_sleep_timeout_s);
    }

    char status_text[256] = {0};
    if (screen_sleeping) {
        snprintf(status_text, sizeof(status_text), "屏幕已待机，当前自动待机设置为 %s 秒。", timeout_text);
    } else if (cfg.auto_sleep_timeout_s <= 0) {
        copy_string(status_text, sizeof(status_text), "自动待机已关闭，屏幕不会因为空闲自动熄屏。");
    } else {
        snprintf(status_text, sizeof(status_text), "当前设置为 %d 秒，空闲后会自动熄屏省电。", cfg.auto_sleep_timeout_s);
    }

    if (!bsp_display_lock(2000)) {
        return;
    }

    if (g_app.sleep_settings_title_label) {
        lv_label_set_text(g_app.sleep_settings_title_label, "自动待机");
    }
    if (g_app.sleep_settings_hint_label) {
        lv_label_set_text(g_app.sleep_settings_hint_label,
                          "输入空闲多少秒后自动熄屏。0 表示关闭自动待机，省电但不会自动黑屏。");
    }
    if (g_app.sleep_settings_timeout_label) {
        lv_label_set_text(g_app.sleep_settings_timeout_label, "自动待机秒数");
    }
    if (g_app.sleep_settings_status_label) {
        lv_label_set_text(g_app.sleep_settings_status_label, status_text);
    }
    if (g_app.sleep_settings_timeout_ta && !lv_obj_has_state(g_app.sleep_settings_timeout_ta, LV_STATE_FOCUSED)) {
        lv_textarea_set_text(g_app.sleep_settings_timeout_ta, timeout_text);
    }
    if (g_app.sleep_settings_keyboard) {
        lv_keyboard_set_mode(g_app.sleep_settings_keyboard, LV_KEYBOARD_MODE_NUMBER);
        if (g_app.sleep_settings_timeout_ta && lv_obj_has_state(g_app.sleep_settings_timeout_ta, LV_STATE_FOCUSED)) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, g_app.sleep_settings_timeout_ta);
        }
    }

    bsp_display_unlock();
}

static void ui_sleep_apply(void)
{
    watch_ai_config_t cfg;
    state_lock();
    cfg = g_app.config;
    state_unlock();

    const char *text = g_app.sleep_settings_timeout_ta ? lv_textarea_get_text(g_app.sleep_settings_timeout_ta) : "";
    while (text && *text && isspace((unsigned char)*text)) {
        ++text;
    }

    if (!string_has_value(text)) {
        if (bsp_display_lock(2000)) {
            if (g_app.sleep_settings_status_label) {
                lv_label_set_text(g_app.sleep_settings_status_label, "请输入 0-86400 之间的秒数。");
            }
            bsp_display_unlock();
        }
        return;
    }

    char *end = NULL;
    long timeout = strtol(text, &end, 10);
    while (end && *end && isspace((unsigned char)*end)) {
        ++end;
    }
    if (end == text || (end && *end != '\0') || timeout < 0 || timeout > AUTO_SLEEP_MAX_SECONDS) {
        if (bsp_display_lock(2000)) {
            if (g_app.sleep_settings_status_label) {
                char err_text[128] = {0};
                snprintf(err_text, sizeof(err_text), "请输入 0-%d 之间的整数秒数。", AUTO_SLEEP_MAX_SECONDS);
                lv_label_set_text(g_app.sleep_settings_status_label, err_text);
            }
            bsp_display_unlock();
        }
        return;
    }

    cfg.auto_sleep_timeout_s = (int)timeout;
    config_prepare_runtime_defaults(&cfg);
    esp_err_t err = app_commit_config(&cfg, false);
    if (err != ESP_OK) {
        if (bsp_display_lock(2000)) {
            if (g_app.sleep_settings_status_label) {
                char err_text[MAX_ERROR_LEN] = {0};
                state_lock();
                copy_string(err_text, sizeof(err_text), g_app.last_error);
                state_unlock();
                lv_label_set_text(g_app.sleep_settings_status_label, err_text[0] ? err_text : "保存失败");
            }
            bsp_display_unlock();
        }
        return;
    }

    state_lock();
    set_status_line_locked("自动待机已保存");
    state_unlock();
    ui_mark_activity();

    if (bsp_display_lock(2000)) {
        if (g_app.sleep_settings_status_label) {
            char status_text[192] = {0};
            if (cfg.auto_sleep_timeout_s <= 0) {
                copy_string(status_text, sizeof(status_text), "自动待机已关闭。");
            } else {
                snprintf(status_text, sizeof(status_text), "已保存，空闲 %d 秒后自动熄屏。", cfg.auto_sleep_timeout_s);
            }
            lv_label_set_text(g_app.sleep_settings_status_label, status_text);
        }
        bsp_display_unlock();
    }
}

static void ui_sleep_textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        ui_mark_activity();
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_mode(g_app.sleep_settings_keyboard, LV_KEYBOARD_MODE_NUMBER);
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, ta);
            lv_obj_clear_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        ui_mark_activity();
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        ui_mark_activity();
    }
}

static void ui_sleep_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ui_mark_activity();
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_CLICKED) {
        ui_mark_activity();
    }
}

static void ui_activity_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_PRESSED:
    case LV_EVENT_CLICKED:
    case LV_EVENT_SCROLL_BEGIN:
    case LV_EVENT_VALUE_CHANGED:
    case LV_EVENT_FOCUSED:
        ui_mark_activity();
        break;
    default:
        break;
    }
}

static void ui_textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        ui_mark_activity();
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, ta);
            lv_obj_clear_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        ui_mark_activity();
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        ui_mark_activity();
    }
}

static void ui_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ui_mark_activity();
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_CLICKED) {
        ui_mark_activity();
    }
}

static void ui_create_settings_menu_screen(void)
{
    if (!g_app.display || g_app.settings_menu_screen) {
        return;
    }

    if (!bsp_display_lock(5000)) {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for settings menu UI");
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(scr, &watch_ai_cn_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x111E33), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(scr, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_menu_title_label = lv_label_create(scr);
    lv_label_set_text(g_app.settings_menu_title_label, "设置");
    lv_obj_set_style_text_color(g_app.settings_menu_title_label, lv_color_hex(0xE8F1FF), 0);
    lv_obj_align(g_app.settings_menu_title_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 4);

    g_app.settings_menu_hint_label = lv_label_create(scr);
    lv_label_set_long_mode(g_app.settings_menu_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.settings_menu_hint_label, UI_SAFE_TEXT_W);
    lv_label_set_text(g_app.settings_menu_hint_label,
                      "首页只保留行情和语音，Wi-Fi / 热点 / 关屏都收在这里。API 和 Key 继续在手机网页里改。");
    lv_obj_set_style_text_color(g_app.settings_menu_hint_label, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(g_app.settings_menu_hint_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 32);

    lv_obj_t *status_card = lv_obj_create(scr);
    lv_obj_set_size(status_card, UI_SAFE_W, 92);
    lv_obj_align(status_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 76);
    lv_obj_set_style_radius(status_card, 22, 0);
    lv_obj_set_style_bg_color(status_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(status_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(status_card, 1, 0);
    lv_obj_set_style_pad_all(status_card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(status_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_menu_status_label = lv_label_create(status_card);
    lv_label_set_long_mode(g_app.settings_menu_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.settings_menu_status_label, UI_CARD_CONTENT_W);
    lv_label_set_text(g_app.settings_menu_status_label, " ");
    lv_obj_set_style_text_color(g_app.settings_menu_status_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.settings_menu_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *button_card = lv_obj_create(scr);
    lv_obj_set_size(button_card, UI_SAFE_W, 276);
    lv_obj_align(button_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 180);
    lv_obj_set_style_radius(button_card, 22, 0);
    lv_obj_set_style_bg_color(button_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(button_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(button_card, 1, 0);
    lv_obj_set_style_pad_all(button_card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(button_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_menu_wifi_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.settings_menu_wifi_btn, UI_CARD_CONTENT_W, 36);
    lv_obj_align(g_app.settings_menu_wifi_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(g_app.settings_menu_wifi_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_menu_wifi_btn, lv_color_hex(0x5B8CFF), 0);
    lv_obj_add_event_cb(g_app.settings_menu_wifi_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_OPEN_WIFI_SETTINGS);
    lv_obj_add_event_cb(g_app.settings_menu_wifi_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_menu_wifi_label = lv_label_create(g_app.settings_menu_wifi_btn);
    lv_label_set_text(g_app.settings_menu_wifi_label, "Wi-Fi 设置");
    lv_obj_center(g_app.settings_menu_wifi_label);

    g_app.settings_menu_ap_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.settings_menu_ap_btn, UI_CARD_CONTENT_W, 36);
    lv_obj_align(g_app.settings_menu_ap_btn, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_set_style_radius(g_app.settings_menu_ap_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_menu_ap_btn, lv_color_hex(0x45D6C2), 0);
    lv_obj_add_event_cb(g_app.settings_menu_ap_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_TOGGLE_AP);
    lv_obj_add_event_cb(g_app.settings_menu_ap_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_menu_ap_label = lv_label_create(g_app.settings_menu_ap_btn);
    lv_label_set_text(g_app.settings_menu_ap_label, "开启热点");
    lv_obj_center(g_app.settings_menu_ap_label);

    g_app.settings_menu_restart_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.settings_menu_restart_btn, UI_CARD_CONTENT_W, 36);
    lv_obj_align(g_app.settings_menu_restart_btn, LV_ALIGN_TOP_LEFT, 0, 84);
    lv_obj_set_style_radius(g_app.settings_menu_restart_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_menu_restart_btn, lv_color_hex(0xFFB86C), 0);
    lv_obj_add_event_cb(g_app.settings_menu_restart_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_RESTART_WIFI);
    lv_obj_add_event_cb(g_app.settings_menu_restart_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_menu_restart_label = lv_label_create(g_app.settings_menu_restart_btn);
    lv_label_set_text(g_app.settings_menu_restart_label, "重启 Wi-Fi");
    lv_obj_center(g_app.settings_menu_restart_label);

    g_app.settings_menu_auto_sleep_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.settings_menu_auto_sleep_btn, UI_CARD_CONTENT_W, 36);
    lv_obj_align(g_app.settings_menu_auto_sleep_btn, LV_ALIGN_TOP_LEFT, 0, 126);
    lv_obj_set_style_radius(g_app.settings_menu_auto_sleep_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_menu_auto_sleep_btn, lv_color_hex(0x5B8CFF), 0);
    lv_obj_add_event_cb(g_app.settings_menu_auto_sleep_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_OPEN_SLEEP_SETTINGS);
    lv_obj_add_event_cb(g_app.settings_menu_auto_sleep_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_menu_auto_sleep_label = lv_label_create(g_app.settings_menu_auto_sleep_btn);
    lv_label_set_text(g_app.settings_menu_auto_sleep_label, "自动待机");
    lv_obj_center(g_app.settings_menu_auto_sleep_label);

    g_app.settings_menu_sleep_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.settings_menu_sleep_btn, UI_CARD_CONTENT_W, 36);
    lv_obj_align(g_app.settings_menu_sleep_btn, LV_ALIGN_TOP_LEFT, 0, 168);
    lv_obj_set_style_radius(g_app.settings_menu_sleep_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_menu_sleep_btn, lv_color_hex(0x56657A), 0);
    lv_obj_add_event_cb(g_app.settings_menu_sleep_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_TOGGLE_SLEEP);
    lv_obj_add_event_cb(g_app.settings_menu_sleep_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_menu_sleep_label = lv_label_create(g_app.settings_menu_sleep_btn);
    lv_label_set_text(g_app.settings_menu_sleep_label, "关屏");
    lv_obj_center(g_app.settings_menu_sleep_label);

    g_app.settings_menu_back_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.settings_menu_back_btn, UI_CARD_CONTENT_W, 36);
    lv_obj_align(g_app.settings_menu_back_btn, LV_ALIGN_TOP_LEFT, 0, 210);
    lv_obj_set_style_radius(g_app.settings_menu_back_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_menu_back_btn, lv_color_hex(0x56657A), 0);
    lv_obj_add_event_cb(g_app.settings_menu_back_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_CHAT_BACK);
    lv_obj_add_event_cb(g_app.settings_menu_back_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_menu_back_label = lv_label_create(g_app.settings_menu_back_btn);
    lv_label_set_text(g_app.settings_menu_back_label, "返回首页");
    lv_obj_center(g_app.settings_menu_back_label);

    ui_apply_cn_font_tree(scr);
    g_app.settings_menu_screen = scr;
    bsp_display_unlock();

    ui_settings_menu_refresh_form();
}

static void ui_create(void)
{
    if (!g_app.display) {
        return;
    }

    if (!bsp_display_lock(5000)) {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for UI setup");
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(scr, &watch_ai_cn_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x111E33), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(scr, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.home_status_bar = lv_obj_create(scr);
    lv_obj_set_size(g_app.home_status_bar, UI_SAFE_W, 28);
    lv_obj_align(g_app.home_status_bar, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y);
    lv_obj_set_style_radius(g_app.home_status_bar, 14, 0);
    lv_obj_set_style_bg_color(g_app.home_status_bar, lv_color_hex(0x0D1829), 0);
    lv_obj_set_style_bg_opa(g_app.home_status_bar, LV_OPA_80, 0);
    lv_obj_set_style_border_color(g_app.home_status_bar, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(g_app.home_status_bar, 1, 0);
    lv_obj_set_style_pad_left(g_app.home_status_bar, 12, 0);
    lv_obj_set_style_pad_right(g_app.home_status_bar, 12, 0);
    lv_obj_set_style_pad_top(g_app.home_status_bar, 4, 0);
    lv_obj_set_style_pad_bottom(g_app.home_status_bar, 4, 0);
    lv_obj_set_flex_flow(g_app.home_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_app.home_status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_app.home_status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_app.home_status_bar, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.home_wifi_label = lv_label_create(g_app.home_status_bar);
    lv_obj_set_style_text_color(g_app.home_wifi_label, lv_color_hex(0xD9E7FF), 0);
    lv_label_set_text(g_app.home_wifi_label, "Wi-Fi");

    g_app.home_time_label = lv_label_create(g_app.home_status_bar);
    lv_obj_set_style_text_color(g_app.home_time_label, lv_color_hex(0xE8F1FF), 0);
    lv_label_set_text(g_app.home_time_label, "--:--");

    g_app.home_battery_box = lv_obj_create(g_app.home_status_bar);
    lv_obj_set_style_bg_opa(g_app.home_battery_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_app.home_battery_box, 0, 0);
    lv_obj_set_style_pad_all(g_app.home_battery_box, 0, 0);
    lv_obj_set_style_pad_gap(g_app.home_battery_box, 4, 0);
    lv_obj_set_size(g_app.home_battery_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_app.home_battery_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_app.home_battery_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_app.home_battery_box, LV_OBJ_FLAG_SCROLLABLE);

    g_app.home_battery_icon = lv_obj_create(g_app.home_battery_box);
    lv_obj_set_size(g_app.home_battery_icon, 18, 10);
    lv_obj_set_style_radius(g_app.home_battery_icon, 2, 0);
    lv_obj_set_style_bg_opa(g_app.home_battery_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_app.home_battery_icon, lv_color_hex(0xD9E7FF), 0);
    lv_obj_set_style_border_width(g_app.home_battery_icon, 1, 0);
    lv_obj_set_style_pad_all(g_app.home_battery_icon, 0, 0);
    lv_obj_clear_flag(g_app.home_battery_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *battery_cap = lv_obj_create(g_app.home_battery_icon);
    lv_obj_set_size(battery_cap, 3, 4);
    lv_obj_align(battery_cap, LV_ALIGN_OUT_RIGHT_MID, 1, 0);
    lv_obj_set_style_radius(battery_cap, 1, 0);
    lv_obj_set_style_bg_color(battery_cap, lv_color_hex(0xD9E7FF), 0);
    lv_obj_set_style_border_width(battery_cap, 0, 0);
    lv_obj_set_style_pad_all(battery_cap, 0, 0);
    lv_obj_clear_flag(battery_cap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *battery_fill = lv_obj_create(g_app.home_battery_icon);
    lv_obj_set_size(battery_fill, 11, 6);
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(battery_fill, 1, 0);
    lv_obj_set_style_bg_color(battery_fill, lv_color_hex(0x45D6C2), 0);
    lv_obj_set_style_border_width(battery_fill, 0, 0);
    lv_obj_set_style_pad_all(battery_fill, 0, 0);
    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);

    g_app.home_battery_label = lv_label_create(g_app.home_battery_box);
    lv_obj_set_style_text_color(g_app.home_battery_label, lv_color_hex(0xD9E7FF), 0);
    lv_label_set_text(g_app.home_battery_label, "88%");

    g_app.crypto_card = lv_obj_create(scr);
    lv_obj_set_size(g_app.crypto_card, UI_SAFE_W, 164);
    lv_obj_align(g_app.crypto_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 40);
    lv_obj_set_style_radius(g_app.crypto_card, 22, 0);
    lv_obj_set_style_bg_color(g_app.crypto_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(g_app.crypto_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(g_app.crypto_card, 1, 0);
    lv_obj_set_style_shadow_width(g_app.crypto_card, 18, 0);
    lv_obj_set_style_shadow_opa(g_app.crypto_card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(g_app.crypto_card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(g_app.crypto_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.crypto_title_label = lv_label_create(g_app.crypto_card);
    lv_label_set_text(g_app.crypto_title_label, "加密货币行情");
    lv_obj_set_style_text_color(g_app.crypto_title_label, lv_color_hex(0x45D6C2), 0);
    lv_obj_align(g_app.crypto_title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_app.crypto_status_label = lv_label_create(g_app.crypto_card);
    lv_label_set_text(g_app.crypto_status_label, "CoinGecko · 等待刷新");
    lv_obj_set_style_text_color(g_app.crypto_status_label, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(g_app.crypto_status_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    g_app.crypto_summary_label = lv_label_create(g_app.crypto_card);
    lv_label_set_long_mode(g_app.crypto_summary_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.crypto_summary_label, 178);
    lv_label_set_text(g_app.crypto_summary_label, "BTC / ETH / SOL 行情稍后显示。");
    lv_obj_set_style_text_color(g_app.crypto_summary_label, lv_color_hex(0xE8F1FF), 0);
    lv_obj_align(g_app.crypto_summary_label, LV_ALIGN_TOP_LEFT, 0, 36);

    g_app.crypto_chart = lv_chart_create(g_app.crypto_card);
    lv_obj_set_size(g_app.crypto_chart, 132, 106);
    lv_obj_align(g_app.crypto_chart, LV_ALIGN_TOP_RIGHT, 0, 38);
    lv_obj_set_style_bg_opa(g_app.crypto_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_app.crypto_chart, 0, 0);
    lv_obj_set_style_pad_all(g_app.crypto_chart, 0, 0);
    lv_chart_set_type(g_app.crypto_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_app.crypto_chart, BTC_CHART_POINT_COUNT);
    lv_chart_set_div_line_count(g_app.crypto_chart, 2, 2);
    lv_chart_set_update_mode(g_app.crypto_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_line_width(g_app.crypto_chart, 2, LV_PART_ITEMS);
    g_app.crypto_chart_series = lv_chart_add_series(g_app.crypto_chart, lv_color_hex(0x45D6C2), LV_CHART_AXIS_PRIMARY_Y);

    btc_chart_data_t placeholder_chart = {0};
    build_placeholder_btc_chart(&placeholder_chart);
    if (placeholder_chart.count > 0) {
        lv_chart_set_axis_range(g_app.crypto_chart, LV_CHART_AXIS_PRIMARY_Y, placeholder_chart.min_value, placeholder_chart.max_value);
        lv_chart_set_series_values(g_app.crypto_chart, g_app.crypto_chart_series, placeholder_chart.values, placeholder_chart.count);
        lv_chart_refresh(g_app.crypto_chart);
        state_lock();
        memcpy(g_app.crypto_chart_values, placeholder_chart.values, sizeof(placeholder_chart.values));
        g_app.crypto_chart_point_count = placeholder_chart.count;
        g_app.crypto_chart_min_value = placeholder_chart.min_value;
        g_app.crypto_chart_max_value = placeholder_chart.max_value;
        g_app.crypto_chart_generation = 1;
        g_app.crypto_chart_applied_generation = 1;
        g_app.crypto_chart_dirty = false;
        state_unlock();
    }

    g_app.crypto_updated_label = lv_label_create(g_app.crypto_card);
    lv_label_set_text(g_app.crypto_updated_label, " ");
    lv_obj_set_style_text_color(g_app.crypto_updated_label, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(g_app.crypto_updated_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    g_app.voice_card = lv_obj_create(scr);
    lv_obj_set_size(g_app.voice_card, UI_SAFE_W, 112);
    lv_obj_align(g_app.voice_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 216);
    lv_obj_set_style_radius(g_app.voice_card, 22, 0);
    lv_obj_set_style_bg_color(g_app.voice_card, lv_color_hex(0x0F1C33), 0);
    lv_obj_set_style_border_color(g_app.voice_card, lv_color_hex(0x2E5A85), 0);
    lv_obj_set_style_border_width(g_app.voice_card, 1, 0);
    lv_obj_set_style_shadow_width(g_app.voice_card, 18, 0);
    lv_obj_set_style_shadow_opa(g_app.voice_card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(g_app.voice_card, UI_CARD_PAD, 0);
    lv_obj_set_style_pad_row(g_app.voice_card, 8, 0);
    lv_obj_add_flag(g_app.voice_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_app.voice_card, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_START_VOICE_CHAT);
    lv_obj_add_event_cb(g_app.voice_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.voice_title_label = lv_label_create(g_app.voice_card);
    lv_label_set_text(g_app.voice_title_label, "语音聊天");
    lv_obj_set_style_text_color(g_app.voice_title_label, lv_color_hex(0x5B8CFF), 0);
    lv_obj_align(g_app.voice_title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_app.voice_desc_label = lv_label_create(g_app.voice_card);
    lv_label_set_long_mode(g_app.voice_desc_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.voice_desc_label, UI_CARD_CONTENT_W);
    lv_label_set_text(g_app.voice_desc_label, "点一下开始语音，全屏聊天。自然人声: alloy");
    lv_obj_set_style_text_color(g_app.voice_desc_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.voice_desc_label, LV_ALIGN_TOP_LEFT, 0, 36);

    g_app.settings_btn = lv_btn_create(scr);
    lv_obj_set_size(g_app.settings_btn, UI_SAFE_W, 50);
    lv_obj_align(g_app.settings_btn, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 342);
    lv_obj_set_style_radius(g_app.settings_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_btn, lv_color_hex(0x5B8CFF), 0);
    lv_obj_add_event_cb(g_app.settings_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_OPEN_SETTINGS_MENU);
    lv_obj_add_event_cb(g_app.settings_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    g_app.settings_label = lv_label_create(g_app.settings_btn);
    lv_label_set_text(g_app.settings_label, "设置");
    lv_obj_center(g_app.settings_label);

    ui_apply_cn_font_tree(scr);
    lv_screen_load(scr);
    g_app.screen = scr;
    state_lock();
    g_app.last_activity_us = esp_timer_get_time();
    g_app.screen_sleeping = false;
    state_unlock();
    g_app.ui_timer = lv_timer_create(ui_refresh_timer, 1000, NULL);
    bsp_display_unlock();

    ui_create_settings_menu_screen();
    ui_create_wifi_settings_screen();
    ui_create_sleep_settings_screen();
    ui_create_chat_screen();

    ui_refresh_timer(NULL);

    state_lock();
    g_app.crypto_busy = false;
    state_unlock();
    BaseType_t crypto_task_created = xTaskCreate(
        crypto_refresh_task,
        "crypto_refresh",
        12288,
        NULL,
        4,
        NULL);
    if (crypto_task_created != pdPASS) {
        state_lock();
        set_last_error_locked("创建行情刷新任务失败");
        state_unlock();
    }
}

static void ui_create_wifi_settings_screen(void)
{
    if (!g_app.display || g_app.settings_screen) {
        return;
    }

    if (!bsp_display_lock(5000)) {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for Wi-Fi settings UI");
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(scr, &watch_ai_cn_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x111E33), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(scr, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_title_label = lv_label_create(scr);
    lv_label_set_text(g_app.settings_title_label, "Wi-Fi 设置");
    lv_obj_set_style_text_color(g_app.settings_title_label, lv_color_hex(0xE8F1FF), 0);
    lv_obj_align(g_app.settings_title_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 4);

    g_app.settings_hint_label = lv_label_create(scr);
    lv_label_set_long_mode(g_app.settings_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.settings_hint_label, UI_SAFE_TEXT_W);
    lv_label_set_text(g_app.settings_hint_label, "直接在手表上输入路由器名称和密码，保存后会自动重连。密码留空表示保持原值。");
    lv_obj_set_style_text_color(g_app.settings_hint_label, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(g_app.settings_hint_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 34);

    g_app.settings_wifi_card = lv_obj_create(scr);
    lv_obj_set_size(g_app.settings_wifi_card, UI_SAFE_W, 206);
    lv_obj_align(g_app.settings_wifi_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 86);
    lv_obj_set_style_radius(g_app.settings_wifi_card, 22, 0);
    lv_obj_set_style_bg_color(g_app.settings_wifi_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(g_app.settings_wifi_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(g_app.settings_wifi_card, 1, 0);
    lv_obj_set_style_pad_all(g_app.settings_wifi_card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(g_app.settings_wifi_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_wifi_ssid_label = lv_label_create(g_app.settings_wifi_card);
    lv_label_set_text(g_app.settings_wifi_ssid_label, "Wi-Fi 名称");
    lv_obj_set_style_text_color(g_app.settings_wifi_ssid_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.settings_wifi_ssid_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_app.settings_wifi_ssid_ta = lv_textarea_create(g_app.settings_wifi_card);
    lv_textarea_set_one_line(g_app.settings_wifi_ssid_ta, true);
    lv_textarea_set_max_length(g_app.settings_wifi_ssid_ta, sizeof(g_app.config.wifi_ssid) - 1);
    lv_textarea_set_placeholder_text(g_app.settings_wifi_ssid_ta, "例如 HomeWiFi");
    lv_obj_set_width(g_app.settings_wifi_ssid_ta, UI_CARD_CONTENT_W);
    lv_obj_align(g_app.settings_wifi_ssid_ta, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_add_event_cb(g_app.settings_wifi_ssid_ta, ui_textarea_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.settings_wifi_ssid_ta, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_wifi_password_label = lv_label_create(g_app.settings_wifi_card);
    lv_label_set_text(g_app.settings_wifi_password_label, "Wi-Fi 密码");
    lv_obj_set_style_text_color(g_app.settings_wifi_password_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.settings_wifi_password_label, LV_ALIGN_TOP_LEFT, 0, 74);

    g_app.settings_wifi_password_ta = lv_textarea_create(g_app.settings_wifi_card);
    lv_textarea_set_one_line(g_app.settings_wifi_password_ta, true);
    lv_textarea_set_password_mode(g_app.settings_wifi_password_ta, true);
    lv_textarea_set_max_length(g_app.settings_wifi_password_ta, sizeof(g_app.config.wifi_password) - 1);
    lv_textarea_set_placeholder_text(g_app.settings_wifi_password_ta, "留空表示不修改");
    lv_obj_set_width(g_app.settings_wifi_password_ta, UI_CARD_CONTENT_W);
    lv_obj_align(g_app.settings_wifi_password_ta, LV_ALIGN_TOP_LEFT, 0, 102);
    lv_obj_add_event_cb(g_app.settings_wifi_password_ta, ui_textarea_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.settings_wifi_password_ta, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *wifi_tip = lv_label_create(g_app.settings_wifi_card);
    lv_label_set_long_mode(wifi_tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wifi_tip, UI_CARD_CONTENT_W);
    lv_label_set_text(wifi_tip, "提示: 留空密码会保留原值。输入新的名称和密码后点“保存并重连”。");
    lv_obj_set_style_text_color(wifi_tip, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(wifi_tip, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *action_card = lv_obj_create(scr);
    lv_obj_set_size(action_card, UI_SAFE_W, 160);
    lv_obj_align(action_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 304);
    lv_obj_set_style_radius(action_card, 22, 0);
    lv_obj_set_style_bg_color(action_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(action_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(action_card, 1, 0);
    lv_obj_set_style_pad_all(action_card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(action_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.settings_status_label = lv_label_create(action_card);
    lv_label_set_long_mode(g_app.settings_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.settings_status_label, UI_CARD_CONTENT_W);
    lv_label_set_text(g_app.settings_status_label, " ");
    lv_obj_set_style_text_color(g_app.settings_status_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.settings_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_app.settings_save_btn = lv_btn_create(action_card);
    lv_obj_set_size(g_app.settings_save_btn, UI_PAIR_BTN_W, 46);
    lv_obj_align(g_app.settings_save_btn, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_obj_set_style_radius(g_app.settings_save_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_save_btn, lv_color_hex(0x45D6C2), 0);
    lv_obj_add_event_cb(g_app.settings_save_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_WIFI_SAVE);
    lv_obj_add_event_cb(g_app.settings_save_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *save_label = lv_label_create(g_app.settings_save_btn);
    lv_label_set_text(save_label, "保存并重连");
    lv_obj_center(save_label);

    g_app.settings_forget_btn = lv_btn_create(action_card);
    lv_obj_set_size(g_app.settings_forget_btn, UI_PAIR_BTN_W, 46);
    lv_obj_align(g_app.settings_forget_btn, LV_ALIGN_TOP_RIGHT, 0, 46);
    lv_obj_set_style_radius(g_app.settings_forget_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_forget_btn, lv_color_hex(0xFF7B7B), 0);
    lv_obj_add_event_cb(g_app.settings_forget_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_WIFI_FORGET);
    lv_obj_add_event_cb(g_app.settings_forget_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *forget_label = lv_label_create(g_app.settings_forget_btn);
    lv_label_set_text(forget_label, "断开 Wi-Fi");
    lv_obj_center(forget_label);

    g_app.settings_back_btn = lv_btn_create(action_card);
    lv_obj_set_size(g_app.settings_back_btn, UI_CARD_CONTENT_W, 42);
    lv_obj_align(g_app.settings_back_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(g_app.settings_back_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.settings_back_btn, lv_color_hex(0x56657A), 0);
    lv_obj_add_event_cb(g_app.settings_back_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_WIFI_BACK);
    lv_obj_add_event_cb(g_app.settings_back_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *back_label = lv_label_create(g_app.settings_back_btn);
    lv_label_set_text(back_label, "返回设置");
    lv_obj_center(back_label);

    g_app.settings_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(g_app.settings_keyboard, UI_KEYBOARD_W, UI_KEYBOARD_H);
    lv_obj_align(g_app.settings_keyboard, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_app.settings_keyboard, ui_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.settings_keyboard, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    ui_apply_cn_font_tree(scr);
    ui_settings_refresh_form();
    g_app.settings_screen = scr;
    bsp_display_unlock();
}

static void ui_create_sleep_settings_screen(void)
{
    if (!g_app.display || g_app.sleep_settings_screen) {
        return;
    }

    if (!bsp_display_lock(5000)) {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for sleep settings UI");
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(scr, &watch_ai_cn_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x111E33), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(scr, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.sleep_settings_title_label = lv_label_create(scr);
    lv_label_set_text(g_app.sleep_settings_title_label, "自动待机");
    lv_obj_set_style_text_color(g_app.sleep_settings_title_label, lv_color_hex(0xE8F1FF), 0);
    lv_obj_align(g_app.sleep_settings_title_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 4);

    g_app.sleep_settings_hint_label = lv_label_create(scr);
    lv_label_set_long_mode(g_app.sleep_settings_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.sleep_settings_hint_label, UI_SAFE_TEXT_W);
    lv_label_set_text(g_app.sleep_settings_hint_label, "输入空闲多少秒后自动熄屏。0 表示关闭自动待机，省电但不会自动黑屏。");
    lv_obj_set_style_text_color(g_app.sleep_settings_hint_label, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(g_app.sleep_settings_hint_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 34);

    g_app.sleep_settings_screen = scr;

    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, UI_SAFE_W, 164);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 86);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.sleep_settings_timeout_label = lv_label_create(card);
    lv_label_set_text(g_app.sleep_settings_timeout_label, "自动待机秒数");
    lv_obj_set_style_text_color(g_app.sleep_settings_timeout_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.sleep_settings_timeout_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_app.sleep_settings_timeout_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(g_app.sleep_settings_timeout_ta, true);
    lv_textarea_set_accepted_chars(g_app.sleep_settings_timeout_ta, "0123456789");
    lv_textarea_set_max_length(g_app.sleep_settings_timeout_ta, 5);
    lv_textarea_set_placeholder_text(g_app.sleep_settings_timeout_ta, "例如 60，0 关闭自动待机");
    lv_obj_set_width(g_app.sleep_settings_timeout_ta, UI_CARD_CONTENT_W);
    lv_obj_align(g_app.sleep_settings_timeout_ta, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_add_event_cb(g_app.sleep_settings_timeout_ta, ui_sleep_textarea_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.sleep_settings_timeout_ta, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *sleep_tip = lv_label_create(card);
    lv_label_set_long_mode(sleep_tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sleep_tip, UI_CARD_CONTENT_W);
    lv_label_set_text(sleep_tip, "0 表示关闭自动待机。建议 30-300 秒，既省电又不会太频繁熄屏。");
    lv_obj_set_style_text_color(sleep_tip, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(sleep_tip, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *action_card = lv_obj_create(scr);
    lv_obj_set_size(action_card, UI_SAFE_W, 154);
    lv_obj_align(action_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 264);
    lv_obj_set_style_radius(action_card, 22, 0);
    lv_obj_set_style_bg_color(action_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(action_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(action_card, 1, 0);
    lv_obj_set_style_pad_all(action_card, UI_CARD_PAD, 0);
    lv_obj_add_event_cb(action_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.sleep_settings_status_label = lv_label_create(action_card);
    lv_label_set_long_mode(g_app.sleep_settings_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.sleep_settings_status_label, UI_CARD_CONTENT_W);
    lv_label_set_text(g_app.sleep_settings_status_label, " ");
    lv_obj_set_style_text_color(g_app.sleep_settings_status_label, lv_color_hex(0xD9E7FF), 0);
    lv_obj_align(g_app.sleep_settings_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    g_app.sleep_settings_save_btn = lv_btn_create(action_card);
    lv_obj_set_size(g_app.sleep_settings_save_btn, UI_PAIR_BTN_W, 48);
    lv_obj_align(g_app.sleep_settings_save_btn, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_obj_set_style_radius(g_app.sleep_settings_save_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.sleep_settings_save_btn, lv_color_hex(0x45D6C2), 0);
    lv_obj_add_event_cb(g_app.sleep_settings_save_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_SLEEP_SAVE);
    lv_obj_add_event_cb(g_app.sleep_settings_save_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *sleep_save_label = lv_label_create(g_app.sleep_settings_save_btn);
    lv_label_set_text(sleep_save_label, "保存");
    lv_obj_center(sleep_save_label);

    g_app.sleep_settings_back_btn = lv_btn_create(action_card);
    lv_obj_set_size(g_app.sleep_settings_back_btn, UI_PAIR_BTN_W, 48);
    lv_obj_align(g_app.sleep_settings_back_btn, LV_ALIGN_TOP_RIGHT, 0, 46);
    lv_obj_set_style_radius(g_app.sleep_settings_back_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.sleep_settings_back_btn, lv_color_hex(0x56657A), 0);
    lv_obj_add_event_cb(g_app.sleep_settings_back_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_SLEEP_BACK);
    lv_obj_add_event_cb(g_app.sleep_settings_back_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *sleep_back_label = lv_label_create(g_app.sleep_settings_back_btn);
    lv_label_set_text(sleep_back_label, "返回设置");
    lv_obj_center(sleep_back_label);

    g_app.sleep_settings_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(g_app.sleep_settings_keyboard, UI_KEYBOARD_W, UI_KEYBOARD_H);
    lv_obj_align(g_app.sleep_settings_keyboard, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_keyboard_set_mode(g_app.sleep_settings_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_app.sleep_settings_keyboard, ui_sleep_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.sleep_settings_keyboard, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    ui_apply_cn_font_tree(scr);
    bsp_display_unlock();

    ui_sleep_settings_refresh_form();
}

static char *build_chat_history_text_locked(void)
{
    const char *empty_text = "还没有对话，先发一条试试。";
    size_t total = strlen(empty_text) + 1;

    for (size_t i = 0; i < g_app.history_count; ++i) {
        const char *role = g_app.history[i].role;
        const char *role_label = "AI";
        if (strcmp(role, "user") == 0) {
            role_label = "我";
        } else if (strcmp(role, "assistant") == 0) {
            role_label = "AI";
        } else if (string_has_value(role)) {
            role_label = role;
        }
        const char *content = g_app.history[i].content ? g_app.history[i].content : "";
        total += strlen(role_label) + strlen(content) + 8;
    }

    char *text = calloc(1, total + 1);
    if (!text) {
        return NULL;
    }

    if (g_app.history_count == 0) {
        copy_string(text, total + 1, empty_text);
        return text;
    }

    char *cursor = text;
    size_t remaining = total + 1;
    for (size_t i = 0; i < g_app.history_count; ++i) {
        const char *role = g_app.history[i].role;
        const char *role_label = "AI";
        if (strcmp(role, "user") == 0) {
            role_label = "我";
        } else if (strcmp(role, "assistant") == 0) {
            role_label = "AI";
        } else if (string_has_value(role)) {
            role_label = role;
        }
        const char *content = g_app.history[i].content ? g_app.history[i].content : "";
        int written = snprintf(cursor, remaining, "%s:\n%s\n\n", role_label, content);
        if (written < 0 || (size_t)written >= remaining) {
            text[total] = '\0';
            break;
        }
        cursor += (size_t)written;
        remaining -= (size_t)written;
    }

    return text;
}

static void __attribute__((unused)) ui_refresh_chat_history_text(const char *history_text)
{
    if (!g_app.chat_history_label || !g_app.chat_history_container) {
        return;
    }

    if (!bsp_display_lock(2000)) {
        return;
    }

    lv_label_set_text(g_app.chat_history_label, history_text && history_text[0] ? history_text : "还没有对话，先发一条试试。");
    lv_obj_scroll_by(g_app.chat_history_container, 0, 10000, LV_ANIM_OFF);
    bsp_display_unlock();
}

static void ui_chat_set_status(const char *text)
{
    if (!g_app.chat_status_label) {
        return;
    }

    if (!bsp_display_lock(2000)) {
        return;
    }

    lv_label_set_text(g_app.chat_status_label, text && text[0] ? text : " ");
    bsp_display_unlock();
}

static void ui_chat_refresh_form(void)
{
    if (!g_app.chat_screen) {
        return;
    }

    watch_ai_config_t cfg;
    bool chat_busy = false;
    bool tts_busy = false;
    bool screen_sleeping = false;
    size_t history_count = 0;
    char last_error[MAX_ERROR_LEN] = "";
    char status_line[160] = "";
    char voice_line[160] = "";

    state_lock();
    cfg = g_app.config;
    chat_busy = g_app.chat_busy;
    tts_busy = g_app.tts_busy;
    screen_sleeping = g_app.screen_sleeping;
    history_count = g_app.history_count;
    copy_string(last_error, sizeof(last_error), g_app.last_error);
    copy_string(status_line, sizeof(status_line), g_app.status_line);
    state_unlock();

    snprintf(voice_line, sizeof(voice_line), "TTS: %s | %s | %.24s | %.1fx",
             cfg.tts_enabled ? "ON" : "OFF",
             cfg.tts_model,
             cfg.tts_voice,
             (double)cfg.tts_speed);

    const char *status_text = "准备就绪";
    if (screen_sleeping) {
        status_text = "屏幕已待机";
    } else if (last_error[0]) {
        status_text = last_error;
    } else if (chat_busy) {
        status_text = "AI 正在思考...";
    } else if (tts_busy) {
        status_text = "AI 已回复，正在朗读...";
    } else if (status_line[0]) {
        status_text = status_line;
    }

    char *history_text = NULL;
    state_lock();
    history_text = build_chat_history_text_locked();
    state_unlock();

    if (!bsp_display_lock(2000)) {
        free(history_text);
        return;
    }

    static size_t s_last_history_count = SIZE_MAX;
    bool should_scroll = (history_count != s_last_history_count);
    s_last_history_count = history_count;

    if (g_app.chat_title_label) {
        lv_label_set_text(g_app.chat_title_label, "全屏聊天");
    }
    if (g_app.chat_status_label) {
        lv_label_set_text(g_app.chat_status_label, status_text);
    }
    if (g_app.chat_voice_label) {
        lv_label_set_text(g_app.chat_voice_label, voice_line);
    }
    if (g_app.chat_history_label) {
        lv_label_set_text(g_app.chat_history_label, history_text && history_text[0] ? history_text : "还没有对话，先发一条试试。");
        if (should_scroll) {
            lv_obj_scroll_by(g_app.chat_history_container, 0, 10000, LV_ANIM_OFF);
        }
    }
    bsp_display_unlock();
    free(history_text);
}

static void ui_chat_textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        ui_mark_activity();
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, ta);
            lv_obj_clear_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        ui_mark_activity();
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        ui_mark_activity();
    }
}

static void ui_chat_keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ui_mark_activity();
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_CLICKED) {
        ui_mark_activity();
    }
}

static void ui_chat_submit_text(const char *text)
{
    if (!text) {
        text = "";
    }

    char prompt[MAX_MESSAGE_LEN] = {0};
    copy_string(prompt, sizeof(prompt), text);

    char *trim = prompt;
    while (*trim && isspace((unsigned char)*trim)) {
        ++trim;
    }
    char *end = trim + strlen(trim);
    while (end > trim && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';

    if (!string_has_value(trim)) {
        ui_chat_set_status("先输入一条消息");
        return;
    }

    watch_ai_config_t cfg;
    bool busy = false;
    state_lock();
    busy = g_app.chat_busy;
    cfg = g_app.config;
    state_unlock();
    if (busy) {
        ui_chat_set_status("已有任务在处理，请稍等");
        return;
    }

    chat_async_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        ui_chat_set_status("创建聊天任务失败");
        state_lock();
        set_last_error_locked("创建聊天任务失败");
        state_unlock();
        return;
    }

    copy_string(job->message, sizeof(job->message), trim);
    job->cfg = cfg;

    BaseType_t created = xTaskCreate(
        chat_turn_task,
        "chat_turn",
        12288,
        job,
        5,
        NULL);
    if (created != pdPASS) {
        free(job);
        ui_chat_set_status("创建聊天任务失败");
        state_lock();
        set_last_error_locked("创建聊天任务失败");
        state_unlock();
        return;
    }

    if (g_app.chat_input_ta) {
        lv_textarea_set_text(g_app.chat_input_ta, "");
    }
    ui_chat_set_status("已发送，正在思考...");
    ui_mark_activity();
}

static void ui_chat_clear_history(void)
{
    history_clear();
    state_lock();
    set_status_line_locked("对话已清空");
    state_unlock();
    ui_chat_set_status("对话已清空");
    ui_chat_refresh_form();
}

static void ui_show_chat_screen(void)
{
    if (!g_app.display) {
        return;
    }
    if (!g_app.chat_screen) {
        ui_create_chat_screen();
    }
    if (!g_app.chat_screen) {
        return;
    }

    if (bsp_display_lock(2000)) {
        lv_screen_load(g_app.chat_screen);
        if (g_app.settings_menu_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_menu_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_menu_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.settings_keyboard) {
            lv_keyboard_set_textarea(g_app.settings_keyboard, NULL);
            lv_obj_add_flag(g_app.settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.sleep_settings_keyboard) {
            lv_keyboard_set_textarea(g_app.sleep_settings_keyboard, NULL);
            lv_obj_add_flag(g_app.sleep_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        if (g_app.chat_keyboard) {
            lv_keyboard_set_textarea(g_app.chat_keyboard, NULL);
            lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        bsp_display_unlock();
    }
    ui_chat_refresh_form();
    ui_mark_activity();
}

static void ui_create_chat_screen(void)
{
    if (!g_app.display || g_app.chat_screen) {
        return;
    }

    if (!bsp_display_lock(5000)) {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for chat UI");
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(scr, &watch_ai_cn_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x111E33), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(scr, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.chat_title_label = lv_label_create(scr);
    lv_label_set_text(g_app.chat_title_label, "全屏聊天");
    lv_obj_set_style_text_color(g_app.chat_title_label, lv_color_hex(0xE8F1FF), 0);
    lv_obj_align(g_app.chat_title_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 2);

    g_app.chat_status_label = lv_label_create(scr);
    lv_label_set_long_mode(g_app.chat_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.chat_status_label, UI_SAFE_TEXT_W);
    lv_obj_set_style_text_color(g_app.chat_status_label, lv_color_hex(0x45D6C2), 0);
    lv_obj_align(g_app.chat_status_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 30);

    g_app.chat_voice_label = lv_label_create(scr);
    lv_label_set_long_mode(g_app.chat_voice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.chat_voice_label, UI_SAFE_TEXT_W);
    lv_obj_set_style_text_color(g_app.chat_voice_label, lv_color_hex(0x90A3C3), 0);
    lv_obj_align(g_app.chat_voice_label, LV_ALIGN_TOP_LEFT, UI_SAFE_TEXT_X, UI_SAFE_Y + 56);

    g_app.chat_history_container = lv_obj_create(scr);
    lv_obj_set_size(g_app.chat_history_container, UI_SAFE_W, 206);
    lv_obj_align(g_app.chat_history_container, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 90);
    lv_obj_set_style_radius(g_app.chat_history_container, 22, 0);
    lv_obj_set_style_bg_color(g_app.chat_history_container, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(g_app.chat_history_container, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(g_app.chat_history_container, 1, 0);
    lv_obj_set_style_pad_all(g_app.chat_history_container, UI_CARD_PAD, 0);
    lv_obj_set_scroll_dir(g_app.chat_history_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_app.chat_history_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(g_app.chat_history_container, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.chat_history_label = lv_label_create(g_app.chat_history_container);
    lv_label_set_long_mode(g_app.chat_history_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_app.chat_history_label, UI_CARD_CONTENT_W);
    lv_obj_set_style_text_color(g_app.chat_history_label, lv_color_hex(0xE8F1FF), 0);
    lv_obj_align(g_app.chat_history_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *input_card = lv_obj_create(scr);
    lv_obj_set_size(input_card, UI_SAFE_W, 90);
    lv_obj_align(input_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 306);
    lv_obj_set_style_radius(input_card, 22, 0);
    lv_obj_set_style_bg_color(input_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(input_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(input_card, 1, 0);
    lv_obj_set_style_pad_all(input_card, 12, 0);
    lv_obj_add_event_cb(input_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.chat_input_ta = lv_textarea_create(input_card);
    lv_textarea_set_one_line(g_app.chat_input_ta, false);
    lv_textarea_set_max_length(g_app.chat_input_ta, MAX_MESSAGE_LEN - 1);
    lv_textarea_set_placeholder_text(g_app.chat_input_ta, "输入要问 AI 的内容，点发送开始聊天");
    lv_obj_set_size(g_app.chat_input_ta, UI_CARD_CONTENT_W - 4, 64);
    lv_obj_align(g_app.chat_input_ta, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(g_app.chat_input_ta, ui_chat_textarea_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.chat_input_ta, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *button_card = lv_obj_create(scr);
    lv_obj_set_size(button_card, UI_SAFE_W, 52);
    lv_obj_align(button_card, LV_ALIGN_TOP_LEFT, UI_SAFE_X, UI_SAFE_Y + 406);
    lv_obj_set_style_radius(button_card, 22, 0);
    lv_obj_set_style_bg_color(button_card, lv_color_hex(0x101B2D), 0);
    lv_obj_set_style_border_color(button_card, lv_color_hex(0x243451), 0);
    lv_obj_set_style_border_width(button_card, 1, 0);
    lv_obj_set_style_pad_all(button_card, 10, 0);
    lv_obj_add_event_cb(button_card, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.chat_send_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.chat_send_btn, UI_CHAT_BTN_W, 34);
    lv_obj_align(g_app.chat_send_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(g_app.chat_send_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.chat_send_btn, lv_color_hex(0x45D6C2), 0);
    lv_obj_add_event_cb(g_app.chat_send_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_CHAT_SEND);
    lv_obj_add_event_cb(g_app.chat_send_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *send_label = lv_label_create(g_app.chat_send_btn);
    lv_label_set_text(send_label, "发送");
    lv_obj_center(send_label);

    g_app.chat_clear_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.chat_clear_btn, UI_CHAT_BTN_W, 34);
    lv_obj_align(g_app.chat_clear_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g_app.chat_clear_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.chat_clear_btn, lv_color_hex(0xFFB86C), 0);
    lv_obj_add_event_cb(g_app.chat_clear_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_CHAT_CLEAR);
    lv_obj_add_event_cb(g_app.chat_clear_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *clear_label = lv_label_create(g_app.chat_clear_btn);
    lv_label_set_text(clear_label, "清空");
    lv_obj_center(clear_label);

    g_app.chat_back_btn = lv_btn_create(button_card);
    lv_obj_set_size(g_app.chat_back_btn, UI_CHAT_BTN_W, 34);
    lv_obj_align(g_app.chat_back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(g_app.chat_back_btn, 999, 0);
    lv_obj_set_style_bg_color(g_app.chat_back_btn, lv_color_hex(0x56657A), 0);
    lv_obj_add_event_cb(g_app.chat_back_btn, ui_action_handler, LV_EVENT_CLICKED, (void *)UI_ACTION_CHAT_BACK);
    lv_obj_add_event_cb(g_app.chat_back_btn, ui_activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *back_label = lv_label_create(g_app.chat_back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_center(back_label);

    g_app.chat_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(g_app.chat_keyboard, UI_KEYBOARD_W, UI_KEYBOARD_H);
    lv_obj_align(g_app.chat_keyboard, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(g_app.chat_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_app.chat_keyboard, ui_chat_keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(g_app.chat_keyboard, ui_activity_event_cb, LV_EVENT_ALL, NULL);

    ui_apply_cn_font_tree(scr);
    g_app.chat_screen = scr;
    bsp_display_unlock();

    ui_chat_refresh_form();
}

static void app_state_init(void)
{
    memset(&g_app, 0, sizeof(g_app));
    g_app.mutex = xSemaphoreCreateMutex();
    if (!g_app.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
}

static void app_boot_config(void)
{
    watch_ai_config_t cfg;
    bool needs_save = false;
    esp_err_t err = config_load(&cfg, &needs_save);
    if (err != ESP_OK) {
        config_set_defaults(&cfg);
        needs_save = true;
    }
    config_prepare_runtime_defaults(&cfg);
    if (needs_save) {
        esp_err_t save_err = config_save(&cfg);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save boot config: %s", esp_err_to_name(save_err));
        }
    }
    state_lock();
    g_app.config = cfg;
    g_app.config_loaded = true;
    build_wifi_qr_payload(g_app.qr_payload, sizeof(g_app.qr_payload), &g_app.config);
    set_status_line_locked("配置已载入");
    state_unlock();
}

static void app_start_ui(void)
{
    g_app.display = bsp_display_start();
    if (!g_app.display) {
        ESP_LOGW(TAG, "Display init failed, UI disabled");
        return;
    }
    bsp_display_brightness_set(100);
    ui_create();
}

static void app_start_wifi(void)
{
    if (wifi_init_and_start() != ESP_OK) {
        state_lock();
        set_last_error_locked("Wi-Fi 初始化失败");
        state_unlock();
    }
}

static void app_start_web(void)
{
    g_app.httpd = start_webserver();
    if (!g_app.httpd) {
        state_lock();
        set_last_error_locked("HTTP 服务器启动失败");
        state_unlock();
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_state_init();
    app_boot_config();
    app_start_wifi();
    app_start_ui();
    app_start_web();

    state_lock();
    ESP_LOGI(TAG, "Boot complete. AP=%s, Wi-Fi=%s, API=%s",
             g_app.config.ap_ssid,
             g_app.config.wifi_ssid,
             g_app.config.api_url);
    state_unlock();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

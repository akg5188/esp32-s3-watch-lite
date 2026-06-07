#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <strings.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif
#include "freertos/FreeRTOS.h"
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "freertos/idf_additions.h"
#endif
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "src/display/lv_display.h"
#include "src/draw/lv_draw_buf.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "src/draw/lv_draw_line.h"
#include "nvs.h"
#include "nvs_flash.h"

LV_FONT_DECLARE(watch_ai_cn_24);
#define AI_DIALOG_FONT (&watch_ai_cn_24)
#define AI_TEXT_FILTER_FONT (&watch_ai_cn_24)
#define AI_BUTTON_FONT (&watch_ai_cn_24)

#define TAG "watch_lite"

static bool s_ai_text_filter_enabled;

#define APP_NS "watch_lite"
#define CONFIG_MAGIC 0x574C5431u
#define CONFIG_VERSION 5u

#define AP_PASSWORD "12345678"
#define DEFAULT_VOICE_GATEWAY_URL "http://127.0.0.1:8790"
#define DEFAULT_API_URL "https://ai.orbitlink.me/v1"
#define DEFAULT_MODEL "gpt-5.4"
#define AI_PROFILE_COUNT 6
#define AI_PROFILE_CUSTOM_INDEX 5
#define AI_PROFILE_MAGIC 0x41495031u
#define AI_PROFILE_VERSION 1u
#define DEFAULT_MARKET_PROVIDER "coingecko"
#define DEFAULT_STARTER_PROMPT "用中文简短回复：手表 AI 聊天已经启动，请问我可以帮你什么？"
#define DEFAULT_AUTO_SLEEP_S 45
#define MAX_AUTO_SLEEP_S 86400
#define DEFAULT_BRIGHTNESS_PERCENT 35
#define DEFAULT_MOTION_WAKE_ENABLED 0
#define DEFAULT_MOTION_WAKE_THRESHOLD_MG 2000
#define MIN_MOTION_WAKE_THRESHOLD_MG 600
#define MAX_MOTION_WAKE_THRESHOLD_MG 6000
#define DEEP_SLEEP_ENABLED 1
#define DEEP_SLEEP_AUTO_ENTER 0
// Light sleep wakes from BOOT, but the touch I2C driver aborts after resume on this board.
#define BUTTON_LIGHT_SLEEP_ENABLED 0
#define AUTO_POWEROFF_ON_SLEEP 1
#define DEEP_SLEEP_AFTER_SCREEN_OFF_S 15
#define DEEP_SLEEP_BOOT_BUTTON_GPIO GPIO_NUM_0
#define DEEP_SLEEP_POWER_BUTTON_GPIO GPIO_NUM_10
#define BOOT_LONG_PRESS_CONFIG_AP_MS 2000
#define BUSY_SLEEP_FORCE_RESET_MS 90000
#define MARKET_BUSY_STALE_MS 65000
#define CHART_BUSY_STALE_MS 65000
#define TTS_BUSY_STALE_MS 90000

#define MARKET_WAIT_INTERVAL_MS 2000
#define DIRECT_HTTP_TIMEOUT_MS 12000
#define DIRECT_AI_TIMEOUT_MS 35000
#define DIRECT_HTTP_MAX_BODY 12288
#define MARKET_HOME_YAHOO_MAX_PER_REFRESH 3
#define VOICE_AI_TIMEOUT_MS 180000
#define VOICE_GATEWAY_TIMEOUT_MS 45000
#define VOICE_AUDIO_TIMEOUT_MS 30000
#define VOICE_NET_LOCK_WAIT_MS 8000
#define VOICE_STALE_RESET_MS 65000
#define VOICE_SAMPLE_RATE 22050
#define VOICE_CHANNELS 1
#define VOICE_CAPTURE_CHANNELS 1
#define VOICE_BITS_PER_SAMPLE 16
#define VOICE_MIC_GAIN_DB 36.0f
#define VOICE_MAX_SECONDS 7
#define VOICE_MIN_RECORD_MS 700
#define VOICE_MAX_IDLE_MS 1000
#define VOICE_SILENCE_MS 600
#define VOICE_WARMUP_DISCARD_MS 100
#define VOICE_SPEECH_THRESHOLD 380
#define VOICE_FORCE_SPEECH_THRESHOLD 900
#define VOICE_NOISE_CALIBRATE_MS 400
#define VOICE_SPEECH_START_CHUNKS 2
#define VOICE_ENERGY_LOG_MS 500
#define VOICE_READ_CHUNK_BYTES 4096
#define VOICE_MAX_AUDIO_BYTES (2 * 1024 * 1024)
#define VOICE_AUDIO_GROW_STEP (16 * 1024)
#define VOICE_PLAY_VOLUME 75
#define VOICE_TASK_STACK_BYTES (20 * 1024)
#define VOICE_TASK_FALLBACK_STACK_BYTES (12 * 1024)
#define VOICE_TTS_TASK_STACK_BYTES (16 * 1024)
#define VOICE_TTS_TASK_FALLBACK_STACK_BYTES (10 * 1024)
#define VOICE_KEEP_STA_CONNECTED 0
#define VOICE_TRY_COMBINED_GATEWAY 1
#define SETUP_PAGE_MAX 24000
#define COINGECKO_MARKET_URL "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum&vs_currencies=usd&include_24hr_change=true"
#define STOOQ_QUOTE_URL_FMT "https://stooq.com/q/l/?s=%s&f=sd2t2ohlcv&h&e=csv"
#define YAHOO_CHART_URL_FMT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=30m"
#define YAHOO_HOME_URL_FMT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=1d"
#define WIFI_CONNECT_WAIT_MS 15000
#define WIFI_CONNECT_RETRY_MS 1600
#define WIFI_SLOT_COUNT 5
#define WIFI_SCAN_MAX_RESULTS 20
#define MARKET_REFRESH_INTERVAL_S 300
#define MARKET_ROW_COUNT 15
#define CHART_POINT_COUNT 48
#define MARKET_CACHE_MAGIC 0x4D4B5431u
#define MARKET_CACHE_VERSION 1u

#define SAFE_X 42
#define SAFE_Y 34
#define SAFE_W (BSP_LCD_H_RES - (SAFE_X * 2))
#define SAFE_H (BSP_LCD_V_RES - (SAFE_Y * 2))

#define QMI8658_ADDR_LOW 0x6B
#define QMI8658_ADDR_HIGH 0x6A
#define QMI8658_WHOAMI 0x00
#define QMI8658_WHOAMI_VALUE 0x05
#define QMI8658_CTRL1 0x02
#define QMI8658_CTRL2 0x03
#define QMI8658_CTRL5 0x06
#define QMI8658_CTRL7 0x08
#define QMI8658_CTRL7_DISABLE 0x00
#define QMI8658_RESET 0x60
#define QMI8658_RST_RESULT 0x4D
#define QMI8658_RST_RESULT_VALUE 0x80
#define QMI8658_ACCEL_X_L 0x35
#define QMI8658_ACCEL_RANGE_MG 8000
#define MOTION_WAKE_POLL_MS 80
#define MOTION_WAKE_COOLDOWN_US 3000000LL

#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00
#define AXP2101_STATUS2 0x01
#define AXP2101_CHIP_ID 0x03
#define AXP2101_CHIP_ID_VALUE_A 0x4A
#define AXP2101_CHIP_ID_VALUE_B 0x47
#define AXP2101_COMMON_CONFIG 0x10
#define AXP2101_SOFT_SHUTDOWN_MASK (1u << 0)
#define AXP2101_CHARGE_GAUGE_WDT_CTRL 0x18
#define AXP2101_GAUGE_ENABLE_MASK (1u << 3)
#define AXP2101_ADC_DATA_VBAT_H 0x34
#define AXP2101_ADC_DATA_VBAT_L 0x35
#define AXP2101_GAUGE_VBAT_H_MASK 0x1F
#define AXP2101_BAT_DET_CTRL 0x68
#define AXP2101_BAT_TYPE_DET_MASK (1u << 0)
#define AXP2101_BAT_PRESENT_MASK (1u << 3)
#define AXP2101_BAT_PERCENT_DATA 0xA4
#define AXP2101_LDO_ONOFF_CTRL0 0x90
#define AXP2101_ALDO2_ENABLE_MASK (1u << 1)

#define WL_RETURN_ON_ERROR(expr, message) do {                         \
        esp_err_t wl_err__ = (expr);                                    \
        if (wl_err__ != ESP_OK) {                                       \
            ESP_LOGE(TAG, "%s: %s", (message), esp_err_to_name(wl_err__)); \
            return wl_err__;                                            \
        }                                                               \
    } while (0)

typedef struct {
    char static_ip[16];
    char static_netmask[16];
    char static_gateway[16];
    char dns1[16];
    char dns2[16];
} wifi_ip_config_t;

typedef struct {
    char name[16];
    char server_url[160];
    char api_key[192];
    char model[64];
} ai_profile_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    int active_index;
    ai_profile_t profiles[AI_PROFILE_COUNT];
} ai_profile_store_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_url[160];
    char api_key[192];
    char model[64];
    char market_provider[16];
    char starter_prompt[160];
    char static_ip[16];
    char static_netmask[16];
    char static_gateway[16];
    char dns1[16];
    char dns2[16];
    char proxy_host[64];
    int proxy_port;
    int auto_sleep_s;
} watch_lite_config_prefix_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_url[160];
    char api_key[192];
    char model[64];
    char market_provider[16];
    char starter_prompt[160];
    char static_ip[16];
    char static_netmask[16];
    char static_gateway[16];
    char dns1[16];
    char dns2[16];
    char proxy_host[64];
    int proxy_port;
    int auto_sleep_s;
    int brightness_percent;
    int motion_wake_enabled;
    int motion_wake_threshold_mg;
    char wifi_ssid2[33];
    char wifi_password2[65];
    char wifi_ssid3[33];
    char wifi_password3[65];
    char wifi_ssid4[33];
    char wifi_password4[65];
    char wifi_ssid5[33];
    char wifi_password5[65];
    wifi_ip_config_t wifi_net[WIFI_SLOT_COUNT];
} watch_lite_config_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_url[160];
    char api_key[192];
    char model[64];
    char market_provider[16];
    char starter_prompt[160];
    char static_ip[16];
    char static_netmask[16];
    char static_gateway[16];
    char dns1[16];
    char dns2[16];
    char proxy_host[64];
    int proxy_port;
    int auto_sleep_s;
    int brightness_percent;
    int motion_wake_enabled;
    int motion_wake_threshold_mg;
    char wifi_ssid2[33];
    char wifi_password2[65];
    char wifi_ssid3[33];
    char wifi_password3[65];
    char wifi_ssid4[33];
    char wifi_password4[65];
    char wifi_ssid5[33];
    char wifi_password5[65];
} watch_lite_config_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_url[160];
    int auto_sleep_s;
} watch_lite_config_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_url[160];
    char api_key[192];
    char model[64];
    char market_provider[16];
    char starter_prompt[160];
    char static_ip[16];
    char static_netmask[16];
    char static_gateway[16];
    char dns1[16];
    char dns2[16];
    char proxy_host[64];
    int proxy_port;
    int auto_sleep_s;
} watch_lite_config_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[33];
    char wifi_password[65];
    char server_url[160];
    char api_key[192];
    char model[64];
    char market_provider[16];
    char starter_prompt[160];
    char static_ip[16];
    char static_netmask[16];
    char static_gateway[16];
    char dns1[16];
    char dns2[16];
    char proxy_host[64];
    int proxy_port;
    int auto_sleep_s;
    int brightness_percent;
    int motion_wake_enabled;
    int motion_wake_threshold_mg;
} watch_lite_config_v3_full_t;

typedef struct {
    char *data;
    int len;
    int capacity;
} http_buffer_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool truncated;
} binary_buffer_t;

typedef struct {
    size_t data_offset;
    size_t data_len;
    esp_codec_dev_sample_info_t fs;
} wav_audio_info_t;

typedef struct {
    char reply[512];
    bool created_with_caps;
} voice_tts_job_t;

typedef struct {
    char transcript[192];
    char reply[512];
    char audio_url[224];
    char action[32];
    char asset[16];
    int brightness_delta;
    bool local;
    bool refresh_market;
} voice_gateway_result_t;

typedef enum {
    MARKET_SOURCE_CRYPTO = 0,
    MARKET_SOURCE_STOOQ,
    MARKET_SOURCE_YAHOO,
} market_source_t;

typedef struct {
    const char *code;
    const char *symbol;
    const char *chart_symbol;
    market_source_t source;
    uint32_t color;
} market_asset_def_t;

typedef struct {
    char price[24];
    char change[16];
    lv_obj_t *code_label;
    lv_obj_t *price_label;
    lv_obj_t *change_label;
    lv_obj_t *hit_obj;
} market_row_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t row_count;
    char market_time[16];
    char price[MARKET_ROW_COUNT][24];
    char change[MARKET_ROW_COUNT][16];
} market_cache_t;

typedef struct {
    watch_lite_config_t cfg;
    char ap_ssid[32];
    char sta_ip[16];
    bool sta_configured;
    bool sta_connected;
    bool sta_connect_requested;
    bool wifi_started;
    bool ap_started;
    bool ap_config_enabled;
    bool sntp_started;
    bool screen_sleeping;
    bool worker_started;
    bool market_refresh_inflight;
    bool ai_start_inflight;
    bool ai_tts_inflight;
    bool ai_playback_inflight;
    bool ai_tts_stop_requested;
    bool chart_inflight;
    bool market_polled_once;
    bool deep_sleep_armed;
    bool motion_worker_started;
    volatile bool boot_wake_refresh_pending;
    volatile bool boot_config_ap_pending;
    bool pmic_present;
    bool battery_present;
    bool imu_present;
    bool imu_baseline_valid;
    uint32_t ai_task_generation;
    int64_t ai_task_started_us;
    int preferred_wifi_slot;
    int pending_wifi_slot;
    int active_wifi_slot;
    int market_yahoo_next_index;
    int64_t last_market_refresh_us;
    int64_t last_activity_us;
    int64_t screen_sleep_started_us;
    int64_t last_motion_wake_us;
    int64_t last_battery_read_us;
    int64_t market_task_started_us;
    int64_t chart_task_started_us;
    int64_t tts_task_started_us;
    esp_netif_t *sta_netif;
    i2c_master_dev_handle_t pmic_dev;
    i2c_master_dev_handle_t imu_dev;
    httpd_handle_t httpd;
    SemaphoreHandle_t net_lock;
    SemaphoreHandle_t audio_lock;
    esp_codec_dev_handle_t speaker_dev;
    esp_codec_dev_handle_t mic_dev;
    int16_t imu_last_raw[3];
    int battery_percent;
    int battery_mv;

    market_row_t market_rows[MARKET_ROW_COUNT];
    char market_status[64];
    char market_time[16];
    int chart_selected_index;
    int chart_point_count;
    double chart_values[CHART_POINT_COUNT];
    char chart_hint[48];
    char ai_state[48];
    char ai_hint[96];
    char ai_user[192];
    char ai_reply[512];
    char ai_dialog[896];
    ai_profile_store_t ai_profiles;

    lv_obj_t *home_screen;
    lv_obj_t *ai_screen;
    lv_obj_t *chart_screen;
    lv_obj_t *wifi_label;
    lv_obj_t *time_label;
    lv_obj_t *battery_label;
    lv_obj_t *market_time_label;
    lv_obj_t *market_status_label;
    lv_obj_t *chart_title_label;
    lv_obj_t *chart_price_label;
    lv_obj_t *chart_hint_label;
    lv_obj_t *chart_plot_obj;
    lv_obj_t *status_label;
    lv_obj_t *ai_title_label;
    lv_obj_t *ai_state_label;
    lv_obj_t *ai_reply_container;
    lv_obj_t *ai_reply_label;
    lv_obj_t *ai_profile_label;
} app_state_t;

static app_state_t g_app;

static const ai_profile_t DEFAULT_AI_PROFILES[AI_PROFILE_COUNT] = {
    {
        .name = "GPT",
        .server_url = "https://api.openai.com/v1",
        .model = "gpt-4o-mini",
    },
    {
        .name = "Gemini",
        .server_url = "https://generativelanguage.googleapis.com/v1beta/openai",
        .model = "gemini-2.5-flash",
    },
    {
        .name = "豆包",
        .server_url = "https://ark.cn-beijing.volces.com/api/v3",
        .model = "doubao-seed-1-6",
    },
    {
        .name = "DeepSeek",
        .server_url = "https://api.deepseek.com/v1",
        .model = "deepseek-chat",
    },
    {
        .name = "千问",
        .server_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .model = "qwen-plus",
    },
    {
        .name = "自定",
        .server_url = DEFAULT_API_URL,
        .model = DEFAULT_MODEL,
    },
};

static void ui_refresh_now(void);
static void ui_refresh_locked(void);
static void ui_show_home(void);
static void ui_show_ai(void);
static void ui_show_chart(int row_index);
static const char *ui_current_screen_name(void);
static void ui_create_chart(void);
static void configure_timezone_once(void);
static void home_market_refresh_async(void);
static bool direct_ai_start_request(const char *source);
static void chart_refresh_async(int row_index);
static void handle_button_wake_refresh(void);
static void process_pending_button_requests(void);
static void motion_task_start_once(void);
static void boot_button_task_start_once(void);
static esp_err_t market_cache_load(void);
static esp_err_t market_cache_save(void);
static esp_err_t config_save(const watch_lite_config_t *cfg);
static esp_err_t ai_profiles_save(const ai_profile_store_t *store);
static void ai_profile_apply(int index, bool persist);
static void config_sanitize(watch_lite_config_t *cfg);
static bool config_has_wifi(const watch_lite_config_t *cfg);
static void apply_ip_config_for_slot(int slot);
static void apply_dns_config_for_slot(int slot);
static int wifi_slot_at_try_index(int index);
static int wifi_current_connected_slot(void);
static int configured_wifi_slot_count(void);
static esp_err_t wifi_apply_sta_slot(int slot);
static void wifi_select_task(void *arg);
static esp_err_t wifi_sta_connect_once(int timeout_ms);
static void wifi_sta_disconnect_now(void);
static void wifi_sta_disconnect_when_idle(void);
static esp_err_t wifi_apply_power_save_mode(void);
static esp_err_t wifi_configure_ap(void);
static esp_err_t wifi_enable_config_ap(void);
static esp_err_t http_server_start(void);
static esp_err_t pmic_init(void);
static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *value);
static esp_err_t pmic_update_bits(uint8_t reg, uint8_t mask, uint8_t value);
static esp_err_t pmic_read_battery(void);
static void battery_update_once(void);
static void pmic_audio_power_set(bool enabled);
static esp_err_t imu_write_reg(uint8_t reg, uint8_t value);
static void log_wakeup_reason(void);
static void update_ai_dialog_text(void);
static bool ai_task_generation_is_current(uint32_t generation);
static void ai_task_set_state_if_current(uint32_t generation, const char *state);
static void ai_task_set_reply_if_current(uint32_t generation, const char *reply);
static esp_err_t record_voice_wav(binary_buffer_t *wav_out, char *error_out, size_t error_out_len);
static esp_err_t play_wav_audio(uint8_t *data, size_t len, char *error_out, size_t error_out_len);
static bool schedule_tts_reply(const char *reply);
static void voice_tts_task(void *arg);
static esp_err_t http_binary_event_cb(esp_http_client_event_t *event);
static bool extract_error_string_from_json(cJSON *root, char *error_out, size_t error_out_len);
static esp_err_t parse_reply_response(const char *body, char *reply_out, size_t reply_out_len,
                                      char *error_out, size_t error_out_len);
static esp_err_t perform_gateway_voice_request(const uint8_t *wav_data, size_t wav_len,
                                               voice_gateway_result_t *result,
                                               char *error_out, size_t error_out_len);
static esp_err_t fetch_gateway_audio(const char *audio_url, binary_buffer_t *audio_out,
                                     char *error_out, size_t error_out_len);
static void binary_buffer_free(binary_buffer_t *buffer);

static const market_asset_def_t MARKET_DEFS[MARKET_ROW_COUNT] = {
    {"BTC", "bitcoin", "BTC-USD", MARKET_SOURCE_CRYPTO, 0xF7C873},
    {"ETH", "ethereum", "ETH-USD", MARKET_SOURCE_CRYPTO, 0x7AA7FF},
    {"XAU", "xauusd", "GC=F", MARKET_SOURCE_STOOQ, 0xF7D774},
    {"WTI", "cl.f", "CL=F", MARKET_SOURCE_STOOQ, 0xEFA26A},
    {"DXY", "dx.f", "DX-Y.NYB", MARKET_SOURCE_STOOQ, 0xB8C7DD},
    {"CNY", "usdcny", "CNY=X", MARKET_SOURCE_STOOQ, 0xB8C7DD},
    {"EUR", "eurusd", "EURUSD=X", MARKET_SOURCE_STOOQ, 0xB8C7DD},
    {"VIX", "%5EVIX", "%5EVIX", MARKET_SOURCE_YAHOO, 0xFF8A7A},
    {"SHC", "%5Eshc", "000001.SS", MARKET_SOURCE_STOOQ, 0x91D7FF},
    {"HSI", "%5Ehsi", "%5EHSI", MARKET_SOURCE_STOOQ, 0x91D7FF},
    {"N225", "%5Enkx", "%5EN225", MARKET_SOURCE_STOOQ, 0x91D7FF},
    {"DAX", "%5Edax", "%5EGDAXI", MARKET_SOURCE_STOOQ, 0xC7B7FF},
    {"UKX", "%5Eukx", "%5EFTSE", MARKET_SOURCE_STOOQ, 0xC7B7FF},
    {"SPX", "%5Espx", "%5EGSPC", MARKET_SOURCE_STOOQ, 0xC7B7FF},
    {"NDX", "%5Endq", "%5ENDX", MARKET_SOURCE_STOOQ, 0xC7B7FF},
};

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
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

static bool utf8_decode_codepoint(const char *src, uint32_t *codepoint, size_t *char_len)
{
    if (!src || !src[0] || !codepoint || !char_len) {
        return false;
    }

    const unsigned char c0 = (unsigned char)src[0];
    if ((c0 & 0x80u) == 0) {
        *codepoint = c0;
        *char_len = 1;
        return true;
    }

    uint32_t cp = 0;
    size_t len = 0;
    uint32_t min_cp = 0;
    if ((c0 & 0xE0u) == 0xC0u) {
        cp = c0 & 0x1Fu;
        len = 2;
        min_cp = 0x80u;
    } else if ((c0 & 0xF0u) == 0xE0u) {
        cp = c0 & 0x0Fu;
        len = 3;
        min_cp = 0x800u;
    } else if ((c0 & 0xF8u) == 0xF0u) {
        cp = c0 & 0x07u;
        len = 4;
        min_cp = 0x10000u;
    } else {
        return false;
    }

    for (size_t i = 1; i < len; ++i) {
        unsigned char cx = (unsigned char)src[i];
        if ((cx & 0xC0u) != 0x80u) {
            return false;
        }
        cp = (cp << 6) | (uint32_t)(cx & 0x3Fu);
    }

    if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return false;
    }

    *codepoint = cp;
    *char_len = len;
    return true;
}

static bool utf8_append_codepoint(char *dst, size_t dst_size, size_t *di, uint32_t codepoint)
{
    if (!dst || !di || *di >= dst_size) {
        return false;
    }

    unsigned char encoded[4] = {0};
    size_t len = 0;
    if (codepoint <= 0x7Fu) {
        encoded[0] = (unsigned char)codepoint;
        len = 1;
    } else if (codepoint <= 0x7FFu) {
        encoded[0] = (unsigned char)(0xC0u | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        len = 2;
    } else if (codepoint <= 0xFFFFu) {
        encoded[0] = (unsigned char)(0xE0u | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[2] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        len = 3;
    } else if (codepoint <= 0x10FFFFu) {
        encoded[0] = (unsigned char)(0xF0u | (codepoint >> 18));
        encoded[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        encoded[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        encoded[3] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        len = 4;
    } else {
        return false;
    }

    if (*di + len >= dst_size) {
        return false;
    }
    memcpy(dst + *di, encoded, len);
    *di += len;
    dst[*di] = '\0';
    return true;
}

static uint32_t display_normalize_codepoint(uint32_t codepoint)
{
    if (codepoint >= 0xFF10u && codepoint <= 0xFF19u) {
        return '0' + (codepoint - 0xFF10u);
    }
    if (codepoint >= 0xFF21u && codepoint <= 0xFF3Au) {
        return 'A' + (codepoint - 0xFF21u);
    }
    if (codepoint >= 0xFF41u && codepoint <= 0xFF5Au) {
        return 'a' + (codepoint - 0xFF41u);
    }

    switch (codepoint) {
    case '\r':
        return 0;
    case '\t':
    case 0x00A0u:
    case 0x3000u:
        return ' ';
    case 0x200Bu: /* zero-width space */
    case 0x200Cu:
    case 0x200Du:
    case 0x2060u:
    case 0xFEFFu:
    case 0xFE0Eu:
    case 0xFE0Fu:
        return 0;
    case 0x00B0u:
        return 0;
    case 0x00A9u:
    case 0x00AEu:
    case 0x2122u:
        return 0;
    case 0x00B7u:
    case 0x30FBu:
        return 0x00B7u;
    case 0x2022u:
    case 0x25CFu:
    case 0x25A0u:
    case 0x25AAu:
        return 0x2022u;
    case 0x2010u:
    case 0x2011u:
    case 0x2012u:
    case 0x2013u:
    case 0x2014u:
    case 0x2015u:
    case 0x2212u:
    case 0xFF0Du:
        return '-';
    case 0x2018u:
    case 0x2019u:
    case 0x201Au:
    case 0x201Bu:
    case 0x2032u:
    case 0x0060u:
    case 0x00B4u:
        return '\'';
    case 0x201Cu:
    case 0x201Du:
    case 0x201Eu:
    case 0x201Fu:
    case 0x2033u:
        return '"';
    case 0x2026u:
    case 0x22EFu:
        return 0x2026u;
    case 0x300Cu:
    case 0x300Eu:
        return 0x201Cu;
    case 0x300Du:
    case 0x300Fu:
        return 0x201Du;
    case 0x3014u:
    case 0x3016u:
    case 0x301Au:
    case 0xFF3Bu:
        return 0x3010u;
    case 0x3015u:
    case 0x3017u:
    case 0x301Bu:
    case 0xFF3Du:
        return 0x3011u;
    case 0x3008u:
    case 0x300Au:
    case 0xFE64u:
        return '<';
    case 0x3009u:
    case 0x300Bu:
    case 0xFE65u:
        return '>';
    case 0x301Cu:
    case 0xFF5Eu:
    case 0x223Cu:
        return '-';
    case 0xFF01u:
        return 0xFF01u;
    case 0xFF08u:
        return 0xFF08u;
    case 0xFF09u:
        return 0xFF09u;
    case 0xFF0Cu:
        return 0xFF0Cu;
    case 0xFF0Eu:
        return '.';
    case 0xFF1Au:
        return 0xFF1Au;
    case 0xFF1Bu:
        return 0xFF1Bu;
    case 0xFF1Fu:
        return 0xFF1Fu;
    case 0xFF05u:
        return 0xFF05u;
    case 0xFF0Bu:
        return 0xFF0Bu;
    case 0x00A5u:
        return 0xFFE5u;
    case 0xFF04u:
        return '$';
    case 0x2705u:
    case 0x2611u:
    case 0x2714u:
        return 0x2713u;
    case 0x274Cu:
    case 0x2716u:
    case 0x2718u:
        return 0x2715u;
    case 0x2190u:
    case 0x21D0u:
        return '<';
    case 0x2192u:
    case 0x21D2u:
        return '>';
    case 0x2191u:
        return 0x2191u;
    case 0x2193u:
        return 0x2193u;
    case 0x2194u:
    case 0x21D4u:
        return '-';
    case 0x20ACu:
        return 'E';
    case 0x00A3u:
        return 'L';
    default:
        return codepoint;
    }
}

static bool display_codepoint_is_cjk(uint32_t codepoint)
{
    return (codepoint >= 0x3400u && codepoint <= 0x9FFFu) ||
           (codepoint >= 0xF900u && codepoint <= 0xFAFFu);
}

static bool ai_dialog_font_has_glyph(uint32_t codepoint)
{
    if (codepoint == '\n' || codepoint == ' ') {
        return true;
    }
    if (codepoint < 0x20u) {
        return false;
    }
    if (codepoint >= 0x1F000u) {
        return false;
    }

    lv_font_glyph_dsc_t glyph = {0};
    if (!lv_font_get_glyph_dsc(AI_TEXT_FILTER_FONT, &glyph, codepoint, 0)) {
        return false;
    }
    return !glyph.is_placeholder;
}

static size_t display_safe_text_copy(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return 0;
    }
    dst[0] = '\0';
    if (!src) {
        return 0;
    }

    if (!s_ai_text_filter_enabled) {
        return utf8_safe_copy(src, dst, dst_len);
    }

    size_t di = 0;
    for (size_t si = 0; src[si] != '\0';) {
        uint32_t cp = 0;
        size_t char_len = 0;
        if (!utf8_decode_codepoint(src + si, &cp, &char_len)) {
            cp = '?';
            char_len = 1;
        }
        si += char_len;

        cp = display_normalize_codepoint(cp);
        if (cp == 0) {
            continue;
        }

        if (!ai_dialog_font_has_glyph(cp)) {
            if (display_codepoint_is_cjk(cp)) {
                cp = '?';
            } else {
                continue;
            }
        }
        if (!utf8_append_codepoint(dst, dst_len, &di, cp)) {
            break;
        }
    }
    return di;
}

static void copy_ai_text(char *dst, size_t dst_size, const char *src)
{
    (void)display_safe_text_copy(src, dst, dst_size);
}

static bool text_has_non_ascii(const char *text)
{
    if (!text) {
        return false;
    }
    while (*text) {
        if ((unsigned char)*text >= 0x80u) {
            return true;
        }
        text++;
    }
    return false;
}

static bool stt_text_is_empty_or_prompt_hallucination(const char *text)
{
    if (!text) {
        return true;
    }
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    if (*text == '\0') {
        return true;
    }

    return strstr(text, "语音转写") ||
           strstr(text, "只输出用户说") ||
           strstr(text, "不要翻译") ||
           strstr(text, "中文普通话");
}

static char *config_wifi_ssid(watch_lite_config_t *cfg, int slot)
{
    if (!cfg) {
        return NULL;
    }
    switch (slot) {
    case 0:
        return cfg->wifi_ssid;
    case 1:
        return cfg->wifi_ssid2;
    case 2:
        return cfg->wifi_ssid3;
    case 3:
        return cfg->wifi_ssid4;
    case 4:
        return cfg->wifi_ssid5;
    default:
        return NULL;
    }
}

static char *config_wifi_password(watch_lite_config_t *cfg, int slot)
{
    if (!cfg) {
        return NULL;
    }
    switch (slot) {
    case 0:
        return cfg->wifi_password;
    case 1:
        return cfg->wifi_password2;
    case 2:
        return cfg->wifi_password3;
    case 3:
        return cfg->wifi_password4;
    case 4:
        return cfg->wifi_password5;
    default:
        return NULL;
    }
}

static const char *config_wifi_ssid_const(const watch_lite_config_t *cfg, int slot)
{
    return config_wifi_ssid((watch_lite_config_t *)cfg, slot);
}

static const char *config_wifi_password_const(const watch_lite_config_t *cfg, int slot)
{
    return config_wifi_password((watch_lite_config_t *)cfg, slot);
}

static wifi_ip_config_t *config_wifi_net(watch_lite_config_t *cfg, int slot)
{
    if (!cfg || slot < 0 || slot >= WIFI_SLOT_COUNT) {
        return NULL;
    }
    return &cfg->wifi_net[slot];
}

static const wifi_ip_config_t *config_wifi_net_const(const watch_lite_config_t *cfg, int slot)
{
    return config_wifi_net((watch_lite_config_t *)cfg, slot);
}

static bool config_wifi_slot_used(const watch_lite_config_t *cfg, int slot)
{
    const char *ssid = config_wifi_ssid_const(cfg, slot);
    return ssid && ssid[0] != '\0';
}

static bool config_has_wifi(const watch_lite_config_t *cfg)
{
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        if (config_wifi_slot_used(cfg, i)) {
            return true;
        }
    }
    return false;
}

static void ai_profiles_set_defaults(ai_profile_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->magic = AI_PROFILE_MAGIC;
    store->version = AI_PROFILE_VERSION;
    store->active_index = AI_PROFILE_CUSTOM_INDEX;
    for (int i = 0; i < AI_PROFILE_COUNT; i++) {
        copy_string(store->profiles[i].name, sizeof(store->profiles[i].name),
                    DEFAULT_AI_PROFILES[i].name);
        copy_string(store->profiles[i].server_url, sizeof(store->profiles[i].server_url),
                    DEFAULT_AI_PROFILES[i].server_url);
        copy_string(store->profiles[i].model, sizeof(store->profiles[i].model),
                    DEFAULT_AI_PROFILES[i].model);
    }
}

static void ai_profiles_import_current_config(ai_profile_store_t *store,
                                              const watch_lite_config_t *cfg)
{
    if (!store || !cfg) {
        return;
    }
    ai_profile_t *custom = &store->profiles[AI_PROFILE_CUSTOM_INDEX];
    copy_string(custom->server_url, sizeof(custom->server_url), cfg->server_url);
    copy_string(custom->api_key, sizeof(custom->api_key), cfg->api_key);
    copy_string(custom->model, sizeof(custom->model), cfg->model);
}

static void ai_profiles_sanitize(ai_profile_store_t *store)
{
    if (!store || store->magic != AI_PROFILE_MAGIC || store->version != AI_PROFILE_VERSION) {
        return;
    }
    if (store->active_index < 0 || store->active_index >= AI_PROFILE_COUNT) {
        store->active_index = AI_PROFILE_CUSTOM_INDEX;
    }
    for (int i = 0; i < AI_PROFILE_COUNT; i++) {
        ai_profile_t *profile = &store->profiles[i];
        profile->name[sizeof(profile->name) - 1] = '\0';
        profile->server_url[sizeof(profile->server_url) - 1] = '\0';
        profile->api_key[sizeof(profile->api_key) - 1] = '\0';
        profile->model[sizeof(profile->model) - 1] = '\0';
        if (profile->name[0] == '\0') {
            copy_string(profile->name, sizeof(profile->name), DEFAULT_AI_PROFILES[i].name);
        }
        if (profile->server_url[0] == '\0') {
            copy_string(profile->server_url, sizeof(profile->server_url),
                        DEFAULT_AI_PROFILES[i].server_url);
        }
        if (profile->model[0] == '\0') {
            copy_string(profile->model, sizeof(profile->model),
                        DEFAULT_AI_PROFILES[i].model);
        }
    }
}

static esp_err_t ai_profiles_load(ai_profile_store_t *store, const watch_lite_config_t *cfg)
{
    ai_profiles_set_defaults(store);
    ai_profiles_import_current_config(store, cfg);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ai profiles load: open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t size = 0;
    err = nvs_get_blob(handle, "ai_profiles", NULL, &size);
    if (err == ESP_OK && size == sizeof(*store)) {
        err = nvs_get_blob(handle, "ai_profiles", store, &size);
        if (err == ESP_OK && store->magic == AI_PROFILE_MAGIC &&
            store->version == AI_PROFILE_VERSION) {
            ai_profiles_sanitize(store);
            nvs_close(handle);
            ESP_LOGI(TAG, "ai profiles load: stored active=%d", store->active_index);
            return ESP_OK;
        }
    }
    nvs_close(handle);

    ai_profiles_set_defaults(store);
    ai_profiles_import_current_config(store, cfg);
    ESP_LOGI(TAG, "ai profiles load: defaults err=%s size=%u",
             esp_err_to_name(err), (unsigned)size);
    (void)ai_profiles_save(store);
    return err;
}

static esp_err_t ai_profiles_save(const ai_profile_store_t *store)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, "ai_profiles", store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static const ai_profile_t *ai_profile_active(void)
{
    int index = g_app.ai_profiles.active_index;
    if (index < 0 || index >= AI_PROFILE_COUNT) {
        index = AI_PROFILE_CUSTOM_INDEX;
    }
    return &g_app.ai_profiles.profiles[index];
}

static void ai_profile_apply(int index, bool persist)
{
    if (index < 0 || index >= AI_PROFILE_COUNT) {
        index = AI_PROFILE_CUSTOM_INDEX;
    }
    g_app.ai_profiles.active_index = index;
    const ai_profile_t *profile = ai_profile_active();
    copy_string(g_app.cfg.server_url, sizeof(g_app.cfg.server_url), profile->server_url);
    copy_string(g_app.cfg.api_key, sizeof(g_app.cfg.api_key), profile->api_key);
    copy_string(g_app.cfg.model, sizeof(g_app.cfg.model), profile->model);
    config_sanitize(&g_app.cfg);
    if (persist) {
        (void)ai_profiles_save(&g_app.ai_profiles);
        (void)config_save(&g_app.cfg);
    }
}

static void config_set_defaults(watch_lite_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    copy_string(cfg->server_url, sizeof(cfg->server_url), DEFAULT_API_URL);
    copy_string(cfg->model, sizeof(cfg->model), DEFAULT_MODEL);
    copy_string(cfg->market_provider, sizeof(cfg->market_provider), DEFAULT_MARKET_PROVIDER);
    copy_string(cfg->starter_prompt, sizeof(cfg->starter_prompt), DEFAULT_STARTER_PROMPT);
    cfg->auto_sleep_s = DEFAULT_AUTO_SLEEP_S;
    cfg->brightness_percent = DEFAULT_BRIGHTNESS_PERCENT;
    cfg->motion_wake_enabled = DEFAULT_MOTION_WAKE_ENABLED;
    cfg->motion_wake_threshold_mg = DEFAULT_MOTION_WAKE_THRESHOLD_MG;
}

static void config_copy_common_from_v4(watch_lite_config_t *cfg, const watch_lite_config_v4_t *old)
{
    if (!cfg || !old) {
        return;
    }
    copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), old->wifi_ssid);
    copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), old->wifi_password);
    copy_string(cfg->server_url, sizeof(cfg->server_url), old->server_url);
    copy_string(cfg->api_key, sizeof(cfg->api_key), old->api_key);
    copy_string(cfg->model, sizeof(cfg->model), old->model);
    copy_string(cfg->market_provider, sizeof(cfg->market_provider), old->market_provider);
    copy_string(cfg->starter_prompt, sizeof(cfg->starter_prompt), old->starter_prompt);
    copy_string(cfg->static_ip, sizeof(cfg->static_ip), old->static_ip);
    copy_string(cfg->static_netmask, sizeof(cfg->static_netmask), old->static_netmask);
    copy_string(cfg->static_gateway, sizeof(cfg->static_gateway), old->static_gateway);
    copy_string(cfg->dns1, sizeof(cfg->dns1), old->dns1);
    copy_string(cfg->dns2, sizeof(cfg->dns2), old->dns2);
    copy_string(cfg->proxy_host, sizeof(cfg->proxy_host), old->proxy_host);
    cfg->proxy_port = old->proxy_port;
    cfg->auto_sleep_s = old->auto_sleep_s;
    cfg->brightness_percent = old->brightness_percent;
    cfg->motion_wake_enabled = old->motion_wake_enabled;
    cfg->motion_wake_threshold_mg = old->motion_wake_threshold_mg;
    copy_string(cfg->wifi_ssid2, sizeof(cfg->wifi_ssid2), old->wifi_ssid2);
    copy_string(cfg->wifi_password2, sizeof(cfg->wifi_password2), old->wifi_password2);
    copy_string(cfg->wifi_ssid3, sizeof(cfg->wifi_ssid3), old->wifi_ssid3);
    copy_string(cfg->wifi_password3, sizeof(cfg->wifi_password3), old->wifi_password3);
    copy_string(cfg->wifi_ssid4, sizeof(cfg->wifi_ssid4), old->wifi_ssid4);
    copy_string(cfg->wifi_password4, sizeof(cfg->wifi_password4), old->wifi_password4);
    copy_string(cfg->wifi_ssid5, sizeof(cfg->wifi_ssid5), old->wifi_ssid5);
    copy_string(cfg->wifi_password5, sizeof(cfg->wifi_password5), old->wifi_password5);
    copy_string(cfg->wifi_net[0].static_ip, sizeof(cfg->wifi_net[0].static_ip), old->static_ip);
    copy_string(cfg->wifi_net[0].static_netmask, sizeof(cfg->wifi_net[0].static_netmask), old->static_netmask);
    copy_string(cfg->wifi_net[0].static_gateway, sizeof(cfg->wifi_net[0].static_gateway), old->static_gateway);
    copy_string(cfg->wifi_net[0].dns1, sizeof(cfg->wifi_net[0].dns1), old->dns1);
    copy_string(cfg->wifi_net[0].dns2, sizeof(cfg->wifi_net[0].dns2), old->dns2);
}

static bool config_migrate_from_v4_blob(watch_lite_config_t *cfg, const watch_lite_config_v4_t *old)
{
    if (!cfg || !old || old->magic != CONFIG_MAGIC) {
        return false;
    }

    config_set_defaults(cfg);
    config_copy_common_from_v4(cfg, old);
    cfg->magic = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    config_sanitize(cfg);
    return true;
}

static void config_copy_prefix_string(char *dst, size_t dst_size, const uint8_t *blob,
                                      size_t blob_size, size_t offset, size_t src_size)
{
    if (!dst || dst_size == 0 || !blob || offset >= blob_size) {
        return;
    }
    size_t available = blob_size - offset;
    size_t copy_len = MIN(MIN(dst_size - 1, src_size), available);
    memcpy(dst, blob + offset, copy_len);
    dst[copy_len] = '\0';
}

static bool config_migrate_from_prefix_blob(watch_lite_config_t *cfg, const uint8_t *blob,
                                            size_t blob_size)
{
    if (!cfg || !blob || blob_size < offsetof(watch_lite_config_prefix_t, wifi_password)) {
        return false;
    }

    const watch_lite_config_prefix_t *old = (const watch_lite_config_prefix_t *)blob;
    if (old->magic != CONFIG_MAGIC) {
        return false;
    }

    config_set_defaults(cfg);
    config_copy_prefix_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, wifi_ssid),
                              sizeof(old->wifi_ssid));
    config_copy_prefix_string(cfg->wifi_password, sizeof(cfg->wifi_password), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, wifi_password),
                              sizeof(old->wifi_password));
    config_copy_prefix_string(cfg->server_url, sizeof(cfg->server_url), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, server_url),
                              sizeof(old->server_url));
    config_copy_prefix_string(cfg->api_key, sizeof(cfg->api_key), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, api_key),
                              sizeof(old->api_key));
    config_copy_prefix_string(cfg->model, sizeof(cfg->model), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, model),
                              sizeof(old->model));
    config_copy_prefix_string(cfg->market_provider, sizeof(cfg->market_provider), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, market_provider),
                              sizeof(old->market_provider));
    config_copy_prefix_string(cfg->starter_prompt, sizeof(cfg->starter_prompt), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, starter_prompt),
                              sizeof(old->starter_prompt));
    config_copy_prefix_string(cfg->static_ip, sizeof(cfg->static_ip), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, static_ip),
                              sizeof(old->static_ip));
    config_copy_prefix_string(cfg->static_netmask, sizeof(cfg->static_netmask), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, static_netmask),
                              sizeof(old->static_netmask));
    config_copy_prefix_string(cfg->static_gateway, sizeof(cfg->static_gateway), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, static_gateway),
                              sizeof(old->static_gateway));
    config_copy_prefix_string(cfg->dns1, sizeof(cfg->dns1), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, dns1),
                              sizeof(old->dns1));
    config_copy_prefix_string(cfg->dns2, sizeof(cfg->dns2), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, dns2),
                              sizeof(old->dns2));
    config_copy_prefix_string(cfg->proxy_host, sizeof(cfg->proxy_host), blob, blob_size,
                              offsetof(watch_lite_config_prefix_t, proxy_host),
                              sizeof(old->proxy_host));

    if (blob_size >= offsetof(watch_lite_config_prefix_t, proxy_port) + sizeof(old->proxy_port)) {
        memcpy(&cfg->proxy_port, blob + offsetof(watch_lite_config_prefix_t, proxy_port),
               sizeof(cfg->proxy_port));
    }
    if (blob_size >= offsetof(watch_lite_config_prefix_t, auto_sleep_s) + sizeof(old->auto_sleep_s)) {
        memcpy(&cfg->auto_sleep_s, blob + offsetof(watch_lite_config_prefix_t, auto_sleep_s),
               sizeof(cfg->auto_sleep_s));
    }
    copy_string(cfg->wifi_net[0].static_ip, sizeof(cfg->wifi_net[0].static_ip), cfg->static_ip);
    copy_string(cfg->wifi_net[0].static_netmask, sizeof(cfg->wifi_net[0].static_netmask),
                cfg->static_netmask);
    copy_string(cfg->wifi_net[0].static_gateway, sizeof(cfg->wifi_net[0].static_gateway),
                cfg->static_gateway);
    copy_string(cfg->wifi_net[0].dns1, sizeof(cfg->wifi_net[0].dns1), cfg->dns1);
    copy_string(cfg->wifi_net[0].dns2, sizeof(cfg->wifi_net[0].dns2), cfg->dns2);
    cfg->magic = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    config_sanitize(cfg);
    return true;
}

static void config_sanitize(watch_lite_config_t *cfg)
{
    if (cfg->magic != CONFIG_MAGIC || cfg->version != CONFIG_VERSION) {
        config_set_defaults(cfg);
        return;
    }

    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        char *ssid = config_wifi_ssid(cfg, i);
        char *password = config_wifi_password(cfg, i);
        if (ssid) {
            ssid[32] = '\0';
        }
        if (password) {
            password[64] = '\0';
        }
        wifi_ip_config_t *net = config_wifi_net(cfg, i);
        if (net) {
            net->static_ip[sizeof(net->static_ip) - 1] = '\0';
            net->static_netmask[sizeof(net->static_netmask) - 1] = '\0';
            net->static_gateway[sizeof(net->static_gateway) - 1] = '\0';
            net->dns1[sizeof(net->dns1) - 1] = '\0';
            net->dns2[sizeof(net->dns2) - 1] = '\0';
        }
    }
    cfg->server_url[sizeof(cfg->server_url) - 1] = '\0';
    cfg->api_key[sizeof(cfg->api_key) - 1] = '\0';
    cfg->model[sizeof(cfg->model) - 1] = '\0';
    cfg->market_provider[sizeof(cfg->market_provider) - 1] = '\0';
    cfg->starter_prompt[sizeof(cfg->starter_prompt) - 1] = '\0';
    cfg->static_ip[sizeof(cfg->static_ip) - 1] = '\0';
    cfg->static_netmask[sizeof(cfg->static_netmask) - 1] = '\0';
    cfg->static_gateway[sizeof(cfg->static_gateway) - 1] = '\0';
    cfg->dns1[sizeof(cfg->dns1) - 1] = '\0';
    cfg->dns2[sizeof(cfg->dns2) - 1] = '\0';
    cfg->proxy_host[sizeof(cfg->proxy_host) - 1] = '\0';

    if (cfg->server_url[0] == '\0') {
        copy_string(cfg->server_url, sizeof(cfg->server_url), DEFAULT_API_URL);
    }
    if (strcmp(cfg->server_url, DEFAULT_VOICE_GATEWAY_URL) == 0) {
        copy_string(cfg->server_url, sizeof(cfg->server_url), DEFAULT_API_URL);
    }
    if (cfg->model[0] == '\0') {
        copy_string(cfg->model, sizeof(cfg->model), DEFAULT_MODEL);
    }
    if (cfg->market_provider[0] == '\0') {
        copy_string(cfg->market_provider, sizeof(cfg->market_provider), DEFAULT_MARKET_PROVIDER);
    }
    if (cfg->starter_prompt[0] == '\0') {
        copy_string(cfg->starter_prompt, sizeof(cfg->starter_prompt), DEFAULT_STARTER_PROMPT);
    }
    if (cfg->auto_sleep_s < 0 || cfg->auto_sleep_s > MAX_AUTO_SLEEP_S) {
        cfg->auto_sleep_s = DEFAULT_AUTO_SLEEP_S;
    }
    if (cfg->proxy_port < 0 || cfg->proxy_port > 65535) {
        cfg->proxy_port = 0;
    }
    if (cfg->brightness_percent < 5 || cfg->brightness_percent > 100) {
        cfg->brightness_percent = DEFAULT_BRIGHTNESS_PERCENT;
    }
    cfg->motion_wake_enabled = 0;
    if (cfg->motion_wake_threshold_mg < MIN_MOTION_WAKE_THRESHOLD_MG ||
        cfg->motion_wake_threshold_mg > MAX_MOTION_WAKE_THRESHOLD_MG) {
        cfg->motion_wake_threshold_mg = DEFAULT_MOTION_WAKE_THRESHOLD_MG;
    }
}

static esp_err_t config_load(watch_lite_config_t *cfg)
{
    config_set_defaults(cfg);
    bool save_after_load = false;
    const char *source = "defaults";

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config load: open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t size = 0;
    err = nvs_get_blob(handle, "config", NULL, &size);
    if (err == ESP_OK && size == sizeof(*cfg)) {
        err = nvs_get_blob(handle, "config", cfg, &size);
        if (err == ESP_OK && cfg->magic == CONFIG_MAGIC) {
            if (cfg->version != CONFIG_VERSION) {
                ESP_LOGW(TAG, "config load: same-size old version=%lu, keeping fields",
                         (unsigned long)cfg->version);
                cfg->version = CONFIG_VERSION;
                save_after_load = true;
            }
            config_sanitize(cfg);
            source = "v5";
        } else {
            config_set_defaults(cfg);
            source = "bad-v5";
        }
    } else if (err == ESP_OK && size == sizeof(watch_lite_config_v4_t)) {
        watch_lite_config_v4_t old = {0};
        size = sizeof(old);
        err = nvs_get_blob(handle, "config", &old, &size);
        if (err == ESP_OK && config_migrate_from_v4_blob(cfg, &old)) {
            save_after_load = true;
            source = "v4-migrated";
        } else {
            config_set_defaults(cfg);
            source = "bad-v4";
        }
    } else if (err == ESP_OK && size == sizeof(watch_lite_config_v1_t)) {
        watch_lite_config_v1_t old = {0};
        size = sizeof(old);
        err = nvs_get_blob(handle, "config", &old, &size);
        config_set_defaults(cfg);
        if (err == ESP_OK && old.magic == CONFIG_MAGIC && old.version == 1u) {
            copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), old.wifi_ssid);
            copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), old.wifi_password);
            cfg->auto_sleep_s = old.auto_sleep_s;
            config_sanitize(cfg);
            save_after_load = true;
            source = "v1-migrated";
        } else {
            source = "bad-v1";
        }
    } else if (err == ESP_OK && size == sizeof(watch_lite_config_v3_t)) {
        watch_lite_config_v3_t old = {0};
        size = sizeof(old);
        err = nvs_get_blob(handle, "config", &old, &size);
        config_set_defaults(cfg);
        if (err == ESP_OK && old.magic == CONFIG_MAGIC && old.version == 3u) {
            copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), old.wifi_ssid);
            copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), old.wifi_password);
            copy_string(cfg->server_url, sizeof(cfg->server_url), old.server_url);
            copy_string(cfg->api_key, sizeof(cfg->api_key), old.api_key);
            copy_string(cfg->model, sizeof(cfg->model), old.model);
            copy_string(cfg->market_provider, sizeof(cfg->market_provider), old.market_provider);
            copy_string(cfg->starter_prompt, sizeof(cfg->starter_prompt), old.starter_prompt);
            copy_string(cfg->static_ip, sizeof(cfg->static_ip), old.static_ip);
            copy_string(cfg->static_netmask, sizeof(cfg->static_netmask), old.static_netmask);
            copy_string(cfg->static_gateway, sizeof(cfg->static_gateway), old.static_gateway);
            copy_string(cfg->dns1, sizeof(cfg->dns1), old.dns1);
            copy_string(cfg->dns2, sizeof(cfg->dns2), old.dns2);
            copy_string(cfg->proxy_host, sizeof(cfg->proxy_host), old.proxy_host);
            cfg->proxy_port = old.proxy_port;
            cfg->auto_sleep_s = old.auto_sleep_s;
            config_sanitize(cfg);
            save_after_load = true;
            source = "v3-migrated";
        } else {
            source = "bad-v3";
        }
    } else if (err == ESP_OK && size == sizeof(watch_lite_config_v3_full_t)) {
        watch_lite_config_v3_full_t old = {0};
        size = sizeof(old);
        err = nvs_get_blob(handle, "config", &old, &size);
        config_set_defaults(cfg);
        if (err == ESP_OK && old.magic == CONFIG_MAGIC && old.version == 3u) {
            copy_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), old.wifi_ssid);
            copy_string(cfg->wifi_password, sizeof(cfg->wifi_password), old.wifi_password);
            copy_string(cfg->server_url, sizeof(cfg->server_url), old.server_url);
            copy_string(cfg->api_key, sizeof(cfg->api_key), old.api_key);
            copy_string(cfg->model, sizeof(cfg->model), old.model);
            copy_string(cfg->market_provider, sizeof(cfg->market_provider), old.market_provider);
            copy_string(cfg->starter_prompt, sizeof(cfg->starter_prompt), old.starter_prompt);
            copy_string(cfg->static_ip, sizeof(cfg->static_ip), old.static_ip);
            copy_string(cfg->static_netmask, sizeof(cfg->static_netmask), old.static_netmask);
            copy_string(cfg->static_gateway, sizeof(cfg->static_gateway), old.static_gateway);
            copy_string(cfg->dns1, sizeof(cfg->dns1), old.dns1);
            copy_string(cfg->dns2, sizeof(cfg->dns2), old.dns2);
            copy_string(cfg->proxy_host, sizeof(cfg->proxy_host), old.proxy_host);
            cfg->proxy_port = old.proxy_port;
            cfg->auto_sleep_s = old.auto_sleep_s;
            cfg->brightness_percent = old.brightness_percent;
            cfg->motion_wake_enabled = old.motion_wake_enabled;
            cfg->motion_wake_threshold_mg = old.motion_wake_threshold_mg;
            config_sanitize(cfg);
            save_after_load = true;
            source = "v3-full-migrated";
        } else {
            source = "bad-v3-full";
        }
    } else {
        if (err == ESP_OK && size >= offsetof(watch_lite_config_prefix_t, wifi_password)) {
            uint8_t *raw = calloc(1, size);
            if (raw) {
                size_t raw_size = size;
                esp_err_t raw_err = nvs_get_blob(handle, "config", raw, &raw_size);
                if (raw_err == ESP_OK && config_migrate_from_prefix_blob(cfg, raw, raw_size)) {
                    save_after_load = true;
                    source = "prefix-migrated";
                    ESP_LOGW(TAG, "config load: recovered unknown blob size=%u", (unsigned)raw_size);
                } else {
                    ESP_LOGW(TAG, "config load: unknown blob rejected err=%s size=%u",
                             esp_err_to_name(raw_err), (unsigned)raw_size);
                    config_set_defaults(cfg);
                    source = "unknown-defaults";
                }
                free(raw);
            } else {
                ESP_LOGW(TAG, "config load: no memory for unknown blob size=%u", (unsigned)size);
                config_set_defaults(cfg);
                source = "unknown-no-mem";
            }
        } else {
            ESP_LOGW(TAG, "config load: no compatible blob err=%s size=%u",
                     esp_err_to_name(err), (unsigned)size);
            config_set_defaults(cfg);
        }
    }
    nvs_close(handle);

    if (save_after_load) {
        esp_err_t save_err = config_save(cfg);
        ESP_LOGI(TAG, "config load: migrated source=%s save=%s",
                 source, esp_err_to_name(save_err));
    } else {
        ESP_LOGI(TAG, "config load: source=%s err=%s size=%u",
                 source, esp_err_to_name(err), (unsigned)size);
    }
    return err;
}

static esp_err_t config_save(const watch_lite_config_t *cfg)
{
    nvs_handle_t handle = 0;
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

static int wifi_preferred_slot_load(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return -1;
    }
    int32_t slot = -1;
    err = nvs_get_i32(handle, "wifi_pref_slot", &slot);
    nvs_close(handle);
    if (err != ESP_OK || slot < 0 || slot >= WIFI_SLOT_COUNT) {
        return -1;
    }
    return (int)slot;
}

static void log_config_summary(void)
{
    const ai_profile_t *profile = ai_profile_active();
    ESP_LOGI(TAG, "config summary: wifi_slots=%d preferred=%d auto_sleep=%d ai=%s api=%s model=%s",
             configured_wifi_slot_count(), g_app.preferred_wifi_slot,
             g_app.cfg.auto_sleep_s, profile->name,
             g_app.cfg.api_key[0] ? "set" : "empty",
             g_app.cfg.model);
    for (int slot = 0; slot < WIFI_SLOT_COUNT; slot++) {
        const char *ssid = config_wifi_ssid_const(&g_app.cfg, slot);
        if (ssid && ssid[0]) {
            ESP_LOGI(TAG, "config wifi slot %d ssid=%s", slot + 1, ssid);
        }
    }
}

static esp_err_t wifi_preferred_slot_save(int slot)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (slot >= 0 && slot < WIFI_SLOT_COUNT) {
        err = nvs_set_i32(handle, "wifi_pref_slot", slot);
    } else {
        err = nvs_erase_key(handle, "wifi_pref_slot");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t market_cache_load(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    market_cache_t cache = {0};
    size_t size = sizeof(cache);
    err = nvs_get_blob(handle, "market_cache", &cache, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(cache) ||
        cache.magic != MARKET_CACHE_MAGIC ||
        cache.version != MARKET_CACHE_VERSION ||
        cache.row_count != MARKET_ROW_COUNT) {
        return ESP_ERR_INVALID_VERSION;
    }

    for (int i = 0; i < MARKET_ROW_COUNT; i++) {
        copy_string(g_app.market_rows[i].price, sizeof(g_app.market_rows[i].price), cache.price[i]);
        copy_string(g_app.market_rows[i].change, sizeof(g_app.market_rows[i].change), cache.change[i]);
    }
    copy_string(g_app.market_time, sizeof(g_app.market_time), cache.market_time);
    g_app.market_polled_once = true;
    copy_string(g_app.market_status, sizeof(g_app.market_status), "CACHE");
    return ESP_OK;
}

static esp_err_t market_cache_save(void)
{
    market_cache_t cache = {
        .magic = MARKET_CACHE_MAGIC,
        .version = MARKET_CACHE_VERSION,
        .row_count = MARKET_ROW_COUNT,
    };
    copy_string(cache.market_time, sizeof(cache.market_time), g_app.market_time);
    for (int i = 0; i < MARKET_ROW_COUNT; i++) {
        copy_string(cache.price[i], sizeof(cache.price[i]), g_app.market_rows[i].price);
        copy_string(cache.change[i], sizeof(cache.change[i]), g_app.market_rows[i].change);
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, "market_cache", &cache, sizeof(cache));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void make_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(g_app.ap_ssid, sizeof(g_app.ap_ssid), "WatchLite-%02X%02X", mac[4], mac[5]);
}

static void app_state_set_defaults(void)
{
    g_app.preferred_wifi_slot = wifi_preferred_slot_load();
    g_app.pending_wifi_slot = -1;
    g_app.active_wifi_slot = -1;
    for (int i = 0; i < MARKET_ROW_COUNT; i++) {
        copy_string(g_app.market_rows[i].price, sizeof(g_app.market_rows[i].price), "--");
        copy_string(g_app.market_rows[i].change, sizeof(g_app.market_rows[i].change), "--");
    }
    copy_string(g_app.market_status, sizeof(g_app.market_status), "IDLE");
    copy_string(g_app.market_time, sizeof(g_app.market_time), "--:--");
    g_app.chart_selected_index = -1;
    g_app.chart_point_count = 0;
    copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "Tap row");
    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
    copy_string(g_app.ai_hint, sizeof(g_app.ai_hint), "");
    copy_string(g_app.ai_user, sizeof(g_app.ai_user), "");
    copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "中文显示正常\n点说话开始");
    update_ai_dialog_text();
    g_app.battery_percent = -1;
    g_app.battery_mv = 0;
}

static void restore_display_brightness(void)
{
    int brightness = g_app.cfg.brightness_percent;
    if (brightness < 5 || brightness > 100) {
        brightness = DEFAULT_BRIGHTNESS_PERCENT;
    }
    (void)bsp_display_brightness_set(brightness);
}

static bool elapsed_ms_exceeded(int64_t started_us, int timeout_ms)
{
    if (started_us <= 0 || timeout_ms <= 0) {
        return false;
    }
    return (esp_timer_get_time() - started_us) > ((int64_t)timeout_ms * 1000LL);
}

static void force_clear_stale_busy_flags(void)
{
    if (g_app.market_refresh_inflight &&
        elapsed_ms_exceeded(g_app.market_task_started_us, MARKET_BUSY_STALE_MS)) {
        ESP_LOGW(TAG, "sleep: clearing stale market task");
        g_app.market_refresh_inflight = false;
        g_app.market_task_started_us = 0;
    }
    if (g_app.chart_inflight &&
        elapsed_ms_exceeded(g_app.chart_task_started_us, CHART_BUSY_STALE_MS)) {
        ESP_LOGW(TAG, "sleep: clearing stale chart task");
        g_app.chart_inflight = false;
        g_app.chart_task_started_us = 0;
    }
    if (g_app.ai_start_inflight &&
        elapsed_ms_exceeded(g_app.ai_task_started_us, BUSY_SLEEP_FORCE_RESET_MS)) {
        ESP_LOGW(TAG, "sleep: clearing stale voice task");
        g_app.ai_task_generation++;
        g_app.ai_start_inflight = false;
        g_app.ai_playback_inflight = false;
        g_app.ai_tts_stop_requested = true;
        g_app.ai_task_started_us = 0;
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
    }
    if (g_app.ai_tts_inflight &&
        elapsed_ms_exceeded(g_app.tts_task_started_us, TTS_BUSY_STALE_MS)) {
        ESP_LOGW(TAG, "sleep: clearing stale tts task");
        g_app.ai_tts_inflight = false;
        g_app.ai_playback_inflight = false;
        g_app.ai_tts_stop_requested = true;
        g_app.tts_task_started_us = 0;
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
    }
}

static void enter_low_power_sleep(void)
{
    (void)bsp_display_backlight_off();
    wifi_sta_disconnect_when_idle();
    gpio_set_level(BSP_POWER_AMP_IO, 0);
    ESP_LOGI(TAG, "sleep: screen off, WiFi STA disconnected, config AP=%d", g_app.ap_config_enabled);
}

static bool app_busy_for_sleep(void)
{
    force_clear_stale_busy_flags();
    return g_app.market_refresh_inflight ||
           g_app.chart_inflight ||
           g_app.ai_start_inflight ||
           g_app.ai_tts_inflight ||
           g_app.ai_playback_inflight;
}

static void audio_shutdown_for_sleep(void)
{
    bool lock_taken = false;
    if (g_app.audio_lock) {
        lock_taken = xSemaphoreTake(g_app.audio_lock, pdMS_TO_TICKS(200)) == pdTRUE;
    }
    if (lock_taken && g_app.mic_dev) {
        (void)esp_codec_dev_close(g_app.mic_dev);
    }
    if (lock_taken && g_app.speaker_dev) {
        (void)esp_codec_dev_close(g_app.speaker_dev);
    }
    gpio_set_level(BSP_POWER_AMP_IO, 0);
    gpio_reset_pin(BSP_I2S_MCLK);
    gpio_reset_pin(BSP_I2S_SCLK);
    gpio_reset_pin(BSP_I2S_LCLK);
    gpio_reset_pin(BSP_I2S_DOUT);
    gpio_reset_pin(BSP_I2S_DSIN);
    pmic_audio_power_set(false);
    if (lock_taken) {
        xSemaphoreGive(g_app.audio_lock);
    }
}

static void imu_shutdown_for_sleep(void)
{
    if (g_app.imu_present && g_app.imu_dev) {
        (void)imu_write_reg(QMI8658_CTRL7, QMI8658_CTRL7_DISABLE);
        g_app.imu_baseline_valid = false;
    }
}

static void pmic_prepare_for_deep_sleep(void)
{
    if (!g_app.pmic_present) {
        return;
    }
    (void)pmic_update_bits(AXP2101_CHARGE_GAUGE_WDT_CTRL, AXP2101_GAUGE_ENABLE_MASK, 0);
    (void)pmic_update_bits(AXP2101_LDO_ONOFF_CTRL0, AXP2101_ALDO2_ENABLE_MASK, 0);
}

static void pmic_audio_power_set(bool enabled)
{
    if (!g_app.pmic_present) {
        return;
    }
    (void)pmic_update_bits(AXP2101_LDO_ONOFF_CTRL0, AXP2101_ALDO2_ENABLE_MASK,
                           enabled ? AXP2101_ALDO2_ENABLE_MASK : 0);
    if (enabled) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void enter_poweroff_now(void)
{
    if (!AUTO_POWEROFF_ON_SLEEP) {
        return;
    }
    if (!g_app.pmic_present) {
        ESP_LOGW(TAG, "poweroff: PMIC not present, staying in standby");
        return;
    }
    if (g_app.deep_sleep_armed) {
        return;
    }

    g_app.deep_sleep_armed = true;
    (void)market_cache_save();

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    bool vbus_good = pmic_read_reg(AXP2101_STATUS1, &status1) == ESP_OK && (status1 & (1u << 5));
    bool vbus_in = pmic_read_reg(AXP2101_STATUS2, &status2) == ESP_OK && ((status2 & (1u << 3)) == 0) && vbus_good;
    ESP_LOGI(TAG, "poweroff: idle timeout, AXP2101 soft shutdown vbus=%d status1=0x%02X status2=0x%02X",
             vbus_in ? 1 : 0, status1, status2);

    esp_err_t err = pmic_update_bits(AXP2101_COMMON_CONFIG,
                                     AXP2101_SOFT_SHUTDOWN_MASK,
                                     AXP2101_SOFT_SHUTDOWN_MASK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poweroff: shutdown request failed: %s", esp_err_to_name(err));
        g_app.deep_sleep_armed = false;
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGW(TAG, "poweroff: PMIC did not cut power, staying in standby");
}

static void wifi_shutdown_for_deep_sleep(void)
{
    wifi_sta_disconnect_now();
    if (!g_app.wifi_started) {
        return;
    }
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    g_app.wifi_started = false;
    g_app.sta_connected = false;
    g_app.pending_wifi_slot = -1;
    g_app.active_wifi_slot = -1;
    g_app.sta_ip[0] = '\0';
}

static void hardware_prepare_for_deep_sleep(void)
{
    ESP_LOGI(TAG, "deep sleep: hardware shutdown");
    g_app.ai_tts_stop_requested = true;
    audio_shutdown_for_sleep();
    imu_shutdown_for_sleep();
    (void)bsp_display_panel_sleep();
    wifi_shutdown_for_deep_sleep();
    pmic_prepare_for_deep_sleep();
#if CONFIG_BT_ENABLED
    (void)esp_bt_controller_disable();
    (void)esp_bt_controller_deinit();
#endif
}

static void configure_power_button_gpio_for_sleep(void)
{
    gpio_config_t power_btn = {
        .pin_bit_mask = 1ULL << DEEP_SLEEP_POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&power_btn);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep: power key gpio config failed: %s", esp_err_to_name(err));
    }
    (void)gpio_sleep_sel_dis(DEEP_SLEEP_POWER_BUTTON_GPIO);

    if (rtc_gpio_is_valid_gpio(DEEP_SLEEP_POWER_BUTTON_GPIO)) {
        (void)rtc_gpio_hold_dis(DEEP_SLEEP_POWER_BUTTON_GPIO);
        err = rtc_gpio_init(DEEP_SLEEP_POWER_BUTTON_GPIO);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deep sleep: power key rtc init failed: %s", esp_err_to_name(err));
        }
        (void)rtc_gpio_set_direction(DEEP_SLEEP_POWER_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
        (void)rtc_gpio_set_direction_in_sleep(DEEP_SLEEP_POWER_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
        (void)rtc_gpio_pullup_dis(DEEP_SLEEP_POWER_BUTTON_GPIO);
        (void)rtc_gpio_pulldown_dis(DEEP_SLEEP_POWER_BUTTON_GPIO);
    }
    (void)esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    ESP_LOGI(TAG, "deep sleep: PWR wake gpio=%d level=%d",
             (int)DEEP_SLEEP_POWER_BUTTON_GPIO, gpio_get_level(DEEP_SLEEP_POWER_BUTTON_GPIO));
}

static void enter_deep_sleep_now(void)
{
#if DEEP_SLEEP_ENABLED
    ESP_LOGI(TAG, "deep sleep: entering, PWR key wake only");
    g_app.deep_sleep_armed = true;
    (void)market_cache_save();
    hardware_prepare_for_deep_sleep();
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    (void)esp_sleep_disable_ext1_wakeup_io(0);
    configure_power_button_gpio_for_sleep();
    esp_err_t wake_err = esp_sleep_enable_ext1_wakeup_io(1ULL << DEEP_SLEEP_POWER_BUTTON_GPIO,
                                                         ESP_EXT1_WAKEUP_ANY_HIGH);
    if (wake_err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep: PWR ext1 wake failed: %s", esp_err_to_name(wake_err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
#endif
}

static void set_sleeping(bool sleeping)
{
    g_app.last_activity_us = esp_timer_get_time();
    if (g_app.screen_sleeping == sleeping) {
        return;
    }
    g_app.screen_sleeping = sleeping;
    if (sleeping) {
        g_app.deep_sleep_armed = false;
        g_app.screen_sleep_started_us = g_app.last_activity_us;
        enter_low_power_sleep();
        enter_poweroff_now();
    } else {
        g_app.deep_sleep_armed = false;
        g_app.screen_sleep_started_us = 0;
        restore_display_brightness();
        battery_update_once();
        home_market_refresh_async();
    }
}

static void wake_screen_for_input(void)
{
    g_app.last_activity_us = esp_timer_get_time();
    if (!g_app.screen_sleeping) {
        return;
    }
    g_app.screen_sleeping = false;
    g_app.deep_sleep_armed = false;
    g_app.screen_sleep_started_us = 0;
    restore_display_brightness();
    battery_update_once();
}

static void activity_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED || code == LV_EVENT_VALUE_CHANGED) {
        if (g_app.screen_sleeping) {
            g_app.last_activity_us = esp_timer_get_time();
            return;
        }
        g_app.last_activity_us = esp_timer_get_time();
    }
}

static lv_obj_t *make_label_font(lv_obj_t *parent, const char *text, int x, int y, int w,
                                 uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_wrap_label_font(lv_obj_t *parent, const char *text, int x, int y,
                                      int w, int h, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                             uint32_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 999, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, activity_event_cb, LV_EVENT_ALL, NULL);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF4F7FF), 0);
    lv_obj_set_style_text_font(label,
                               text_has_non_ascii(text) ? AI_BUTTON_FONT : &lv_font_montserrat_20,
                               0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void home_action_cb(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    if (g_app.screen_sleeping) {
        wake_screen_for_input();
    }
    if (action == 1) {
        ESP_LOGI(TAG, "home action: show AI");
        ui_show_ai();
    } else if (action >= 100 && action < 100 + AI_PROFILE_COUNT) {
        int profile_index = (int)(action - 100);
        ESP_LOGI(TAG, "home action: AI profile %d", profile_index);
        ai_profile_apply(profile_index, true);
        ui_show_ai();
    }
}

static void ai_action_cb(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    if (g_app.screen_sleeping) {
        wake_screen_for_input();
    }
    if (action == 1) {
        ESP_LOGI(TAG, "AI action: back");
        ui_show_home();
    } else if (action == 2) {
        ESP_LOGI(TAG, "AI action: talk");
        (void)direct_ai_start_request("ui");
    }
}

static void market_row_action_cb(lv_event_t *event)
{
    if (g_app.screen_sleeping) {
        return;
    }
    int row_index = (int)(uintptr_t)lv_event_get_user_data(event);
    ui_show_chart(row_index);
    chart_refresh_async(row_index);
}

static void chart_action_cb(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    if (g_app.screen_sleeping) {
        return;
    }
    if (action == 1) {
        ui_show_home();
    } else if (action == 2 && g_app.chart_selected_index >= 0) {
        chart_refresh_async(g_app.chart_selected_index);
    }
}

static void chart_plot_draw_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!obj || !layer) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int w = lv_area_get_width(&coords);
    int h = lv_area_get_height(&coords);
    const int pad = 8;

    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = lv_color_hex(0x1B2638);
    grid.opa = LV_OPA_70;
    grid.width = 1;
    for (int i = 1; i < 4; i++) {
        int y = coords.y1 + pad + ((h - pad * 2) * i) / 4;
        grid.p1.x = coords.x1 + pad;
        grid.p1.y = y;
        grid.p2.x = coords.x2 - pad;
        grid.p2.y = y;
        lv_draw_line(layer, &grid);
    }

    if (g_app.chart_point_count < 2) {
        return;
    }

    double min_v = g_app.chart_values[0];
    double max_v = g_app.chart_values[0];
    for (int i = 1; i < g_app.chart_point_count; i++) {
        double v = g_app.chart_values[i];
        if (v < min_v) {
            min_v = v;
        }
        if (v > max_v) {
            max_v = v;
        }
    }
    if (max_v <= min_v) {
        max_v = min_v + 1.0;
    }

    bool up = g_app.chart_values[g_app.chart_point_count - 1] >= g_app.chart_values[0];
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(up ? 0x46E09F : 0xFF5C5C);
    line.opa = LV_OPA_COVER;
    line.width = 3;
    line.round_start = 1;
    line.round_end = 1;

    int prev_x = 0;
    int prev_y = 0;
    for (int i = 0; i < g_app.chart_point_count; i++) {
        double ratio = (g_app.chart_values[i] - min_v) / (max_v - min_v);
        int x = coords.x1 + pad + ((w - pad * 2) * i) / (g_app.chart_point_count - 1);
        int y = coords.y2 - pad - (int)((h - pad * 2) * ratio);
        if (i > 0) {
            line.p1.x = prev_x;
            line.p1.y = prev_y;
            line.p2.x = x;
            line.p2.y = y;
            lv_draw_line(layer, &line);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void ui_create_home(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05080E), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.time_label = make_label_font(scr, "--:--", 56, 14, 82, 0xEAF1FF, &lv_font_montserrat_20);
    g_app.wifi_label = make_label_font(scr, "W-", 178, 14, 42, 0x91A3BF, &lv_font_montserrat_20);
    g_app.battery_label = make_label_font(scr, "--%", 224, 14, 54, 0x91A3BF, &lv_font_montserrat_20);
    g_app.market_time_label = make_label_font(scr, "--:--", 290, 14, 82, 0x91A3BF, &lv_font_montserrat_20);

    const int row_y0 = 52;
    const int row_h = 30;
    for (int i = 0; i < MARKET_ROW_COUNT; i++) {
        int y = row_y0 + i * row_h;
        g_app.market_rows[i].code_label = make_label_font(scr, MARKET_DEFS[i].code, 80, y, 64,
                                                          MARKET_DEFS[i].color, &lv_font_montserrat_24);
        g_app.market_rows[i].price_label = make_label_font(scr, "--", 146, y, 94,
                                                           0x91A3BF, &lv_font_montserrat_24);
        g_app.market_rows[i].change_label = make_label_font(scr, "--", 244, y, 96,
                                                            0x91A3BF, &lv_font_montserrat_24);
        g_app.market_rows[i].hit_obj = lv_obj_create(scr);
        lv_obj_remove_style_all(g_app.market_rows[i].hit_obj);
        lv_obj_set_pos(g_app.market_rows[i].hit_obj, 76, y - 1);
        lv_obj_set_size(g_app.market_rows[i].hit_obj, 268, row_h);
        lv_obj_add_flag(g_app.market_rows[i].hit_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(g_app.market_rows[i].hit_obj, activity_event_cb, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(g_app.market_rows[i].hit_obj, market_row_action_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    const int btn_y_top = 72;
    const int btn_y_mid = 238;
    const int btn_y_bottom = 404;
    const int side_btn_h = 42;
    make_button(scr, "GPT", 8, btn_y_top, 62, side_btn_h, 0x2255A4, home_action_cb, (void *)100);
    make_button(scr, "Gem", 8, btn_y_mid, 62, side_btn_h, 0x167C7B, home_action_cb, (void *)101);
    make_button(scr, "豆包", 8, btn_y_bottom, 62, side_btn_h, 0x8A4E1D, home_action_cb, (void *)102);
    make_button(scr, "DS", 340, btn_y_top, 62, side_btn_h, 0x1B5D78, home_action_cb, (void *)103);
    make_button(scr, "千问", 340, btn_y_mid, 62, side_btn_h, 0x5C4FA3, home_action_cb, (void *)104);
    make_button(scr, "自定", 340, btn_y_bottom, 62, side_btn_h, 0x31523E, home_action_cb, (void *)105);
    g_app.ai_profile_label = NULL;
    g_app.home_screen = scr;
}

static void ui_create_ai(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x03070D), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 20, 56);
    lv_obj_set_size(box, 370, 340);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1B2638), 0);
    lv_obj_add_event_cb(box, activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.ai_title_label = make_label_font(scr, "AI", 24, 18, 160, 0x46E09F, AI_DIALOG_FONT);
    g_app.ai_state_label = make_wrap_label_font(scr, "状态：READY", 34, 68, 342, 42,
                                                0xD8E7FF, AI_DIALOG_FONT);
    lv_obj_set_style_text_line_space(g_app.ai_state_label, 4, 0);
    g_app.ai_reply_container = lv_obj_create(scr);
    lv_obj_remove_style_all(g_app.ai_reply_container);
    lv_obj_set_pos(g_app.ai_reply_container, 34, 118);
    lv_obj_set_size(g_app.ai_reply_container, 342, 276);
    lv_obj_set_style_bg_opa(g_app.ai_reply_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_app.ai_reply_container, 0, 0);
    lv_obj_set_scroll_dir(g_app.ai_reply_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_app.ai_reply_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(g_app.ai_reply_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_app.ai_reply_container, activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.ai_reply_label = make_wrap_label_font(
        g_app.ai_reply_container,
        "中文显示正常\n点说话开始",
        0, 0, 342, LV_SIZE_CONTENT,
        0xF8FBFF, AI_DIALOG_FONT);
    lv_obj_set_style_text_line_space(g_app.ai_reply_label, 8, 0);
    lv_obj_set_style_bg_color(g_app.ai_reply_label, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(g_app.ai_reply_label, LV_OPA_TRANSP, 0);

    make_button(scr, "返回", 34, 428, 140, 54, 0x223148, ai_action_cb, (void *)1);
    make_button(scr, "说话", 236, 428, 140, 54, 0x1C755C, ai_action_cb, (void *)2);
    g_app.ai_screen = scr;
}

static void ui_create_chart(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x03070D), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    g_app.chart_title_label = make_label_font(scr, "--", 38, 24, 92, 0xEAF1FF, &lv_font_montserrat_28);
    g_app.chart_price_label = make_label_font(scr, "-- --", 132, 31, 244, 0x91A3BF, &lv_font_montserrat_20);

    g_app.chart_plot_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(g_app.chart_plot_obj);
    lv_obj_set_pos(g_app.chart_plot_obj, 30, 76);
    lv_obj_set_size(g_app.chart_plot_obj, 350, 330);
    lv_obj_set_style_bg_color(g_app.chart_plot_obj, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(g_app.chart_plot_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_app.chart_plot_obj, 18, 0);
    lv_obj_set_style_border_width(g_app.chart_plot_obj, 1, 0);
    lv_obj_set_style_border_color(g_app.chart_plot_obj, lv_color_hex(0x1B2638), 0);
    lv_obj_add_event_cb(g_app.chart_plot_obj, chart_plot_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    g_app.chart_hint_label = make_label_font(scr, "Tap refresh", 42, 410, 300, 0x91A3BF, &lv_font_montserrat_20);

    make_button(scr, "BACK", 46, 442, 112, 42, 0x223148, chart_action_cb, (void *)1);
    make_button(scr, "REF", 252, 442, 112, 42, 0x1C755C, chart_action_cb, (void *)2);
    g_app.chart_screen = scr;
}

static void ui_show_home(void)
{
    if (g_app.home_screen) {
        lv_screen_load(g_app.home_screen);
        ui_refresh_locked();
    }
}

static void ui_show_ai(void)
{
    if (g_app.ai_screen) {
        lv_screen_load(g_app.ai_screen);
        ui_refresh_locked();
    }
}

static void ui_show_chart(int row_index)
{
    if (row_index < 0 || row_index >= MARKET_ROW_COUNT || !g_app.chart_screen) {
        return;
    }
    g_app.chart_selected_index = row_index;
    g_app.chart_point_count = 0;
    copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "Loading");
    lv_screen_load(g_app.chart_screen);
    ui_refresh_locked();
}

static const char *ui_current_screen_name(void)
{
    lv_obj_t *active = lv_screen_active();
    if (active == g_app.home_screen) {
        return "home";
    }
    if (active == g_app.ai_screen) {
        return "ai";
    }
    if (active == g_app.chart_screen) {
        return "chart";
    }
    return "unknown";
}

static uint32_t market_value_color(const char *change)
{
    if (!change || change[0] == '\0' || strcmp(change, "--") == 0) {
        return 0x91A3BF;
    }
    return change[0] == '-' ? 0xFF5C5C : 0x46E09F;
}

static uint32_t battery_value_color(void)
{
    if (g_app.battery_percent < 0) {
        return 0x91A3BF;
    }
    if (g_app.battery_percent <= 15) {
        return 0xFF5C5C;
    }
    if (g_app.battery_percent <= 30) {
        return 0xF7C873;
    }
    return 0xEAF1FF;
}

static void set_market_value_label(lv_obj_t *label, const char *text, const char *change)
{
    if (!label) {
        return;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(market_value_color(change)), 0);
}

static void ui_refresh_locked(void)
{
    if (g_app.wifi_label) {
        const char *state = g_app.sta_connected ? "W+" :
                            (g_app.ap_started ? "AP" : (g_app.sta_configured ? "W-" : "AP"));
        lv_label_set_text(g_app.wifi_label, state);
        lv_obj_set_style_text_color(g_app.wifi_label,
                                    lv_color_hex(g_app.sta_connected ? 0x46E09F : 0x91A3BF), 0);
    }

    if (g_app.time_label) {
        time_t now = 0;
        struct tm tm_now = {0};
        time(&now);
        localtime_r(&now, &tm_now);
        if (tm_now.tm_year >= 120) {
            lv_label_set_text_fmt(g_app.time_label, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        } else {
            lv_label_set_text(g_app.time_label, "--:--");
        }
    }

    if (g_app.market_time_label) {
        lv_label_set_text(g_app.market_time_label, g_app.market_time);
    }

    if (g_app.battery_label) {
        if (g_app.battery_percent >= 0) {
            lv_label_set_text_fmt(g_app.battery_label, "%d%%", g_app.battery_percent);
        } else {
            lv_label_set_text(g_app.battery_label, "--%");
        }
        lv_obj_set_style_text_color(g_app.battery_label, lv_color_hex(battery_value_color()), 0);
    }

    for (int i = 0; i < MARKET_ROW_COUNT; i++) {
        market_row_t *row = &g_app.market_rows[i];
        set_market_value_label(row->price_label, row->price, row->change);
        set_market_value_label(row->change_label, row->change, row->change);
    }
    if (g_app.market_status_label) {
        lv_label_set_text(g_app.market_status_label, g_app.market_status);
    }

    const ai_profile_t *active_profile = ai_profile_active();
    if (g_app.ai_profile_label) {
        lv_label_set_text(g_app.ai_profile_label, active_profile->name);
    }

    if (g_app.status_label) {
        if (g_app.sta_connected) {
            lv_label_set_text_fmt(g_app.status_label, "%s", g_app.sta_ip);
        } else if (g_app.ap_started) {
            lv_label_set_text_fmt(g_app.status_label, "%s 192.168.4.1", g_app.ap_ssid);
        } else {
            lv_label_set_text(g_app.status_label, "BOOT 2s = CFG AP");
        }
    }
    if (g_app.ai_state_label || g_app.ai_reply_label) {
        if (g_app.ai_title_label) {
            lv_label_set_text(g_app.ai_title_label, active_profile->name);
        }
        if (g_app.ai_state_label) {
            lv_label_set_text_fmt(g_app.ai_state_label, "状态：%s",
                                  g_app.ai_state[0] ? g_app.ai_state : "READY");
        }
        if (g_app.ai_reply_label) {
            char reply_text[sizeof(g_app.ai_reply)] = {0};
            display_safe_text_copy(g_app.ai_reply, reply_text, sizeof(reply_text));
            lv_label_set_long_mode(g_app.ai_reply_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(g_app.ai_reply_label,
                              reply_text[0] ? reply_text : "中文显示正常\n点说话开始");
            lv_obj_set_height(g_app.ai_reply_label, LV_SIZE_CONTENT);
        }
    }

    if (g_app.chart_selected_index >= 0 && g_app.chart_selected_index < MARKET_ROW_COUNT) {
        int i = g_app.chart_selected_index;
        market_row_t *row = &g_app.market_rows[i];
        if (g_app.chart_title_label) {
            lv_label_set_text(g_app.chart_title_label, MARKET_DEFS[i].code);
            lv_obj_set_style_text_color(g_app.chart_title_label, lv_color_hex(MARKET_DEFS[i].color), 0);
        }
        if (g_app.chart_price_label) {
            lv_label_set_text_fmt(g_app.chart_price_label, "%s %s", row->price, row->change);
            lv_obj_set_style_text_color(g_app.chart_price_label,
                                        lv_color_hex(market_value_color(row->change)), 0);
        }
        if (g_app.chart_hint_label) {
            lv_label_set_text(g_app.chart_hint_label, g_app.chart_hint);
        }
        if (g_app.chart_plot_obj) {
            lv_obj_invalidate(g_app.chart_plot_obj);
        }
    }
}

static void ui_refresh_now(void)
{
    if (!bsp_display_lock(100)) {
        return;
    }
    ui_refresh_locked();
    bsp_display_unlock();
}

static bool enter_button_light_sleep_if_ready(void)
{
    if (!BUTTON_LIGHT_SLEEP_ENABLED || DEEP_SLEEP_AUTO_ENTER ||
        !g_app.screen_sleeping || app_busy_for_sleep() ||
        g_app.boot_wake_refresh_pending || g_app.boot_config_ap_pending) {
        return false;
    }

    if (gpio_get_level(DEEP_SLEEP_BOOT_BUTTON_GPIO) == 0) {
        g_app.last_activity_us = esp_timer_get_time();
        g_app.boot_wake_refresh_pending = true;
        return true;
    }

    esp_err_t err = gpio_wakeup_enable(DEEP_SLEEP_BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light sleep: BOOT gpio wake enable failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light sleep: gpio wake source enable failed: %s", esp_err_to_name(err));
        (void)gpio_wakeup_disable(DEEP_SLEEP_BOOT_BUTTON_GPIO);
        return false;
    }

    ESP_LOGI(TAG, "light sleep: entering, BOOT/GPIO%d low wakes",
             (int)DEEP_SLEEP_BOOT_BUTTON_GPIO);
    int64_t before_us = esp_timer_get_time();
    err = esp_light_sleep_start();
    int64_t slept_ms = (esp_timer_get_time() - before_us) / 1000;

    (void)gpio_wakeup_disable(DEEP_SLEEP_BOOT_BUTTON_GPIO);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light sleep: start failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "light sleep: woke cause=%d boot=%d slept=%lldms",
             (int)esp_sleep_get_wakeup_cause(),
             gpio_get_level(DEEP_SLEEP_BOOT_BUTTON_GPIO),
             (long long)slept_ms);
    g_app.last_activity_us = esp_timer_get_time();
    g_app.boot_wake_refresh_pending = true;
    return true;
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    process_pending_button_requests();
    ui_refresh_now();

    int64_t now_us = esp_timer_get_time();
    if (!g_app.screen_sleeping && !app_busy_for_sleep() &&
        (g_app.last_market_refresh_us == 0 ||
         now_us - g_app.last_market_refresh_us >= (int64_t)MARKET_REFRESH_INTERVAL_S * 1000000LL)) {
        home_market_refresh_async();
    }

    if (!g_app.screen_sleeping && g_app.cfg.auto_sleep_s > 0 &&
        !app_busy_for_sleep()) {
        int64_t idle_us = now_us - g_app.last_activity_us;
        if (idle_us >= (int64_t)g_app.cfg.auto_sleep_s * 1000000LL) {
            set_sleeping(true);
        }
    }

    if (DEEP_SLEEP_AUTO_ENTER && g_app.screen_sleeping && !g_app.deep_sleep_armed && !app_busy_for_sleep()) {
        int64_t sleep_start_us = g_app.screen_sleep_started_us ? g_app.screen_sleep_started_us : g_app.last_activity_us;
        int64_t sleep_us = now_us - sleep_start_us;
        if (sleep_us >= (int64_t)DEEP_SLEEP_AFTER_SCREEN_OFF_S * 1000000LL) {
            enter_deep_sleep_now();
        }
    }

    if (enter_button_light_sleep_if_ready()) {
        process_pending_button_requests();
        ui_refresh_now();
    }
}

static esp_err_t pmic_write_reg(uint8_t reg, uint8_t value)
{
    if (!g_app.pmic_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(g_app.pmic_dev, data, sizeof(data), 100);
}

static esp_err_t pmic_read_reg(uint8_t reg, uint8_t *value)
{
    if (!g_app.pmic_dev || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(g_app.pmic_dev, &reg, 1, value, 1, 100);
}

static esp_err_t pmic_update_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old_value = 0;
    esp_err_t err = pmic_read_reg(reg, &old_value);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t new_value = (old_value & (uint8_t)~mask) | (value & mask);
    if (new_value == old_value) {
        return ESP_OK;
    }
    return pmic_write_reg(reg, new_value);
}

static esp_err_t pmic_init(void)
{
    g_app.pmic_present = false;
    g_app.battery_present = false;
    g_app.battery_percent = -1;
    g_app.battery_mv = 0;

    if (!g_app.pmic_dev) {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (!bus) {
            ESP_LOGW(TAG, "PMIC I2C bus unavailable");
            return ESP_ERR_INVALID_STATE;
        }

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 400000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &g_app.pmic_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AXP2101 add failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    uint8_t chip_id = 0;
    esp_err_t err = pmic_read_reg(AXP2101_CHIP_ID, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 probe failed: %s", esp_err_to_name(err));
        return err;
    }

    if (chip_id != AXP2101_CHIP_ID_VALUE_A && chip_id != AXP2101_CHIP_ID_VALUE_B) {
        ESP_LOGW(TAG, "AXP2101 unexpected chip id 0x%02X; trying battery registers", chip_id);
    } else {
        ESP_LOGI(TAG, "AXP2101 found id=0x%02X", chip_id);
    }

    g_app.pmic_present = true;
    (void)pmic_update_bits(AXP2101_BAT_DET_CTRL, AXP2101_BAT_TYPE_DET_MASK,
                           AXP2101_BAT_TYPE_DET_MASK);
    (void)pmic_update_bits(AXP2101_CHARGE_GAUGE_WDT_CTRL, AXP2101_GAUGE_ENABLE_MASK,
                           AXP2101_GAUGE_ENABLE_MASK);
    return ESP_OK;
}

static esp_err_t pmic_read_battery(void)
{
    if (!g_app.pmic_present) {
        g_app.battery_present = false;
        g_app.battery_percent = -1;
        g_app.battery_mv = 0;
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    esp_err_t status_err = pmic_read_reg(AXP2101_STATUS1, &status);
    if (status_err == ESP_OK) {
        g_app.battery_present = (status & AXP2101_BAT_PRESENT_MASK) != 0;
    }

    uint8_t percent_reg = 0;
    esp_err_t err = pmic_read_reg(AXP2101_BAT_PERCENT_DATA, &percent_reg);
    if (err != ESP_OK) {
        g_app.battery_percent = -1;
        ESP_LOGW(TAG, "battery percent read failed: %s", esp_err_to_name(err));
        return err;
    }

    int percent = percent_reg & 0x7F;
    if (percent > 100 || percent_reg == 0xFF) {
        percent = -1;
    }
    g_app.battery_percent = percent;

    uint8_t vbat_h = 0;
    uint8_t vbat_l = 0;
    if (pmic_read_reg(AXP2101_ADC_DATA_VBAT_H, &vbat_h) == ESP_OK &&
        pmic_read_reg(AXP2101_ADC_DATA_VBAT_L, &vbat_l) == ESP_OK) {
        g_app.battery_mv = ((int)(vbat_h & AXP2101_GAUGE_VBAT_H_MASK) << 8) | vbat_l;
    } else {
        g_app.battery_mv = 0;
    }

    g_app.last_battery_read_us = esp_timer_get_time();
    ESP_LOGI(TAG, "battery wake-read percent=%d raw=0x%02X mv=%d present=%d status=0x%02X",
             g_app.battery_percent, percent_reg, g_app.battery_mv, g_app.battery_present, status);
    return ESP_OK;
}

static void battery_update_once(void)
{
    if (!g_app.pmic_present) {
        return;
    }
    (void)pmic_read_battery();
    ui_refresh_now();
}

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    if (!g_app.imu_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(g_app.imu_dev, data, sizeof(data), 100);
}

static esp_err_t imu_read_reg(uint8_t reg, uint8_t *value)
{
    if (!g_app.imu_dev || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(g_app.imu_dev, &reg, 1, value, 1, 100);
}

static esp_err_t imu_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (!g_app.imu_dev || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(g_app.imu_dev, &reg, 1, data, len, 100);
}

static esp_err_t imu_add_device(uint8_t address)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &g_app.imu_dev);
}

static esp_err_t imu_try_address(uint8_t address)
{
    if (g_app.imu_dev) {
        (void)i2c_master_bus_rm_device(g_app.imu_dev);
        g_app.imu_dev = NULL;
    }

    esp_err_t err = imu_add_device(address);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t id = 0;
    err = imu_read_reg(QMI8658_WHOAMI, &id);
    if (err != ESP_OK || id != QMI8658_WHOAMI_VALUE) {
        ESP_LOGW(TAG, "QMI8658 probe 0x%02X failed err=%s id=0x%02X",
                 address, esp_err_to_name(err), id);
        (void)i2c_master_bus_rm_device(g_app.imu_dev);
        g_app.imu_dev = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "QMI8658 found at 0x%02X", address);
    return ESP_OK;
}

static esp_err_t imu_init(void)
{
    esp_err_t err = imu_try_address(QMI8658_ADDR_LOW);
    if (err != ESP_OK) {
        err = imu_try_address(QMI8658_ADDR_HIGH);
    }
    if (err != ESP_OK) {
        g_app.imu_present = false;
        return err;
    }

    (void)imu_write_reg(QMI8658_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t reset_result = 0;
    for (int i = 0; i < 20; i++) {
        if (imu_read_reg(QMI8658_RST_RESULT, &reset_result) == ESP_OK &&
            reset_result == QMI8658_RST_RESULT_VALUE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    WL_RETURN_ON_ERROR(imu_write_reg(QMI8658_CTRL1, 0x40), "imu ctrl1 failed");
    WL_RETURN_ON_ERROR(imu_write_reg(QMI8658_CTRL2, 0x2D), "imu accel cfg failed");
    WL_RETURN_ON_ERROR(imu_write_reg(QMI8658_CTRL5, 0x00), "imu lpf cfg failed");
    WL_RETURN_ON_ERROR(imu_write_reg(QMI8658_CTRL7, 0x01), "imu accel enable failed");

    g_app.imu_present = true;
    g_app.imu_baseline_valid = false;
    ESP_LOGI(TAG, "QMI8658 motion wake ready");
    return ESP_OK;
}

static esp_err_t imu_read_accel_raw(int16_t raw[3])
{
    uint8_t data[6] = {0};
    esp_err_t err = imu_read_regs(QMI8658_ACCEL_X_L, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    raw[0] = (int16_t)((uint16_t)data[1] << 8 | data[0]);
    raw[1] = (int16_t)((uint16_t)data[3] << 8 | data[2]);
    raw[2] = (int16_t)((uint16_t)data[5] << 8 | data[4]);
    return ESP_OK;
}

static int raw_delta_to_mg(int delta_raw)
{
    if (delta_raw < 0) {
        delta_raw = -delta_raw;
    }
    return (delta_raw * QMI8658_ACCEL_RANGE_MG) / 32768;
}

static bool motion_is_strong_shake(const int16_t raw[3])
{
    int max_delta_mg = 0;
    for (int i = 0; i < 3; i++) {
        int delta_mg = raw_delta_to_mg((int)raw[i] - (int)g_app.imu_last_raw[i]);
        if (delta_mg > max_delta_mg) {
            max_delta_mg = delta_mg;
        }
    }

    memcpy(g_app.imu_last_raw, raw, sizeof(g_app.imu_last_raw));
    return max_delta_mg >= g_app.cfg.motion_wake_threshold_mg;
}

static void motion_task(void *arg)
{
    (void)arg;
    if (imu_init() != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 unavailable; motion wake disabled");
    }

    while (true) {
        if (!g_app.imu_present || !g_app.cfg.motion_wake_enabled) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!g_app.screen_sleeping) {
            g_app.imu_baseline_valid = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int16_t raw[3] = {0};
        if (imu_read_accel_raw(raw) != ESP_OK) {
            g_app.imu_baseline_valid = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!g_app.imu_baseline_valid) {
            memcpy(g_app.imu_last_raw, raw, sizeof(g_app.imu_last_raw));
            g_app.imu_baseline_valid = true;
            vTaskDelay(pdMS_TO_TICKS(MOTION_WAKE_POLL_MS));
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (now - g_app.last_motion_wake_us > MOTION_WAKE_COOLDOWN_US &&
            motion_is_strong_shake(raw)) {
            g_app.last_motion_wake_us = now;
            g_app.imu_baseline_valid = false;
            set_sleeping(false);
            ui_refresh_now();
        }

        vTaskDelay(pdMS_TO_TICKS(MOTION_WAKE_POLL_MS));
    }
}

static void motion_task_start_once(void)
{
    if (!g_app.cfg.motion_wake_enabled) {
        ESP_LOGI(TAG, "motion wake disabled; IMU task not started");
        return;
    }
    if (g_app.motion_worker_started) {
        return;
    }
    g_app.motion_worker_started = true;
    xTaskCreate(motion_task, "motion_wake", 4096, NULL, 3, NULL);
}

static void power_button_gpio_prepare_active(void)
{
    if (rtc_gpio_is_valid_gpio(DEEP_SLEEP_POWER_BUTTON_GPIO)) {
        (void)rtc_gpio_hold_dis(DEEP_SLEEP_POWER_BUTTON_GPIO);
        (void)rtc_gpio_deinit(DEEP_SLEEP_POWER_BUTTON_GPIO);
    }
    (void)gpio_sleep_sel_dis(DEEP_SLEEP_POWER_BUTTON_GPIO);
    gpio_config_t power_btn = {
        .pin_bit_mask = 1ULL << DEEP_SLEEP_POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&power_btn);
}

static void handle_button_wake_refresh(void)
{
    int64_t now_us = esp_timer_get_time();
    if (g_app.screen_sleeping) {
        wake_screen_for_input();
    } else {
        g_app.last_activity_us = now_us;
    }
    if (g_app.chart_screen && lv_screen_active() == g_app.chart_screen &&
        g_app.chart_selected_index >= 0) {
        chart_refresh_async(g_app.chart_selected_index);
    } else {
        ui_show_home();
        home_market_refresh_async();
    }
}

static void process_pending_button_requests(void)
{
    bool config_ap_pending = g_app.boot_config_ap_pending;
    bool wake_refresh_pending = g_app.boot_wake_refresh_pending;

    if (!config_ap_pending && !wake_refresh_pending) {
        return;
    }

    g_app.boot_config_ap_pending = false;
    g_app.boot_wake_refresh_pending = false;

    if (config_ap_pending) {
        ESP_LOGI(TAG, "boot button: handle config AP on UI task");
        g_app.last_activity_us = esp_timer_get_time();
        if (g_app.screen_sleeping) {
            wake_screen_for_input();
        }
        (void)wifi_enable_config_ap();
        return;
    }

    ESP_LOGI(TAG, "boot button: handle wake and refresh on UI task");
    handle_button_wake_refresh();
}

static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t boot_btn = {
        .pin_bit_mask = 1ULL << DEEP_SLEEP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&boot_btn);

    int last_level = gpio_get_level(DEEP_SLEEP_BOOT_BUTTON_GPIO);
    ESP_LOGI(TAG, "boot button: active gpio=%d initial=%d",
             (int)DEEP_SLEEP_BOOT_BUTTON_GPIO, last_level);
    int64_t press_start_us = 0;
    bool long_press_handled = false;
    while (true) {
        int level = gpio_get_level(DEEP_SLEEP_BOOT_BUTTON_GPIO);
        if (last_level == 1 && level == 0) {
            press_start_us = esp_timer_get_time();
            long_press_handled = false;
        } else if (level == 0 && press_start_us > 0 && !long_press_handled) {
            int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;
            if (held_ms >= BOOT_LONG_PRESS_CONFIG_AP_MS) {
                long_press_handled = true;
                g_app.last_activity_us = esp_timer_get_time();
                ESP_LOGI(TAG, "boot button: long press, request config AP");
                g_app.boot_config_ap_pending = true;
            }
        } else if (last_level == 0 && level == 1) {
            int64_t now_us = esp_timer_get_time();
            int64_t held_ms = press_start_us > 0 ? (now_us - press_start_us) / 1000 : 0;
            press_start_us = 0;
            if (!long_press_handled && held_ms >= 60) {
                ESP_LOGI(TAG, "boot button: wake and refresh requested");
                g_app.last_activity_us = now_us;
                g_app.boot_wake_refresh_pending = true;
            }
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static void boot_button_task_start_once(void)
{
    xTaskCreate(boot_button_task, "boot_button", 3072, NULL, 3, NULL);
}

static const char *wake_cause_name(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "undefined";
    case ESP_SLEEP_WAKEUP_EXT0:
        return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
        return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
        return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        return "touch";
    case ESP_SLEEP_WAKEUP_ULP:
        return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:
        return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
        return "uart";
    default:
        return "other";
    }
}

static void log_wakeup_reason(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    uint64_t ext1_mask = esp_sleep_get_ext1_wakeup_status();
    ESP_LOGI(TAG, "wake cause: %s(%d) ext1=0x%llx boot=%d pwr=%d",
             wake_cause_name(cause), (int)cause, (unsigned long long)ext1_mask,
             gpio_get_level(DEEP_SLEEP_BOOT_BUTTON_GPIO),
             gpio_get_level(DEEP_SLEEP_POWER_BUTTON_GPIO));
}

static void update_ai_dialog_text(void)
{
    const char *state = g_app.ai_state[0] ? g_app.ai_state : "READY";
    const char *state_cn = "就绪";
    if (strcmp(state, "REC") == 0) {
        state_cn = "录音";
    } else if (strcmp(state, "NET") == 0) {
        state_cn = "联网";
    } else if (strcmp(state, "VOICE") == 0 || strcmp(state, "STT") == 0) {
        state_cn = "识别";
    } else if (strcmp(state, "AI") == 0) {
        state_cn = "思考";
    } else if (strcmp(state, "TTS") == 0) {
        state_cn = "合成";
    } else if (strcmp(state, "PLAY") == 0) {
        state_cn = "播放";
    } else if (strstr(state, "ERR") || strcmp(state, "ERROR") == 0) {
        state_cn = "错误";
    } else if (g_app.ai_tts_inflight || g_app.ai_playback_inflight) {
        state_cn = "语音";
    }

    char user_text[sizeof(g_app.ai_user)] = {0};
    char reply_text[sizeof(g_app.ai_reply)] = {0};
    display_safe_text_copy(g_app.ai_user, user_text, sizeof(user_text));
    display_safe_text_copy(g_app.ai_reply, reply_text, sizeof(reply_text));
    if (!reply_text[0]) {
        copy_string(reply_text, sizeof(reply_text), "就绪");
    }

    if (g_app.ai_user[0]) {
        snprintf(g_app.ai_dialog, sizeof(g_app.ai_dialog),
                 "状态：%s\n你：%s\n答：%s", state_cn, user_text, reply_text);
    } else {
        snprintf(g_app.ai_dialog, sizeof(g_app.ai_dialog),
                 "状态：%s\n答：%s", state_cn, reply_text);
    }
}

static void log_voice_task_heap(const char *reason)
{
    ESP_LOGE(TAG,
             "voice: %s int_free=%u int_largest=%u psram_free=%u psram_largest=%u",
             reason ? reason : "task create failed",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t direct_http_event_cb(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data && event->data_len > 0) {
        http_buffer_t *buffer = (http_buffer_t *)event->user_data;
        if (!buffer->data || buffer->capacity <= 0) {
            return ESP_OK;
        }
        int copy_len = event->data_len;
        int remain = buffer->capacity - buffer->len - 1;
        if (copy_len > remain) {
            copy_len = remain;
        }
        if (copy_len > 0) {
            memcpy(buffer->data + buffer->len, event->data, copy_len);
            buffer->len += copy_len;
            buffer->data[buffer->len] = '\0';
        }
    }
    return ESP_OK;
}

static char *http_body_alloc(size_t size)
{
    char *body = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
    }
    return body;
}

static void http_body_free(char *body)
{
    if (body) {
        heap_caps_free(body);
    }
}

static void log_http_failure(const char *url, esp_err_t err, int status,
                             int sock_errno, int tls_err, int tls_flags)
{
    ESP_LOGW(TAG,
             "http failed err=%s status=%d errno=%d tls=%d flags=0x%x url=%s int_free=%u int_largest=%u psram_free=%u psram_largest=%u",
             esp_err_to_name(err), status, sock_errno, tls_err, tls_flags,
             url ? url : "",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t direct_http_request(const char *url, const char *payload, int timeout_ms,
                                     bool use_auth, char *body, size_t body_size)
{
    if (!g_app.sta_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!body || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    body[0] = '\0';

    http_buffer_t buffer = {
        .data = body,
        .len = 0,
        .capacity = (int)body_size,
    };
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = direct_http_event_cb,
        .user_data = &buffer,
        .disable_auto_redirect = false,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    if (payload) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload, strlen(payload));
    }
    if (use_auth && g_app.cfg.api_key[0]) {
        char auth[224] = {0};
        snprintf(auth, sizeof(auth), "Bearer %s", g_app.cfg.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    esp_http_client_set_header(client, "Accept", "*/*");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int sock_errno = esp_http_client_get_errno(client);
    int tls_err = 0;
    int tls_flags = 0;
    (void)esp_http_client_get_and_clear_last_tls_error(client, &tls_err, &tls_flags);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        log_http_failure(url, err, status, sock_errno, tls_err, tls_flags);
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "http status=%d url=%s body=%s", status, url, body);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void build_api_endpoint_url(const char *endpoint_path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!endpoint_path || endpoint_path[0] == '\0') {
        return;
    }

    char base[192] = {0};
    copy_string(base, sizeof(base), g_app.cfg.server_url);
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') {
        base[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    static const char *const api_base_paths[] = {
        "/v1",
        "/api/v3",
        "/v1beta/openai",
    };
    for (size_t i = 0; i < sizeof(api_base_paths) / sizeof(api_base_paths[0]); ++i) {
        const char *api_base_path = api_base_paths[i];
        const size_t api_base_path_len = strlen(api_base_path);
        char api_base_path_with_slash[16] = {0};
        snprintf(api_base_path_with_slash, sizeof(api_base_path_with_slash), "%s/", api_base_path);

        const char *path = strstr(base, api_base_path_with_slash);
        if (path) {
            size_t prefix_len = (size_t)(path - base) + api_base_path_len;
            snprintf(out, out_size, "%.*s/%s", (int)prefix_len, base, endpoint_path);
            return;
        }
        if (len >= api_base_path_len &&
            strcmp(base + len - api_base_path_len, api_base_path) == 0) {
            snprintf(out, out_size, "%s/%s", base, endpoint_path);
            return;
        }
    }

    snprintf(out, out_size, "%s/v1/%s", base, endpoint_path);
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

    size_t new_cap = buffer->cap ? buffer->cap : VOICE_AUDIO_GROW_STEP;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap > VOICE_MAX_AUDIO_BYTES) {
            new_cap = VOICE_MAX_AUDIO_BYTES;
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
    if (buffer->len + len > VOICE_MAX_AUDIO_BYTES) {
        len = (buffer->len < VOICE_MAX_AUDIO_BYTES) ? (VOICE_MAX_AUDIO_BYTES - buffer->len) : 0;
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

static bool build_gateway_root_url(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    char base[192] = {0};
    copy_string(base, sizeof(base), g_app.cfg.server_url);
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') {
        base[--len] = '\0';
    }
    if (len == 0) {
        return false;
    }

    char *v1 = strstr(base, "/v1");
    if (v1 && (v1[3] == '\0' || v1[3] == '/')) {
        *v1 = '\0';
    }
    if (base[0] == '\0') {
        return false;
    }
    copy_string(out, out_size, base);
    return out[0] != '\0';
}

static bool build_gateway_url(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        return false;
    }
    char root[192] = {0};
    if (!build_gateway_root_url(root, sizeof(root))) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, out_size, "%s%s%s", root, path[0] == '/' ? "" : "/", path);
    return out[0] != '\0';
}

static bool api_url_is_local_gateway(void)
{
    const char *url = g_app.cfg.server_url;
    if (!url || !url[0]) {
        return false;
    }
    return strncasecmp(url, "http://", 7) == 0 ||
           strstr(url, "/api/watch") != NULL ||
           strstr(url, "192.168.") != NULL ||
           strstr(url, "10.") != NULL ||
           strstr(url, "172.16.") != NULL ||
           strstr(url, "172.17.") != NULL ||
           strstr(url, "172.18.") != NULL ||
           strstr(url, "localhost") != NULL ||
           strstr(url, "127.0.0.1") != NULL;
}

static char *build_voice_context_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "battery_percent", g_app.battery_percent);
    cJSON_AddStringToObject(root, "market_time", g_app.market_time);
    cJSON *markets = cJSON_AddArrayToObject(root, "markets");
    if (markets) {
        for (int i = 0; i < MARKET_ROW_COUNT; ++i) {
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            cJSON_AddStringToObject(item, "code", MARKET_DEFS[i].code);
            cJSON_AddStringToObject(item, "price", g_app.market_rows[i].price);
            cJSON_AddStringToObject(item, "change", g_app.market_rows[i].change);
            cJSON_AddItemToArray(markets, item);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t build_gateway_voice_multipart_body(const uint8_t *wav_data, size_t wav_len,
                                                    binary_buffer_t *body)
{
    if (!wav_data || wav_len == 0 || !body) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *boundary = "----watchlite-voice-boundary";
    esp_err_t err = binary_buffer_append_format(
        body,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\nzh\r\n",
        boundary);
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append_format(
        body,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n"
        "中文普通话语音转写，只输出用户说的话，不要翻译。\r\n",
        boundary);
    if (err != ESP_OK) {
        return err;
    }

    char *context = build_voice_context_json();
    if (context) {
        err = binary_buffer_append_format(
            body,
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"context\"\r\n"
            "Content-Type: application/json\r\n\r\n",
            boundary);
        if (err == ESP_OK) {
            err = binary_buffer_append(body, (const uint8_t *)context, strlen(context));
        }
        free(context);
        if (err != ESP_OK) {
            return err;
        }
        err = binary_buffer_append(body, (const uint8_t *)"\r\n", 2);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = binary_buffer_append_format(
        body,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary);
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append(body, wav_data, wav_len);
    if (err != ESP_OK) {
        return err;
    }
    return binary_buffer_append_format(body, "\r\n--%s--\r\n", boundary);
}

static bool parse_gateway_voice_response(const char *body, voice_gateway_result_t *result,
                                         char *error_out, size_t error_out_len)
{
    if (!body || !result) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "网关响应不是有效 JSON");
        }
        return false;
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(root, "reply");
    const cJSON *audio_url = cJSON_GetObjectItemCaseSensitive(root, "audio_url");
    const cJSON *local = cJSON_GetObjectItemCaseSensitive(root, "local");
    const cJSON *refresh_market = cJSON_GetObjectItemCaseSensitive(root, "refresh_market");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *asset = cJSON_GetObjectItemCaseSensitive(root, "asset");
    const cJSON *brightness_delta = cJSON_GetObjectItemCaseSensitive(root, "brightness_delta");
    if (cJSON_IsString(text) && text->valuestring) {
        copy_ai_text(result->transcript, sizeof(result->transcript), text->valuestring);
    }
    if (cJSON_IsString(reply) && reply->valuestring) {
        copy_ai_text(result->reply, sizeof(result->reply), reply->valuestring);
    }
    if (cJSON_IsString(audio_url) && audio_url->valuestring) {
        copy_string(result->audio_url, sizeof(result->audio_url), audio_url->valuestring);
    }
    if (cJSON_IsString(action) && action->valuestring) {
        copy_string(result->action, sizeof(result->action), action->valuestring);
    }
    if (cJSON_IsString(asset) && asset->valuestring) {
        copy_string(result->asset, sizeof(result->asset), asset->valuestring);
    }
    if (cJSON_IsNumber(brightness_delta)) {
        result->brightness_delta = brightness_delta->valueint;
    }
    result->local = cJSON_IsTrue(local);
    result->refresh_market = cJSON_IsTrue(refresh_market);

    bool ok = result->transcript[0] != '\0' && result->reply[0] != '\0' &&
              result->audio_url[0] != '\0';
    if (!ok && error_out && error_out_len) {
        if (!extract_error_string_from_json(root, error_out, error_out_len)) {
            copy_string(error_out, error_out_len, "网关响应缺少文本或音频地址");
        }
    }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t perform_gateway_voice_request(const uint8_t *wav_data, size_t wav_len,
                                               voice_gateway_result_t *result,
                                               char *error_out, size_t error_out_len)
{
    if (!wav_data || wav_len == 0 || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    char voice_url[224] = {0};
    if (!build_gateway_url("/api/watch/voice", voice_url, sizeof(voice_url))) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "无法生成语音网关地址");
        }
        return ESP_ERR_INVALID_ARG;
    }

    binary_buffer_t request = {0};
    esp_err_t err = build_gateway_voice_multipart_body(wav_data, wav_len, &request);
    if (err != ESP_OK) {
        binary_buffer_free(&request);
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "构建语音网关请求失败");
        }
        return err;
    }

    char *body = http_body_alloc(DIRECT_HTTP_MAX_BODY + 1);
    if (!body) {
        binary_buffer_free(&request);
        return ESP_ERR_NO_MEM;
    }
    http_buffer_t response = {
        .data = body,
        .len = 0,
        .capacity = DIRECT_HTTP_MAX_BODY + 1,
    };
    esp_http_client_config_t config = {
        .url = voice_url,
        .timeout_ms = VOICE_GATEWAY_TIMEOUT_MS,
        .event_handler = direct_http_event_cb,
        .user_data = &response,
        .disable_auto_redirect = false,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncasecmp(voice_url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        http_body_free(body);
        binary_buffer_free(&request);
        return ESP_ERR_NO_MEM;
    }

    const char *boundary = "----watchlite-voice-boundary";
    char content_type[128] = {0};
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    if (g_app.cfg.api_key[0]) {
        char auth[224] = {0};
        snprintf(auth, sizeof(auth), "Bearer %s", g_app.cfg.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, (const char *)request.data, (int)request.len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    binary_buffer_free(&request);

    if (err == ESP_OK && status >= 200 && status < 300) {
        if (!parse_gateway_voice_response(body, result, error_out, error_out_len)) {
            err = ESP_FAIL;
        }
    } else {
        if (error_out && error_out_len) {
            char temp[192] = {0};
            if (body[0] &&
                parse_reply_response(body, temp, sizeof(temp), temp, sizeof(temp)) == ESP_OK) {
                copy_string(error_out, error_out_len, temp);
            } else if (body[0] && temp[0]) {
                copy_string(error_out, error_out_len, temp);
            } else if (err == ESP_OK) {
                snprintf(error_out, error_out_len, "语音网关 HTTP %d", status);
            } else {
                snprintf(error_out, error_out_len, "语音网关失败: %s", esp_err_to_name(err));
            }
        }
        err = (err == ESP_OK) ? ESP_FAIL : err;
    }

    http_body_free(body);
    return err;
}

static esp_err_t fetch_gateway_audio(const char *audio_url, binary_buffer_t *audio_out,
                                     char *error_out, size_t error_out_len)
{
    if (!audio_url || !audio_url[0] || !audio_out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(audio_out, 0, sizeof(*audio_out));
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    char url[256] = {0};
    if (strncasecmp(audio_url, "http://", 7) == 0 ||
        strncasecmp(audio_url, "https://", 8) == 0) {
        copy_string(url, sizeof(url), audio_url);
    } else if (!build_gateway_url(audio_url, url, sizeof(url))) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "无法生成语音音频地址");
        }
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = VOICE_AUDIO_TIMEOUT_MS,
        .event_handler = http_binary_event_cb,
        .user_data = audio_out,
        .disable_auto_redirect = false,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncasecmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    if (g_app.cfg.api_key[0]) {
        char auth[224] = {0};
        snprintf(auth, sizeof(auth), "Bearer %s", g_app.cfg.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "语音音频下载失败: %s", esp_err_to_name(err));
        }
        binary_buffer_free(audio_out);
        return err;
    }
    if (status < 200 || status >= 300) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "语音音频 HTTP %d", status);
        }
        binary_buffer_free(audio_out);
        return ESP_FAIL;
    }
    if (audio_out->truncated || !audio_out->data || audio_out->len == 0) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len,
                        audio_out->truncated ? "语音音频过大" : "语音音频为空");
        }
        binary_buffer_free(audio_out);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t http_binary_event_cb(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data && event->data_len > 0) {
        binary_buffer_t *buffer = (binary_buffer_t *)event->user_data;
        return binary_buffer_append(buffer, (const uint8_t *)event->data, (size_t)event->data_len);
    }
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
        if (chunk_size == UINT32_MAX || payload + chunk_size > len) {
            if (memcmp(chunk, "data", 4) == 0) {
                chunk_size = (uint32_t)(len - payload);
            } else {
                return false;
            }
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
    if (audio_format != 1 || out->fs.bits_per_sample != 16 ||
        out->fs.channel == 0 || out->fs.sample_rate == 0) {
        return false;
    }
    return out->data_offset + out->data_len <= len;
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
    } else if (cJSON_IsString(error) && error->valuestring) {
        copy_string(error_out, error_out_len, error->valuestring);
        return true;
    }
    return extract_reply_string_from_json(root, error_out, error_out_len);
}

static esp_err_t parse_reply_response(const char *body, char *reply_out, size_t reply_out_len,
                                      char *error_out, size_t error_out_len)
{
    if (!body || !reply_out || reply_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply_out[0] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "响应不是有效 JSON");
        }
        return ESP_FAIL;
    }

    if (!extract_reply_string_from_json(root, reply_out, reply_out_len)) {
        if (error_out && error_out_len &&
            !extract_error_string_from_json(root, error_out, error_out_len)) {
            copy_string(error_out, error_out_len, "找不到回复内容");
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t build_stt_multipart_body(const uint8_t *wav_data, size_t wav_len, binary_buffer_t *body)
{
    if (!wav_data || wav_len == 0 || !body) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *boundary = "----watchlite-stt-boundary";
    esp_err_t err = binary_buffer_append_format(body, "--%s\r\n", boundary);
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append_format(
        body,
        "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n");
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append_format(body, "--%s\r\n", boundary);
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append_format(
        body,
        "Content-Disposition: form-data; name=\"language\"\r\n\r\nzh\r\n");
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append_format(body, "--%s\r\n", boundary);
    if (err != ESP_OK) {
        return err;
    }
    err = binary_buffer_append_format(
        body,
        "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n中文普通话语音转写，只输出用户说的话，不要翻译。\r\n");
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

static esp_err_t perform_stt_request(const uint8_t *wav_data, size_t wav_len,
                                     char *text_out, size_t text_out_len,
                                     char *error_out, size_t error_out_len)
{
    if (!wav_data || wav_len == 0 || !text_out || text_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    text_out[0] = '\0';
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    char stt_url[224] = {0};
    build_api_endpoint_url("audio/transcriptions", stt_url, sizeof(stt_url));
    if (stt_url[0] == '\0') {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "无法生成语音转写地址");
        }
        return ESP_ERR_INVALID_ARG;
    }

    binary_buffer_t request = {0};
    esp_err_t err = build_stt_multipart_body(wav_data, wav_len, &request);
    if (err != ESP_OK) {
        binary_buffer_free(&request);
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "构建转写请求失败");
        }
        return err;
    }

    char *body = http_body_alloc(DIRECT_HTTP_MAX_BODY + 1);
    if (!body) {
        binary_buffer_free(&request);
        return ESP_ERR_NO_MEM;
    }
    http_buffer_t response = {
        .data = body,
        .len = 0,
        .capacity = DIRECT_HTTP_MAX_BODY + 1,
    };
    esp_http_client_config_t config = {
        .url = stt_url,
        .timeout_ms = VOICE_AI_TIMEOUT_MS,
        .event_handler = direct_http_event_cb,
        .user_data = &response,
        .disable_auto_redirect = false,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncasecmp(stt_url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        http_body_free(body);
        binary_buffer_free(&request);
        return ESP_ERR_NO_MEM;
    }

    const char *boundary = "----watchlite-stt-boundary";
    char content_type[128] = {0};
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    if (g_app.cfg.api_key[0]) {
        char auth[224] = {0};
        snprintf(auth, sizeof(auth), "Bearer %s", g_app.cfg.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, (const char *)request.data, (int)request.len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    binary_buffer_free(&request);

    if (err == ESP_OK && status >= 200 && status < 300) {
        err = parse_reply_response(body, text_out, text_out_len, error_out, error_out_len);
    } else {
        if (error_out && error_out_len) {
            if (body[0]) {
                char temp[160] = {0};
                if (parse_reply_response(body, temp, sizeof(temp), temp, sizeof(temp)) == ESP_OK || temp[0]) {
                    copy_string(error_out, error_out_len, temp);
                } else {
                    snprintf(error_out, error_out_len, "STT HTTP %d", status);
                }
            } else {
                snprintf(error_out, error_out_len, "STT 请求失败: %s", esp_err_to_name(err));
            }
        }
        err = (err == ESP_OK) ? ESP_FAIL : err;
    }

    http_body_free(body);
    return err;
}

static esp_err_t perform_tts_request(const char *text, binary_buffer_t *audio_out,
                                     char *error_out, size_t error_out_len)
{
    if (!text || !audio_out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(audio_out, 0, sizeof(*audio_out));
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    char tts_url[224] = {0};
    build_api_endpoint_url("audio/speech", tts_url, sizeof(tts_url));
    if (tts_url[0] == '\0') {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "无法生成语音合成地址");
        }
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", "gpt-4o-mini-tts");
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", "alloy");
    cJSON_AddStringToObject(root, "response_format", "wav");
    cJSON_AddNumberToObject(root, "speed", 1.0);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = tts_url,
        .timeout_ms = VOICE_AI_TIMEOUT_MS,
        .event_handler = http_binary_event_cb,
        .user_data = audio_out,
        .disable_auto_redirect = false,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncasecmp(tts_url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (g_app.cfg.api_key[0]) {
        char auth[224] = {0};
        snprintf(auth, sizeof(auth), "Bearer %s", g_app.cfg.api_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);

    if (err != ESP_OK) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "TTS 请求失败: %s", esp_err_to_name(err));
        }
        binary_buffer_free(audio_out);
        return err;
    }
    if (status < 200 || status >= 300) {
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "TTS HTTP %d", status);
        }
        binary_buffer_free(audio_out);
        return ESP_FAIL;
    }
    if (audio_out->truncated || !audio_out->data || audio_out->len == 0) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, audio_out->truncated ? "TTS 音频过大" : "TTS 返回为空");
        }
        binary_buffer_free(audio_out);
        return ESP_FAIL;
    }
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

    const size_t sample_bytes = VOICE_BITS_PER_SAMPLE / 8;
    const size_t output_frame_bytes = VOICE_CHANNELS * sample_bytes;
    const size_t capture_frame_bytes = VOICE_CAPTURE_CHANNELS * sample_bytes;
    size_t max_pcm_bytes = (size_t)VOICE_SAMPLE_RATE * output_frame_bytes * VOICE_MAX_SECONDS;
    size_t total_bytes = 44 + max_pcm_bytes;
    uint8_t *data = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_malloc(total_bytes, MALLOC_CAP_8BIT);
    }
    if (!data) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "分配录音缓冲失败");
        }
        return ESP_ERR_NO_MEM;
    }
    memset(data, 0, total_bytes);

    size_t capture_chunk_frames = VOICE_READ_CHUNK_BYTES / capture_frame_bytes;
    if (capture_chunk_frames < 256) {
        capture_chunk_frames = 256;
    }
    size_t capture_chunk_bytes = capture_chunk_frames * capture_frame_bytes;
    uint8_t *capture = heap_caps_malloc(capture_chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!capture) {
        capture = heap_caps_malloc(capture_chunk_bytes, MALLOC_CAP_8BIT);
    }
    if (!capture) {
        free(data);
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "分配录音临时缓冲失败");
        }
        return ESP_ERR_NO_MEM;
    }

    if (g_app.audio_lock) {
        xSemaphoreTake(g_app.audio_lock, portMAX_DELAY);
    }
    pmic_audio_power_set(true);

    if (!g_app.mic_dev) {
        g_app.mic_dev = bsp_audio_codec_microphone_init();
    }
    if (!g_app.mic_dev) {
        pmic_audio_power_set(false);
        gpio_set_level(BSP_POWER_AMP_IO, 0);
        if (g_app.audio_lock) {
            xSemaphoreGive(g_app.audio_lock);
        }
        free(capture);
        free(data);
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "麦克风初始化失败");
        }
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t capture_fs = {
        .bits_per_sample = VOICE_BITS_PER_SAMPLE,
        .channel = VOICE_CAPTURE_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = VOICE_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    esp_codec_dev_sample_info_t wav_fs = {
        .bits_per_sample = VOICE_BITS_PER_SAMPLE,
        .channel = VOICE_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = VOICE_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    int open_ret = esp_codec_dev_open(g_app.mic_dev, &capture_fs);
    if (open_ret != ESP_CODEC_DEV_OK) {
        pmic_audio_power_set(false);
        gpio_set_level(BSP_POWER_AMP_IO, 0);
        if (g_app.audio_lock) {
            xSemaphoreGive(g_app.audio_lock);
        }
        free(capture);
        free(data);
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "打开麦克风失败(%d)", open_ret);
        }
        return ESP_FAIL;
    }
    int mute_ret = esp_codec_dev_set_in_mute(g_app.mic_dev, false);
    if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mic unmute failed: %d", mute_ret);
    }
    int gain_ret = esp_codec_dev_set_in_gain(g_app.mic_dev, VOICE_MIC_GAIN_DB);
    if (gain_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "mic gain failed: %d", gain_ret);
    }

    size_t pcm_bytes = 0;
    bool speech_started = false;
    int idle_ms = 0;
    int total_ms = 0;
    int energy_log_ms = 0;
    int64_t max_energy = 0;
    int64_t max_left_energy = 0;
    int64_t max_right_energy = 0;
    int64_t noise_energy_sum = 0;
    uint32_t noise_energy_count = 0;
    uint32_t speech_candidate_chunks = 0;
    uint32_t left_chunks = 0;
    uint32_t right_chunks = 0;
    size_t min_pcm_bytes = ((size_t)VOICE_SAMPLE_RATE * output_frame_bytes * VOICE_MIN_RECORD_MS) / 1000;

    while (pcm_bytes < max_pcm_bytes) {
        size_t remain_frames = (max_pcm_bytes - pcm_bytes) / output_frame_bytes;
        size_t request_frames = MIN(capture_chunk_frames, remain_frames);
        size_t request = request_frames * capture_frame_bytes;
        if (request == 0) {
            break;
        }
        int read_ret = esp_codec_dev_read(g_app.mic_dev, capture, (int)request);
        if (read_ret != ESP_CODEC_DEV_OK) {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "录音失败(%d)", read_ret);
            }
            break;
        }
        size_t got_capture_bytes = request;
        size_t got_frames = got_capture_bytes / capture_frame_bytes;
        if (got_frames == 0) {
            continue;
        }
        if (got_frames > remain_frames) {
            got_frames = remain_frames;
        }
        int chunk_ms = (int)((got_frames * 1000u) / VOICE_SAMPLE_RATE);
        if (chunk_ms <= 0) {
            chunk_ms = 1;
        }

        const int16_t *capture_samples = (const int16_t *)capture;
        int64_t left_energy_sum = 0;
        int64_t right_energy_sum = 0;
        for (size_t i = 0; i < got_frames; ++i) {
            int32_t left = capture_samples[i * VOICE_CAPTURE_CHANNELS + 0];
            int32_t right = (VOICE_CAPTURE_CHANNELS > 1) ?
                capture_samples[i * VOICE_CAPTURE_CHANNELS + 1] : left;
            left_energy_sum += (left < 0) ? -(int64_t)left : (int64_t)left;
            right_energy_sum += (right < 0) ? -(int64_t)right : (int64_t)right;
        }
        int64_t left_energy = left_energy_sum / (int64_t)got_frames;
        int64_t right_energy = right_energy_sum / (int64_t)got_frames;
        bool use_right = (VOICE_CAPTURE_CHANNELS > 1) && right_energy > left_energy;
        if (use_right) {
            ++right_chunks;
        } else {
            ++left_chunks;
        }
        if (left_energy > max_left_energy) {
            max_left_energy = left_energy;
        }
        if (right_energy > max_right_energy) {
            max_right_energy = right_energy;
        }
        if (total_ms < VOICE_WARMUP_DISCARD_MS) {
            total_ms += chunk_ms;
            continue;
        }

        int16_t *out_samples = (int16_t *)(data + 44 + pcm_bytes);
        for (size_t i = 0; i < got_frames; ++i) {
            out_samples[i] = capture_samples[i * VOICE_CAPTURE_CHANNELS + (use_right ? 1 : 0)];
        }

        size_t got_bytes = got_frames * output_frame_bytes;
        pcm_bytes += got_bytes;

        size_t sample_count = got_bytes / 2;
        int64_t energy = pcm_chunk_energy_16bit(out_samples, sample_count);
        if (energy > max_energy) {
            max_energy = energy;
        }
        total_ms += chunk_ms;
        energy_log_ms += chunk_ms;
        int64_t vad_threshold = VOICE_SPEECH_THRESHOLD;
        if (!speech_started && total_ms <= VOICE_NOISE_CALIBRATE_MS) {
            noise_energy_sum += energy;
            ++noise_energy_count;
        }
        if (noise_energy_count > 0) {
            int64_t noise_floor = noise_energy_sum / (int64_t)noise_energy_count;
            int64_t adaptive_threshold = noise_floor * 2 + 260;
            if (adaptive_threshold > vad_threshold) {
                vad_threshold = adaptive_threshold;
            }
        }
        if (vad_threshold > VOICE_FORCE_SPEECH_THRESHOLD) {
            vad_threshold = VOICE_FORCE_SPEECH_THRESHOLD;
        }

        if (energy_log_ms >= VOICE_ENERGY_LOG_MS) {
            energy_log_ms = 0;
            ESP_LOGI(TAG, "voice rec: ms=%d energy=%lld threshold=%lld L=%lld R=%lld max=%lld speech=%d",
                     total_ms, (long long)energy, (long long)vad_threshold,
                     (long long)left_energy, (long long)right_energy,
                     (long long)max_energy, speech_started ? 1 : 0);
        }

        bool speech_chunk = energy >= vad_threshold || energy >= VOICE_FORCE_SPEECH_THRESHOLD;
        if (!speech_started) {
            if (speech_chunk) {
                ++speech_candidate_chunks;
                if (speech_candidate_chunks >= VOICE_SPEECH_START_CHUNKS) {
                    speech_started = true;
                    idle_ms = 0;
                }
            } else {
                speech_candidate_chunks = 0;
            }
        } else if (speech_chunk) {
            idle_ms = 0;
        } else {
            idle_ms += chunk_ms;
        }

        if (!speech_started && total_ms >= VOICE_MAX_IDLE_MS) {
            break;
        }
        if (speech_started && pcm_bytes >= min_pcm_bytes && idle_ms >= VOICE_SILENCE_MS) {
            break;
        }
    }

    esp_codec_dev_close(g_app.mic_dev);
    gpio_set_level(BSP_POWER_AMP_IO, 0);
    pmic_audio_power_set(false);
    if (g_app.audio_lock) {
        xSemaphoreGive(g_app.audio_lock);
    }
    free(capture);

    ESP_LOGI(TAG, "voice rec: done ms=%d bytes=%u max=%lld Lmax=%lld Rmax=%lld pick=L%u/R%u speech=%d",
             total_ms, (unsigned)pcm_bytes, (long long)max_energy,
             (long long)max_left_energy, (long long)max_right_energy,
             (unsigned)left_chunks, (unsigned)right_chunks, speech_started ? 1 : 0);

    if (!speech_started) {
        free(data);
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "没有检测到语音，请靠近一点再试");
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (pcm_bytes == 0) {
        free(data);
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "录音为空");
        }
        return ESP_FAIL;
    }

    esp_err_t header_err = build_wav_header(data, pcm_bytes, &wav_fs);
    if (header_err != ESP_OK) {
        free(data);
        return header_err;
    }
    wav_out->data = data;
    wav_out->len = pcm_bytes + 44;
    wav_out->cap = total_bytes;
    wav_out->truncated = false;
    return ESP_OK;
}

static esp_err_t play_wav_audio(uint8_t *data, size_t len, char *error_out, size_t error_out_len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    wav_audio_info_t wav = {0};
    if (!parse_wav_audio(data, len, &wav)) {
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "TTS 返回的不是 16-bit WAV");
        }
        return ESP_FAIL;
    }

    if (g_app.audio_lock) {
        xSemaphoreTake(g_app.audio_lock, portMAX_DELAY);
    }
    pmic_audio_power_set(true);
    if (!g_app.speaker_dev) {
        g_app.speaker_dev = bsp_audio_codec_speaker_init();
    }
    if (!g_app.speaker_dev) {
        pmic_audio_power_set(false);
        gpio_set_level(BSP_POWER_AMP_IO, 0);
        if (g_app.audio_lock) {
            xSemaphoreGive(g_app.audio_lock);
        }
        if (error_out && error_out_len) {
            copy_string(error_out, error_out_len, "扬声器初始化失败");
        }
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {0};
    build_wav_sample_info(&wav, &fs);
    int open_ret = esp_codec_dev_open(g_app.speaker_dev, &fs);
    if (open_ret != ESP_CODEC_DEV_OK) {
        pmic_audio_power_set(false);
        gpio_set_level(BSP_POWER_AMP_IO, 0);
        if (g_app.audio_lock) {
            xSemaphoreGive(g_app.audio_lock);
        }
        if (error_out && error_out_len) {
            snprintf(error_out, error_out_len, "打开扬声器失败(%d)", open_ret);
        }
        return ESP_FAIL;
    }

    int vol_ret = esp_codec_dev_set_out_vol(g_app.speaker_dev, VOICE_PLAY_VOLUME);
    if (vol_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker volume failed: %d", vol_ret);
    }

    esp_err_t err = ESP_OK;
    size_t offset = wav.data_offset;
    size_t end = wav.data_offset + wav.data_len;
    while (offset < end) {
        if (g_app.ai_tts_stop_requested) {
            ESP_LOGI(TAG, "voice: play interrupted");
            err = ESP_ERR_INVALID_STATE;
            if (error_out && error_out_len) {
                copy_string(error_out, error_out_len, "播放已打断");
            }
            break;
        }
        size_t chunk = MIN((size_t)4096, end - offset);
        int write_ret = esp_codec_dev_write(g_app.speaker_dev, data + offset, (int)chunk);
        if (write_ret != ESP_CODEC_DEV_OK) {
            if (error_out && error_out_len) {
                snprintf(error_out, error_out_len, "播放失败(%d)", write_ret);
            }
            err = ESP_FAIL;
            break;
        }
        offset += chunk;
    }

    esp_codec_dev_close(g_app.speaker_dev);
    gpio_set_level(BSP_POWER_AMP_IO, 0);
    pmic_audio_power_set(false);
    if (g_app.audio_lock) {
        xSemaphoreGive(g_app.audio_lock);
    }
    return err;
}

static void format_price(const char *symbol, double value, char *out, size_t out_size)
{
    (void)symbol;
    if (value <= 0.0) {
        copy_string(out, out_size, "--");
    } else if (value >= 100000.0) {
        snprintf(out, out_size, "%.1fk", value / 1000.0);
    } else if (value >= 1000.0) {
        snprintf(out, out_size, "%.0f", value);
    } else if (value >= 100.0) {
        snprintf(out, out_size, "%.1f", value);
    } else {
        snprintf(out, out_size, "%.2f", value);
    }
}

static void format_change(double value, char *out, size_t out_size)
{
    snprintf(out, out_size, "%+.1f%%", value);
}

static void mark_market_time_now(void)
{
    time_t now = 0;
    struct tm tm_now = {0};
    time(&now);
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year >= 120) {
        snprintf(g_app.market_time, sizeof(g_app.market_time), "%02d:%02d",
                 tm_now.tm_hour, tm_now.tm_min);
    }
}

static bool clock_is_valid(void)
{
    time_t now = 0;
    struct tm tm_now = {0};
    time(&now);
    localtime_r(&now, &tm_now);
    return tm_now.tm_year >= 120;
}

static bool clock_is_ready_for_tls(void)
{
    if (!clock_is_valid()) {
        return false;
    }
    if (g_app.sntp_started &&
        esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        return false;
    }
    return true;
}

static bool api_url_uses_https(void)
{
    return strncasecmp(g_app.cfg.server_url, "https://", 8) == 0;
}

static void wait_for_clock_once(int timeout_ms)
{
    int waited_ms = 0;
    while (!clock_is_ready_for_tls() && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }
}

static bool parse_yahoo_chart(const char *body, int row_index, bool update_chart)
{
    if (!body || row_index < 0 || row_index >= MARKET_ROW_COUNT) {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }

    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *results = cJSON_IsObject(chart) ? cJSON_GetObjectItemCaseSensitive(chart, "result") : NULL;
    cJSON *result = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
    cJSON *meta = cJSON_IsObject(result) ? cJSON_GetObjectItemCaseSensitive(result, "meta") : NULL;
    cJSON *indicators = cJSON_IsObject(result) ? cJSON_GetObjectItemCaseSensitive(result, "indicators") : NULL;
    cJSON *quotes = cJSON_IsObject(indicators) ? cJSON_GetObjectItemCaseSensitive(indicators, "quote") : NULL;
    cJSON *quote = cJSON_IsArray(quotes) ? cJSON_GetArrayItem(quotes, 0) : NULL;
    cJSON *close_array = cJSON_IsObject(quote) ? cJSON_GetObjectItemCaseSensitive(quote, "close") : NULL;
    if (!cJSON_IsArray(close_array)) {
        cJSON_Delete(root);
        return false;
    }

    double points[CHART_POINT_COUNT] = {0};
    int point_count = 0;
    int close_count = cJSON_GetArraySize(close_array);
    for (int i = 0; i < close_count; i++) {
        cJSON *item = cJSON_GetArrayItem(close_array, i);
        if (!cJSON_IsNumber(item) || item->valuedouble <= 0.0) {
            continue;
        }
        if (point_count < CHART_POINT_COUNT) {
            points[point_count++] = item->valuedouble;
        } else {
            memmove(points, points + 1, sizeof(double) * (CHART_POINT_COUNT - 1));
            points[CHART_POINT_COUNT - 1] = item->valuedouble;
        }
    }

    cJSON *regular_price = cJSON_IsObject(meta) ?
        cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice") : NULL;
    double price = cJSON_IsNumber(regular_price) ? regular_price->valuedouble : 0.0;
    if (price <= 0.0 && point_count > 0) {
        price = points[point_count - 1];
    }
    if (price <= 0.0 || point_count <= 0) {
        cJSON_Delete(root);
        return false;
    }

    points[point_count - 1] = price;
    cJSON *chart_previous = cJSON_IsObject(meta) ?
        cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose") : NULL;
    double previous = cJSON_IsNumber(chart_previous) ? chart_previous->valuedouble : 0.0;
    if (update_chart && point_count >= 2) {
        previous = points[0];
    } else if (previous <= 0.0 && point_count >= 2) {
        previous = points[0];
    }

    char price_text[24] = {0};
    char change_text[16] = {0};
    format_price(MARKET_DEFS[row_index].code, price, price_text, sizeof(price_text));
    if (previous > 0.0) {
        format_change(((price - previous) / previous) * 100.0, change_text, sizeof(change_text));
    } else {
        copy_string(change_text, sizeof(change_text), "--");
    }

    copy_string(g_app.market_rows[row_index].price, sizeof(g_app.market_rows[row_index].price),
                price_text);
    copy_string(g_app.market_rows[row_index].change, sizeof(g_app.market_rows[row_index].change),
                change_text);
    if (update_chart) {
        memcpy(g_app.chart_values, points, sizeof(double) * point_count);
        g_app.chart_point_count = point_count;
    }

    cJSON_Delete(root);
    return true;
}

static bool parse_coingecko_home_market(const char *body)
{
    if (!body || !body[0]) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }

    struct {
        const char *json_key;
        int row_index;
    } coins[] = {
        {"bitcoin", 0},
        {"ethereum", 1},
    };

    int ok_count = 0;
    for (size_t i = 0; i < sizeof(coins) / sizeof(coins[0]); i++) {
        cJSON *coin = cJSON_GetObjectItemCaseSensitive(root, coins[i].json_key);
        cJSON *usd = cJSON_IsObject(coin) ? cJSON_GetObjectItemCaseSensitive(coin, "usd") : NULL;
        cJSON *change = cJSON_IsObject(coin) ? cJSON_GetObjectItemCaseSensitive(coin, "usd_24h_change") : NULL;
        if (!cJSON_IsNumber(usd) || usd->valuedouble <= 0.0) {
            continue;
        }
        char price_text[24] = {0};
        char change_text[16] = {0};
        format_price(MARKET_DEFS[coins[i].row_index].code, usd->valuedouble,
                     price_text, sizeof(price_text));
        if (cJSON_IsNumber(change)) {
            format_change(change->valuedouble, change_text, sizeof(change_text));
        } else {
            copy_string(change_text, sizeof(change_text), "--");
        }
        copy_string(g_app.market_rows[coins[i].row_index].price,
                    sizeof(g_app.market_rows[coins[i].row_index].price), price_text);
        copy_string(g_app.market_rows[coins[i].row_index].change,
                    sizeof(g_app.market_rows[coins[i].row_index].change), change_text);
        ok_count++;
    }

    cJSON_Delete(root);
    return ok_count > 0;
}

static int refresh_crypto_from_coingecko(char *body, size_t body_size)
{
    memset(body, 0, body_size);
    esp_err_t err = direct_http_request(COINGECKO_MARKET_URL, NULL, DIRECT_HTTP_TIMEOUT_MS,
                                        false, body, body_size);
    if (err != ESP_OK || !parse_coingecko_home_market(body)) {
        return 0;
    }
    mark_market_time_now();
    return 2;
}

static bool parse_gateway_market_full(const char *body)
{
    if (!body || !body[0]) {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsArray(assets)) {
        cJSON_Delete(root);
        return false;
    }

    int ok_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, assets) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(item, "price");
        cJSON *change = cJSON_GetObjectItemCaseSensitive(item, "change");
        if (!cJSON_IsString(code) || !cJSON_IsString(price) || !cJSON_IsString(change)) {
            continue;
        }
        bool has_price = price->valuestring[0] != '\0' && strcmp(price->valuestring, "--") != 0;
        for (int i = 0; i < MARKET_ROW_COUNT; i++) {
            if (strcmp(code->valuestring, MARKET_DEFS[i].code) == 0) {
                copy_string(g_app.market_rows[i].price, sizeof(g_app.market_rows[i].price), price->valuestring);
                copy_string(g_app.market_rows[i].change, sizeof(g_app.market_rows[i].change), change->valuestring);
                if (has_price) {
                    ok_count++;
                }
                break;
            }
        }
    }

    cJSON *market_time = cJSON_GetObjectItemCaseSensitive(root, "market_time");
    if (cJSON_IsString(market_time) && market_time->valuestring[0]) {
        copy_string(g_app.market_time, sizeof(g_app.market_time), market_time->valuestring);
    } else if (ok_count > 0) {
        mark_market_time_now();
    }
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (cJSON_IsString(status) && status->valuestring[0]) {
        copy_string(g_app.market_status, sizeof(g_app.market_status), status->valuestring);
    } else if (ok_count > 0) {
        snprintf(g_app.market_status, sizeof(g_app.market_status), "GW %d", ok_count);
    }
    cJSON_Delete(root);
    return ok_count > 0;
}

static bool refresh_market_from_gateway(char *body, size_t body_size)
{
    if (!api_url_is_local_gateway()) {
        return false;
    }
    char url[224] = {0};
    if (!build_gateway_url("/api/watch/market/full", url, sizeof(url))) {
        return false;
    }
    memset(body, 0, body_size);
    esp_err_t err = direct_http_request(url, NULL, DIRECT_HTTP_TIMEOUT_MS,
                                        true, body, body_size);
    return err == ESP_OK && parse_gateway_market_full(body);
}

static void home_market_refresh_task(void *arg)
{
    (void)arg;
    if (g_app.net_lock) {
        xSemaphoreTake(g_app.net_lock, portMAX_DELAY);
    }

    copy_string(g_app.market_status, sizeof(g_app.market_status), "WiFi");
    ui_refresh_now();
    esp_err_t net_err = wifi_sta_connect_once(WIFI_CONNECT_WAIT_MS);
    if (net_err != ESP_OK) {
        copy_string(g_app.market_status, sizeof(g_app.market_status), "NO WIFI");
        g_app.market_refresh_inflight = false;
        g_app.market_task_started_us = 0;
        wifi_sta_disconnect_when_idle();
        if (g_app.net_lock) {
            xSemaphoreGive(g_app.net_lock);
        }
        ui_refresh_now();
        vTaskDelete(NULL);
        return;
    }

    wait_for_clock_once(12000);

    char *body = http_body_alloc(DIRECT_HTTP_MAX_BODY + 1);
    if (!body) {
        copy_string(g_app.market_status, sizeof(g_app.market_status), "NO MEM");
        g_app.market_refresh_inflight = false;
        g_app.market_task_started_us = 0;
        wifi_sta_disconnect_when_idle();
        if (g_app.net_lock) {
            xSemaphoreGive(g_app.net_lock);
        }
        ui_refresh_now();
        vTaskDelete(NULL);
        return;
    }

    int ok_count = refresh_crypto_from_coingecko(body, DIRECT_HTTP_MAX_BODY + 1);
    if (ok_count > 0) {
        snprintf(g_app.market_status, sizeof(g_app.market_status), "CG %d", ok_count);
        ui_refresh_now();
    }

    int yahoo_tries = 0;
    const int yahoo_start_index = ok_count > 0 ? 2 : 0;
    int yahoo_index = ok_count > 0 ? g_app.market_yahoo_next_index : 0;
    if (yahoo_index < yahoo_start_index || yahoo_index >= MARKET_ROW_COUNT) {
        yahoo_index = yahoo_start_index;
    }
    for (int visited = 0; visited < MARKET_ROW_COUNT - yahoo_start_index; visited++) {
        if (yahoo_tries >= MARKET_HOME_YAHOO_MAX_PER_REFRESH) {
            break;
        }
        int i = yahoo_index + visited;
        if (i >= MARKET_ROW_COUNT) {
            i = yahoo_start_index + (i - MARKET_ROW_COUNT);
        }
        char url[192] = {0};
        snprintf(url, sizeof(url), YAHOO_HOME_URL_FMT, MARKET_DEFS[i].chart_symbol);
        memset(body, 0, DIRECT_HTTP_MAX_BODY + 1);
        yahoo_tries++;
        esp_err_t err = direct_http_request(url, NULL, DIRECT_HTTP_TIMEOUT_MS,
                                            false, body, DIRECT_HTTP_MAX_BODY + 1);
        if (err == ESP_OK && parse_yahoo_chart(body, i, false)) {
            ok_count++;
            snprintf(g_app.market_status, sizeof(g_app.market_status), "%d/%d", ok_count, MARKET_ROW_COUNT);
            ui_refresh_now();
        }
    }
    if (ok_count > 0 && yahoo_tries > 0) {
        g_app.market_yahoo_next_index = yahoo_index + yahoo_tries;
        if (g_app.market_yahoo_next_index >= MARKET_ROW_COUNT) {
            g_app.market_yahoo_next_index = 2;
        }
    }

    if (ok_count > 0) {
        mark_market_time_now();
        g_app.market_polled_once = true;
        snprintf(g_app.market_status, sizeof(g_app.market_status), "DIR %d", ok_count);
        (void)market_cache_save();
    } else if (refresh_market_from_gateway(body, DIRECT_HTTP_MAX_BODY + 1)) {
        g_app.market_polled_once = true;
        (void)market_cache_save();
    } else {
        copy_string(g_app.market_status, sizeof(g_app.market_status), "MKT FAIL");
    }

    http_body_free(body);
    g_app.market_refresh_inflight = false;
    g_app.market_task_started_us = 0;
    wifi_sta_disconnect_when_idle();
    if (g_app.net_lock) {
        xSemaphoreGive(g_app.net_lock);
    }
    ui_refresh_now();
    vTaskDelete(NULL);
}

static void home_market_refresh_async(void)
{
    int64_t now_us = esp_timer_get_time();
    if (g_app.market_refresh_inflight || g_app.chart_inflight ||
        g_app.ai_start_inflight || g_app.ai_tts_inflight ||
        g_app.ai_playback_inflight) {
        static int64_t last_busy_log_us;
        if (now_us - last_busy_log_us > 10000000LL) {
            last_busy_log_us = now_us;
            ESP_LOGW(TAG, "home refresh skipped: busy market=%d chart=%d ai=%d tts=%d play=%d",
                     g_app.market_refresh_inflight, g_app.chart_inflight,
                     g_app.ai_start_inflight, g_app.ai_tts_inflight,
                     g_app.ai_playback_inflight);
        }
        return;
    }
    if (!g_app.wifi_started) {
        static int64_t last_no_wifi_start_log_us;
        if (now_us - last_no_wifi_start_log_us > 10000000LL) {
            last_no_wifi_start_log_us = now_us;
            ESP_LOGW(TAG, "home refresh skipped: WiFi not started");
        }
        copy_string(g_app.market_status, sizeof(g_app.market_status), "NO WIFI");
        ui_refresh_now();
        return;
    }
    g_app.sta_configured = config_has_wifi(&g_app.cfg);
    if (!g_app.sta_configured) {
        static int64_t last_no_cfg_log_us;
        if (now_us - last_no_cfg_log_us > 10000000LL) {
            last_no_cfg_log_us = now_us;
            ESP_LOGW(TAG, "home refresh skipped: no STA WiFi config");
        }
        copy_string(g_app.market_status, sizeof(g_app.market_status), "NO CFG");
        ui_refresh_now();
        return;
    }
    g_app.last_market_refresh_us = now_us;
    g_app.market_refresh_inflight = true;
    g_app.market_task_started_us = now_us;
    copy_string(g_app.market_status, sizeof(g_app.market_status), "Starting");
    if (xTaskCreate(home_market_refresh_task, "home_market", 12288, NULL, 4, NULL) != pdPASS) {
        g_app.market_refresh_inflight = false;
        g_app.market_task_started_us = 0;
        copy_string(g_app.market_status, sizeof(g_app.market_status), "NO TASK");
    }
}

static esp_err_t perform_chat_request(const char *user_text, char *reply_out, size_t reply_out_len,
                                      char *error_out, size_t error_out_len)
{
    if (!user_text || !reply_out || reply_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply_out[0] = '\0';
    if (error_out && error_out_len) {
        error_out[0] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *system = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (!root || !messages || !system || !user) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", g_app.cfg.model);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "temperature", 0.4);
    cJSON_AddNumberToObject(root, "max_tokens", 80);
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", "你是手表里的中文助手，回复要短、直接、自然，通常不超过30个汉字；只用常用简体中文，不用 emoji、繁体字或生僻字。");
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_text);
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToArray(messages, user);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    char url[224] = {0};
    build_api_endpoint_url("chat/completions", url, sizeof(url));
    char *body = http_body_alloc(DIRECT_HTTP_MAX_BODY + 1);
    if (!body) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = direct_http_request(url, payload, DIRECT_AI_TIMEOUT_MS, true,
                                        body, DIRECT_HTTP_MAX_BODY + 1);
    free(payload);
    if (err == ESP_OK) {
        err = parse_reply_response(body, reply_out, reply_out_len, error_out, error_out_len);
    } else if (body[0] && error_out && error_out_len) {
        char temp[192] = {0};
        if (parse_reply_response(body, temp, sizeof(temp), temp, sizeof(temp)) == ESP_OK || temp[0]) {
            copy_string(error_out, error_out_len, temp);
        }
    }
    http_body_free(body);
    return err;
}

static void voice_tts_task(void *arg)
{
    voice_tts_job_t *job = (voice_tts_job_t *)arg;
    bool created_with_caps = job ? job->created_with_caps : false;
    bool lock_taken = false;
    binary_buffer_t audio = {0};
    char error_text[192] = {0};
    esp_err_t err = ESP_OK;

    if (!job) {
        goto finish;
    }

    ESP_LOGI(TAG, "voice: background tts start");
    if (g_app.net_lock) {
        xSemaphoreTake(g_app.net_lock, portMAX_DELAY);
        lock_taken = true;
    }

    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "TTS");
    ui_refresh_now();

    if (!g_app.sta_connected) {
        ESP_LOGI(TAG, "voice: tts wifi connect");
        err = wifi_sta_connect_once(WIFI_CONNECT_WAIT_MS);
        if (err != ESP_OK) {
            snprintf(error_text, sizeof(error_text), "WiFi: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK && api_url_uses_https()) {
        wait_for_clock_once(12000);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "voice: tts request start");
        err = perform_tts_request(job->reply, &audio, error_text, sizeof(error_text));
    }

    if (lock_taken) {
        wifi_sta_disconnect_when_idle();
        xSemaphoreGive(g_app.net_lock);
        lock_taken = false;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice: tts failed: %s err=%s",
                 error_text[0] ? error_text : "tts failed", esp_err_to_name(err));
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "TTS ERR");
        goto finish;
    }

    ESP_LOGI(TAG, "voice: tts request done bytes=%u", (unsigned)audio.len);
    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "PLAY");
    ui_refresh_now();

    ESP_LOGI(TAG, "voice: play start");
    err = play_wav_audio(audio.data, audio.len, error_text, sizeof(error_text));
    if (err != ESP_OK) {
        if (g_app.ai_tts_stop_requested) {
            ESP_LOGI(TAG, "voice: tts interrupted by new recording");
            copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
            goto finish;
        }
        ESP_LOGE(TAG, "voice: play failed: %s err=%s",
                 error_text[0] ? error_text : "play failed", esp_err_to_name(err));
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "PLAY ERR");
        goto finish;
    }

    ESP_LOGI(TAG, "voice: background tts done");
    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");

finish:
    binary_buffer_free(&audio);
    if (lock_taken) {
        wifi_sta_disconnect_when_idle();
        xSemaphoreGive(g_app.net_lock);
    }
    g_app.ai_tts_inflight = false;
    g_app.tts_task_started_us = 0;
    g_app.ai_tts_stop_requested = false;
    ui_refresh_now();
    free(job);
    if (created_with_caps) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        vTaskDeleteWithCaps(NULL);
#else
        vTaskDelete(NULL);
#endif
    } else {
        vTaskDelete(NULL);
    }
}

static bool schedule_tts_reply(const char *reply)
{
    if (!reply || reply[0] == '\0') {
        return false;
    }
    if (g_app.ai_tts_inflight) {
        ESP_LOGW(TAG, "voice: tts already running, skip");
        return false;
    }

    voice_tts_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "TTS ERR");
        return false;
    }
    copy_ai_text(job->reply, sizeof(job->reply), reply);

    g_app.ai_tts_inflight = true;
    g_app.tts_task_started_us = esp_timer_get_time();
    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "TTS");
    ui_refresh_now();

    BaseType_t task_ok = pdFAIL;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    job->created_with_caps = true;
    task_ok = xTaskCreateWithCaps(voice_tts_task, "voice_tts",
                                  VOICE_TTS_TASK_STACK_BYTES, job, 4, NULL,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "voice: psram tts task create failed, trying internal stack");
        job->created_with_caps = false;
        task_ok = xTaskCreate(voice_tts_task, "voice_tts",
                              VOICE_TTS_TASK_FALLBACK_STACK_BYTES, job, 4, NULL);
    }
    if (task_ok != pdPASS) {
        g_app.ai_tts_inflight = false;
        g_app.tts_task_started_us = 0;
        free(job);
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "TTS ERR");
        log_voice_task_heap("tts task create failed");
        ui_refresh_now();
        return false;
    }
    return true;
}

static bool ascii_contains_case_insensitive(const char *text, const char *needle)
{
    if (!text || !needle || needle[0] == '\0') {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static int market_index_from_voice_text(const char *text)
{
    if (!text || text[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < MARKET_ROW_COUNT; ++i) {
        if (ascii_contains_case_insensitive(text, MARKET_DEFS[i].code)) {
            return i;
        }
    }
    if (strstr(text, "比特币") || strstr(text, "底特币") || strstr(text, "笔特币") ||
        strstr(text, "必特币") || strstr(text, "币特币") || strstr(text, "逼特币") ||
        ascii_contains_case_insensitive(text, "bitcoin")) return 0;
    if (strstr(text, "以太坊") || strstr(text, "以太") || strstr(text, "一太坊") ||
        strstr(text, "以太方") || strstr(text, "以太房") ||
        ascii_contains_case_insensitive(text, "ethereum")) return 1;
    if (strstr(text, "黄金") || strstr(text, "金价")) return 2;
    if (strstr(text, "原油") || strstr(text, "油价")) return 3;
    if (strstr(text, "美元指数")) return 4;
    if (strstr(text, "人民币") || strstr(text, "美元人民币")) return 5;
    if (strstr(text, "欧元")) return 6;
    if (strstr(text, "恐慌") || strstr(text, "波动率")) return 7;
    if (strstr(text, "上证") || strstr(text, "A股") || strstr(text, "沪指")) return 8;
    if (strstr(text, "恒生") || strstr(text, "港股")) return 9;
    if (strstr(text, "日经")) return 10;
    if (strstr(text, "德国") || strstr(text, "德指")) return 11;
    if (strstr(text, "英国") || strstr(text, "富时")) return 12;
    if (strstr(text, "标普") || strstr(text, "美股")) return 13;
    if (strstr(text, "纳指") || strstr(text, "纳斯达克")) return 14;
    return -1;
}

static int market_index_from_asset_code(const char *asset)
{
    if (!asset || asset[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < MARKET_ROW_COUNT; ++i) {
        if (strcasecmp(asset, MARKET_DEFS[i].code) == 0) {
            return i;
        }
    }
    return market_index_from_voice_text(asset);
}

static void execute_gateway_voice_action(const voice_gateway_result_t *result)
{
    (void)result;
    /* Voice mode is pure chat: do not let model/gateway responses operate the watch. */
    return;

    if (!result || result->action[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "voice: action=%s asset=%s brightness_delta=%d",
             result->action, result->asset, result->brightness_delta);

    if (strcmp(result->action, "sleep") == 0) {
        set_sleeping(true);
        return;
    }

    wake_screen_for_input();

    if (strcmp(result->action, "brightness") == 0) {
        int next = g_app.cfg.brightness_percent + result->brightness_delta;
        if (next < 5) {
            next = 5;
        } else if (next > 100) {
            next = 100;
        }
        if (next != g_app.cfg.brightness_percent) {
            g_app.cfg.brightness_percent = next;
            restore_display_brightness();
            esp_err_t save_err = config_save(&g_app.cfg);
            if (save_err != ESP_OK) {
                ESP_LOGW(TAG, "voice: save brightness failed: %s", esp_err_to_name(save_err));
            }
        } else {
            restore_display_brightness();
        }
        return;
    }

    if (strcmp(result->action, "refresh_market") == 0) {
        home_market_refresh_async();
        return;
    }

    bool chart_action = strcmp(result->action, "chart") == 0 ||
                        strcmp(result->action, "chart_refresh") == 0;
    if (chart_action) {
        int row_index = market_index_from_asset_code(result->asset);
        if (row_index < 0) {
            row_index = market_index_from_voice_text(result->transcript);
        }
        if (row_index < 0) {
            ESP_LOGW(TAG, "voice: chart action missing valid asset");
            return;
        }

        if (bsp_display_lock(1000)) {
            ui_show_chart(row_index);
            chart_refresh_async(row_index);
            bsp_display_unlock();
        }
        return;
    }

    if (strcmp(result->action, "home") == 0) {
        if (bsp_display_lock(1000)) {
            ui_show_home();
            bsp_display_unlock();
        }
        return;
    }

    if (strcmp(result->action, "ai") == 0) {
        if (bsp_display_lock(1000)) {
            ui_show_ai();
            bsp_display_unlock();
        }
        return;
    }

    ESP_LOGW(TAG, "voice: unknown action=%s", result->action);
}

static bool build_local_voice_reply(const char *text, char *reply_out, size_t reply_len,
                                    bool *refresh_market_out)
{
    (void)text;
    (void)reply_out;
    (void)reply_len;
    (void)refresh_market_out;
    return false;

    if (!text || !reply_out || reply_len == 0) {
        return false;
    }
    if (refresh_market_out) {
        *refresh_market_out = false;
    }
    reply_out[0] = '\0';

    if (strstr(text, "刷新") &&
        (strstr(text, "行情") || strstr(text, "市场") || strstr(text, "价格"))) {
        if (refresh_market_out) {
            *refresh_market_out = true;
        }
        copy_string(reply_out, reply_len, "正在刷新行情，请稍等。");
        return true;
    }

    if (strstr(text, "几点") || strstr(text, "时间")) {
        time_t now = 0;
        struct tm tm_now = {0};
        time(&now);
        localtime_r(&now, &tm_now);
        if (tm_now.tm_year >= 120) {
            snprintf(reply_out, reply_len, "现在 %02d:%02d。", tm_now.tm_hour, tm_now.tm_min);
        } else {
            copy_string(reply_out, reply_len, "时间还没同步好。");
        }
        return true;
    }

    if (strstr(text, "电量") || strstr(text, "电池")) {
        if (g_app.battery_percent >= 0) {
            snprintf(reply_out, reply_len, "电量 %d%%。", g_app.battery_percent);
        } else {
            copy_string(reply_out, reply_len, "暂时读不到电量。");
        }
        return true;
    }

    int market_index = market_index_from_voice_text(text);
    if (market_index >= 0) {
        market_row_t *row = &g_app.market_rows[market_index];
        if (strcmp(row->price, "--") != 0) {
            snprintf(reply_out, reply_len, "%s 现在 %s，涨跌 %s，更新时间 %s。",
                     MARKET_DEFS[market_index].code,
                     row->price,
                     row->change[0] ? row->change : "--",
                     g_app.market_time[0] ? g_app.market_time : "--:--");
        } else {
            if (refresh_market_out) {
                *refresh_market_out = true;
            }
            snprintf(reply_out, reply_len, "%s 暂无行情，我先刷新。",
                     MARKET_DEFS[market_index].code);
        }
        return true;
    }

    return false;
}

static bool ai_task_generation_is_current(uint32_t generation)
{
    return generation != 0 && generation == g_app.ai_task_generation;
}

static void ai_task_set_state_if_current(uint32_t generation, const char *state)
{
    if (ai_task_generation_is_current(generation)) {
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), state);
    }
}

static void ai_task_set_reply_if_current(uint32_t generation, const char *reply)
{
    if (ai_task_generation_is_current(generation)) {
        copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), reply);
    }
}

static void direct_ai_start_task(void *arg)
{
    uintptr_t task_token = (uintptr_t)arg;
    bool created_with_caps = (task_token & 1u) != 0u;
    uint32_t generation = (uint32_t)(task_token >> 1);
    bool lock_taken = false;
    bool refresh_market_after_done = false;
    bool tts_after_done = false;
    binary_buffer_t wav = {0};
    binary_buffer_t gateway_audio = {0};
    voice_gateway_result_t gateway_result = {0};
    char error_text[192] = {0};
    char transcript[192] = {0};
    char reply[512] = {0};
    char deferred_tts_reply[512] = {0};

    if (generation == 0u) {
        generation = g_app.ai_task_generation;
    }
    ESP_LOGI(TAG, "voice: task start");
    ai_task_set_state_if_current(generation, "REC");
    copy_ai_text(g_app.ai_user, sizeof(g_app.ai_user), "");
    ai_task_set_reply_if_current(generation, "请直接说话...");
    ui_refresh_now();

    ESP_LOGI(TAG, "voice: record start");
    esp_err_t err = record_voice_wav(&wav, error_text, sizeof(error_text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice: record failed: %s err=%s",
                 error_text[0] ? error_text : "record failed", esp_err_to_name(err));
        if (err == ESP_ERR_NOT_FOUND) {
            ai_task_set_state_if_current(generation, "READY");
            ai_task_set_reply_if_current(generation,
                                         error_text[0] ? error_text : "没有检测到语音，请再说一遍");
            goto done;
        }
        char display_error[256] = {0};
        snprintf(display_error, sizeof(display_error), "Record: %s",
                 error_text[0] ? error_text : esp_err_to_name(err));
        ai_task_set_state_if_current(generation, "ERROR");
        ai_task_set_reply_if_current(generation, display_error);
        goto done;
    }
    ESP_LOGI(TAG, "voice: record done bytes=%u", (unsigned)wav.len);

    if (g_app.net_lock) {
        if (xSemaphoreTake(g_app.net_lock, pdMS_TO_TICKS(VOICE_NET_LOCK_WAIT_MS)) == pdTRUE) {
            lock_taken = true;
        } else {
            ESP_LOGW(TAG, "voice: network busy timeout");
            ai_task_set_state_if_current(generation, "ERROR");
            ai_task_set_reply_if_current(generation, "网络忙，请再说一遍");
            goto done;
        }
    }

    ai_task_set_state_if_current(generation, "NET");
    ai_task_set_reply_if_current(generation,
                                 g_app.sta_connected ? "已联网，正在上传语音..." : "正在连接 WiFi...");
    ui_refresh_now();
    ESP_LOGI(TAG, "voice: wifi connect");
    err = wifi_sta_connect_once(WIFI_CONNECT_WAIT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice: wifi failed: %s", esp_err_to_name(err));
        char display_error[256] = {0};
        snprintf(display_error, sizeof(display_error), "WiFi: %s", esp_err_to_name(err));
        ai_task_set_state_if_current(generation, "ERROR");
        ai_task_set_reply_if_current(generation, display_error);
        goto done;
    }

    if (lock_taken) {
        xSemaphoreGive(g_app.net_lock);
        lock_taken = false;
    }

    if (api_url_uses_https()) {
        wait_for_clock_once(12000);
    }

#if VOICE_TRY_COMBINED_GATEWAY
    if (api_url_is_local_gateway()) {
        ai_task_set_state_if_current(generation, "VOICE");
        ai_task_set_reply_if_current(generation, "正在识别和回复...");
        ui_refresh_now();
        ESP_LOGI(TAG, "voice: gateway combined start");
        err = perform_gateway_voice_request(wav.data, wav.len, &gateway_result,
                                            error_text, sizeof(error_text));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "voice: gateway combined done text=%u reply=%u local=%d refresh=%d action=%s asset=%s",
                     (unsigned)strlen(gateway_result.transcript),
                     (unsigned)strlen(gateway_result.reply),
                     gateway_result.local ? 1 : 0,
                     gateway_result.refresh_market ? 1 : 0,
                     gateway_result.action,
                     gateway_result.asset);
            if (ai_task_generation_is_current(generation)) {
                copy_ai_text(g_app.ai_user, sizeof(g_app.ai_user),
                             gateway_result.transcript[0] ? gateway_result.transcript : "(empty)");
                copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), gateway_result.reply);
            }
            refresh_market_after_done = gateway_result.refresh_market;

            ai_task_set_state_if_current(generation, "PLAY");
            if (ai_task_generation_is_current(generation)) {
                g_app.ai_playback_inflight = true;
            }
            ui_refresh_now();
            ESP_LOGI(TAG, "voice: gateway audio fetch start");
            err = fetch_gateway_audio(gateway_result.audio_url, &gateway_audio,
                                      error_text, sizeof(error_text));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "voice: gateway audio fetch done bytes=%u",
                         (unsigned)gateway_audio.len);
                if (g_app.ai_tts_stop_requested) {
                    ESP_LOGI(TAG, "voice: gateway playback cancelled before play");
                    g_app.ai_playback_inflight = false;
                    ai_task_set_state_if_current(generation, "READY");
                    goto done;
                }
                if (lock_taken) {
                    wifi_sta_disconnect_when_idle();
                    xSemaphoreGive(g_app.net_lock);
                    lock_taken = false;
                }
                ESP_LOGI(TAG, "voice: gateway play start");
                err = play_wav_audio(gateway_audio.data, gateway_audio.len,
                                     error_text, sizeof(error_text));
                g_app.ai_playback_inflight = false;
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "voice: gateway play done");
                    ai_task_set_state_if_current(generation, "READY");
                    goto done;
                }
                if (g_app.ai_tts_stop_requested) {
                    ESP_LOGI(TAG, "voice: gateway play interrupted by new recording");
                    ai_task_set_state_if_current(generation, "READY");
                    goto done;
                }
                ESP_LOGE(TAG, "voice: gateway play failed: %s err=%s",
                         error_text[0] ? error_text : "play failed", esp_err_to_name(err));
            } else {
                g_app.ai_playback_inflight = false;
                if (g_app.ai_tts_stop_requested) {
                    ESP_LOGI(TAG, "voice: gateway audio fetch interrupted by new recording");
                    ai_task_set_state_if_current(generation, "READY");
                    goto done;
                }
                ESP_LOGW(TAG, "voice: gateway audio fetch failed: %s err=%s",
                         error_text[0] ? error_text : "audio failed", esp_err_to_name(err));
            }

            if (ai_task_generation_is_current(generation) && !schedule_tts_reply(gateway_result.reply)) {
                ai_task_set_state_if_current(generation, "PLAY ERR");
            }
            goto done;
        } else {
            ESP_LOGW(TAG, "voice: gateway combined failed, fallback: %s err=%s",
                     error_text[0] ? error_text : "combined failed", esp_err_to_name(err));
            char gateway_display_error[256] = {0};
            snprintf(gateway_display_error, sizeof(gateway_display_error), "网关错误：%s",
                     error_text[0] ? error_text : esp_err_to_name(err));
            ai_task_set_state_if_current(generation, "ERROR");
            ai_task_set_reply_if_current(generation, gateway_display_error);
            goto done;
        }
    } else {
        ESP_LOGI(TAG, "voice: public API mode, skip combined gateway");
    }
#endif

    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "STT");
    copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "正在识别...");
    ui_refresh_now();
    ESP_LOGI(TAG, "voice: stt start");
    err = perform_stt_request(wav.data, wav.len, transcript, sizeof(transcript),
                              error_text, sizeof(error_text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice: stt failed: %s err=%s",
                 error_text[0] ? error_text : "stt failed", esp_err_to_name(err));
        if (strstr(error_text, "没有识别到有效语音")) {
            copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
            copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "没有识别到有效语音，请再说一遍");
            goto done;
        }
        char display_error[256] = {0};
        snprintf(display_error, sizeof(display_error), "STT: %s",
                 error_text[0] ? error_text : esp_err_to_name(err));
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "ERROR");
        copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), display_error);
        goto done;
    }
    ESP_LOGI(TAG, "voice: stt done chars=%u", (unsigned)strlen(transcript));
    if (stt_text_is_empty_or_prompt_hallucination(transcript)) {
        ESP_LOGW(TAG, "voice: stt ignored empty/prompt hallucination");
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
        copy_ai_text(g_app.ai_user, sizeof(g_app.ai_user), "");
        copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "没有识别到有效语音，请再说一遍");
        goto done;
    }
    copy_ai_text(g_app.ai_user, sizeof(g_app.ai_user), transcript[0] ? transcript : "(empty)");

    bool local_refresh_market = false;
    if (build_local_voice_reply(transcript, reply, sizeof(reply), &local_refresh_market)) {
        ESP_LOGI(TAG, "voice: local command reply");
        copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), reply);
        if (local_refresh_market) {
            refresh_market_after_done = true;
            tts_after_done = true;
            copy_ai_text(deferred_tts_reply, sizeof(deferred_tts_reply), reply);
            copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
        } else if (!schedule_tts_reply(reply)) {
            copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
        }
        goto done;
    }

    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "AI");
    copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "正在思考...");
    ui_refresh_now();
    ESP_LOGI(TAG, "voice: chat start");
    err = perform_chat_request(transcript, reply, sizeof(reply), error_text, sizeof(error_text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice: chat failed: %s err=%s",
                 error_text[0] ? error_text : "chat failed", esp_err_to_name(err));
        char display_error[256] = {0};
        snprintf(display_error, sizeof(display_error), "AI: %s",
                 error_text[0] ? error_text : esp_err_to_name(err));
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "ERROR");
        copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), display_error);
        goto done;
    }
    ESP_LOGI(TAG, "voice: chat done chars=%u", (unsigned)strlen(reply));
    copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), reply);
    if (!schedule_tts_reply(reply)) {
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
    }

done:
    binary_buffer_free(&gateway_audio);
    binary_buffer_free(&wav);
    if (lock_taken) {
        wifi_sta_disconnect_when_idle();
        xSemaphoreGive(g_app.net_lock);
    }
    bool current_generation = ai_task_generation_is_current(generation);
    if (current_generation) {
        g_app.ai_playback_inflight = false;
        g_app.ai_start_inflight = false;
        g_app.ai_tts_stop_requested = false;
        g_app.ai_task_started_us = 0;
    }
    bool gateway_action_refreshes_market = strcmp(gateway_result.action, "refresh_market") == 0;
    if (current_generation) {
        execute_gateway_voice_action(&gateway_result);
    }
    if (current_generation && refresh_market_after_done && !gateway_action_refreshes_market) {
        home_market_refresh_async();
    }
    if (current_generation && tts_after_done && deferred_tts_reply[0]) {
        (void)schedule_tts_reply(deferred_tts_reply);
    }
    if (current_generation) {
        ui_refresh_now();
    }
    if (created_with_caps) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        vTaskDeleteWithCaps(NULL);
#else
        vTaskDelete(NULL);
#endif
    } else {
        vTaskDelete(NULL);
    }
}

static bool direct_ai_start_request(const char *source)
{
    if (g_app.ai_start_inflight) {
        int64_t now_us = esp_timer_get_time();
        bool stale = g_app.ai_task_started_us > 0 &&
                     (now_us - g_app.ai_task_started_us) > ((int64_t)VOICE_STALE_RESET_MS * 1000LL);
        if (stale && !g_app.ai_playback_inflight) {
            ESP_LOGW(TAG, "voice: stale recognition reset before new recording");
            g_app.ai_task_generation++;
            g_app.ai_start_inflight = false;
            g_app.ai_playback_inflight = false;
            g_app.ai_tts_stop_requested = true;
            g_app.ai_task_started_us = 0;
            copy_string(g_app.ai_state, sizeof(g_app.ai_state), "READY");
        } else if (!g_app.ai_playback_inflight) {
            ESP_LOGW(TAG, "voice: start ignored, recognition already running");
            return false;
        } else {
            ESP_LOGI(TAG, "voice: interrupt gateway playback before new recording");
            g_app.ai_tts_stop_requested = true;
            for (int i = 0; i < 60 &&
                 (g_app.ai_playback_inflight || g_app.ai_start_inflight); ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (g_app.ai_playback_inflight || g_app.ai_start_inflight) {
                ESP_LOGW(TAG, "voice: start ignored, playback did not stop in time");
                return false;
            }
        }
    }
    if (g_app.ai_tts_inflight) {
        ESP_LOGI(TAG, "voice: interrupt tts before new recording");
        g_app.ai_tts_stop_requested = true;
        for (int i = 0; i < 30 && g_app.ai_tts_inflight; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (g_app.ai_tts_inflight) {
            ESP_LOGW(TAG, "voice: start ignored, tts did not stop in time");
            return false;
        }
    }
    g_app.ai_tts_stop_requested = false;
    ESP_LOGI(TAG, "voice: start requested by %s", source ? source : "unknown");
    wake_screen_for_input();
    g_app.ai_start_inflight = true;
    uint32_t generation = ++g_app.ai_task_generation;
    if (generation == 0) {
        generation = ++g_app.ai_task_generation;
    }
    g_app.ai_task_started_us = esp_timer_get_time();
    copy_string(g_app.ai_state, sizeof(g_app.ai_state), "REC");
    copy_string(g_app.ai_user, sizeof(g_app.ai_user), "");
    copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "请直接说话...");
    if (source && strcmp(source, "ui") == 0) {
        ui_refresh_locked();
    } else if (bsp_display_lock(1000)) {
        ui_show_ai();
        bsp_display_unlock();
    } else {
        ui_refresh_now();
    }
    BaseType_t task_ok = pdFAIL;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    task_ok = xTaskCreateWithCaps(direct_ai_start_task, "voice_ai",
                                  VOICE_TASK_STACK_BYTES,
                                  (void *)(((uintptr_t)generation << 1) | 1u), 4, NULL,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "voice: psram task create failed, trying internal stack");
        task_ok = xTaskCreate(direct_ai_start_task, "voice_ai",
                              VOICE_TASK_FALLBACK_STACK_BYTES,
                              (void *)((uintptr_t)generation << 1), 4, NULL);
    }
    if (task_ok != pdPASS) {
        g_app.ai_start_inflight = false;
        g_app.ai_task_started_us = 0;
        copy_string(g_app.ai_state, sizeof(g_app.ai_state), "ERROR");
        copy_ai_text(g_app.ai_reply, sizeof(g_app.ai_reply), "创建语音任务失败");
        if (source && strcmp(source, "ui") == 0) {
            ui_refresh_locked();
        } else {
            ui_refresh_now();
        }
        log_voice_task_heap("task create failed");
        return false;
    }
    return true;
}

static void chart_refresh_task(void *arg)
{
    int row_index = (int)(uintptr_t)arg;
    if (row_index < 0 || row_index >= MARKET_ROW_COUNT) {
        g_app.chart_inflight = false;
        g_app.chart_task_started_us = 0;
        vTaskDelete(NULL);
        return;
    }

    if (g_app.net_lock) {
        xSemaphoreTake(g_app.net_lock, portMAX_DELAY);
    }

    copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "WiFi");
    ui_refresh_now();
    esp_err_t net_err = wifi_sta_connect_once(WIFI_CONNECT_WAIT_MS);
    if (net_err != ESP_OK) {
        copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "NO WIFI");
        g_app.chart_inflight = false;
        g_app.chart_task_started_us = 0;
        wifi_sta_disconnect_when_idle();
        if (g_app.net_lock) {
            xSemaphoreGive(g_app.net_lock);
        }
        ui_refresh_now();
        vTaskDelete(NULL);
        return;
    }

    wait_for_clock_once(12000);

    char *body = http_body_alloc(DIRECT_HTTP_MAX_BODY + 1);
    if (!body) {
        copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "NO MEM");
        g_app.chart_inflight = false;
        g_app.chart_task_started_us = 0;
        wifi_sta_disconnect_when_idle();
        if (g_app.net_lock) {
            xSemaphoreGive(g_app.net_lock);
        }
        ui_refresh_now();
        vTaskDelete(NULL);
        return;
    }

    char url[192] = {0};
    snprintf(url, sizeof(url), YAHOO_CHART_URL_FMT, MARKET_DEFS[row_index].chart_symbol);
    copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "Loading");
    ui_refresh_now();

    esp_err_t err = direct_http_request(url, NULL, DIRECT_HTTP_TIMEOUT_MS,
                                        false, body, DIRECT_HTTP_MAX_BODY + 1);
    bool parsed = err == ESP_OK && parse_yahoo_chart(body, row_index, true);
    if (parsed) {
        mark_market_time_now();
        g_app.market_polled_once = true;
        snprintf(g_app.market_status, sizeof(g_app.market_status), "OK %s", MARKET_DEFS[row_index].code);
        copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "OK");
        (void)market_cache_save();
    } else {
        copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "FAIL");
        snprintf(g_app.market_status, sizeof(g_app.market_status), "FAIL %s", MARKET_DEFS[row_index].code);
    }

    http_body_free(body);
    g_app.chart_inflight = false;
    g_app.chart_task_started_us = 0;
    wifi_sta_disconnect_when_idle();
    if (g_app.net_lock) {
        xSemaphoreGive(g_app.net_lock);
    }
    ui_refresh_now();
    vTaskDelete(NULL);
}

static void chart_refresh_async(int row_index)
{
    if (row_index < 0 || row_index >= MARKET_ROW_COUNT) {
        return;
    }
    if (g_app.chart_inflight || g_app.ai_start_inflight || g_app.ai_tts_inflight) {
        copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "WAIT");
        ui_refresh_locked();
        return;
    }
    g_app.chart_selected_index = row_index;
    g_app.chart_inflight = true;
    g_app.chart_task_started_us = esp_timer_get_time();
    copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "Starting");
    ui_refresh_locked();
    if (xTaskCreate(chart_refresh_task, "chart_refresh", 12288,
                    (void *)(uintptr_t)row_index, 4, NULL) != pdPASS) {
        g_app.chart_inflight = false;
        g_app.chart_task_started_us = 0;
        copy_string(g_app.chart_hint, sizeof(g_app.chart_hint), "NO TASK");
        ui_refresh_locked();
    }
}

static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < dst_size; i++) {
        const char *rep = NULL;
        if (src[i] == '&') {
            rep = "&amp;";
        } else if (src[i] == '<') {
            rep = "&lt;";
        } else if (src[i] == '>') {
            rep = "&gt;";
        } else if (src[i] == '"') {
            rep = "&quot;";
        }
        if (rep) {
            size_t n = strlen(rep);
            if (j + n >= dst_size) {
                break;
            }
            memcpy(&dst[j], rep, n);
            j += n;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            int hi = hex_value(r[1]);
            int lo = hex_value(r[2]);
            *w++ = (char)((hi << 4) | lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void form_get_value(char *body, const char *key, char *out, size_t out_size)
{
    if (!body || !key || !out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    size_t key_len = strlen(key);
    char *p = body;
    while (p && *p) {
        char *next = strchr(p, '&');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            if (strlen(p) == key_len && strcmp(p, key) == 0) {
                copy_string(out, out_size, eq + 1);
                url_decode(out);
                if (next) {
                    *next = '&';
                }
                *eq = '=';
                return;
            }
            *eq = '=';
        }
        if (!next) {
            break;
        }
        *next = '&';
        p = next + 1;
    }
}

static bool form_body_has_key(const char *body, const char *key)
{
    if (!body || !key || key[0] == '\0') {
        return false;
    }

    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static esp_err_t httpd_sendf_chunk(httpd_req_t *req, const char *fmt, ...)
{
    char stack_buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return ESP_FAIL;
    }
    if (n < (int)sizeof(stack_buf)) {
        return httpd_resp_send_chunk(req, stack_buf, n);
    }

    char *heap_buf = malloc((size_t)n + 1u);
    if (!heap_buf) {
        return ESP_ERR_NO_MEM;
    }
    va_start(ap, fmt);
    (void)vsnprintf(heap_buf, (size_t)n + 1u, fmt, ap);
    va_end(ap);
    esp_err_t err = httpd_resp_send_chunk(req, heap_buf, n);
    free(heap_buf);
    return err;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    char ssid1[96] = {0};
    char ssid2[96] = {0};
    char ssid3[96] = {0};
    char ssid4[96] = {0};
    char ssid5[96] = {0};
    char slot_ip[WIFI_SLOT_COUNT][32] = {{0}};
    char slot_netmask[WIFI_SLOT_COUNT][32] = {{0}};
    char slot_gateway[WIFI_SLOT_COUNT][32] = {{0}};
    char slot_dns1[WIFI_SLOT_COUNT][32] = {{0}};
    char slot_dns2[WIFI_SLOT_COUNT][32] = {{0}};
    char ai_url[AI_PROFILE_COUNT][224] = {{0}};
    char ai_model[AI_PROFILE_COUNT][96] = {{0}};
    char prompt[256] = {0};
    char market_provider[32] = {0};
    char proxy_host[96] = {0};
    char active_ssid[96] = {0};
    char preferred_ssid[96] = {0};
    html_escape(config_wifi_ssid_const(&g_app.cfg, 0), ssid1, sizeof(ssid1));
    html_escape(config_wifi_ssid_const(&g_app.cfg, 1), ssid2, sizeof(ssid2));
    html_escape(config_wifi_ssid_const(&g_app.cfg, 2), ssid3, sizeof(ssid3));
    html_escape(config_wifi_ssid_const(&g_app.cfg, 3), ssid4, sizeof(ssid4));
    html_escape(config_wifi_ssid_const(&g_app.cfg, 4), ssid5, sizeof(ssid5));
    for (int i = 0; i < AI_PROFILE_COUNT; i++) {
        html_escape(g_app.ai_profiles.profiles[i].server_url,
                    ai_url[i], sizeof(ai_url[i]));
        html_escape(g_app.ai_profiles.profiles[i].model,
                    ai_model[i], sizeof(ai_model[i]));
    }
    html_escape(g_app.cfg.starter_prompt, prompt, sizeof(prompt));
    html_escape(g_app.cfg.market_provider, market_provider, sizeof(market_provider));
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        const wifi_ip_config_t *net = config_wifi_net_const(&g_app.cfg, i);
        if (!net) {
            continue;
        }
        html_escape(net->static_ip, slot_ip[i], sizeof(slot_ip[i]));
        html_escape(net->static_netmask, slot_netmask[i], sizeof(slot_netmask[i]));
        html_escape(net->static_gateway, slot_gateway[i], sizeof(slot_gateway[i]));
        html_escape(net->dns1, slot_dns1[i], sizeof(slot_dns1[i]));
        html_escape(net->dns2, slot_dns2[i], sizeof(slot_dns2[i]));
    }
    html_escape(g_app.cfg.proxy_host, proxy_host, sizeof(proxy_host));
    int active_slot = wifi_current_connected_slot();
    if (active_slot >= 0 && active_slot < WIFI_SLOT_COUNT) {
        html_escape(config_wifi_ssid_const(&g_app.cfg, active_slot),
                    active_ssid, sizeof(active_ssid));
    }
    if (g_app.preferred_wifi_slot >= 0 && g_app.preferred_wifi_slot < WIFI_SLOT_COUNT) {
        html_escape(config_wifi_ssid_const(&g_app.cfg, g_app.preferred_wifi_slot),
                    preferred_ssid, sizeof(preferred_ssid));
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WatchLite WiFi</title><style>"
        "body{margin:0;background:linear-gradient(160deg,#06111f,#102039 56%,#06111f);"
        "color:#eaf1ff;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
        ".wrap{max-width:680px;margin:0 auto;padding:18px}"
        ".card{background:rgba(16,29,46,.92);border:1px solid #263a58;border-radius:22px;padding:18px;margin:14px 0;"
        "box-shadow:0 16px 38px rgba(0,0,0,.22)}"
        "h2{margin:6px 0 14px}h3{margin:6px 0 12px}"
        "label{display:block;margin:13px 0 6px;color:#91a3bf}"
        "input,textarea,select{width:100%;box-sizing:border-box;border:1px solid #314a70;border-radius:14px;"
        "padding:13px;background:#07111f;color:#fff;font-size:16px}"
        "textarea{min-height:74px}button{width:100%;border:0;border-radius:16px;padding:14px;margin-top:14px;"
        "background:#335cff;color:white;font-weight:700;font-size:16px}"
        "button.secondary{background:#223653}.slot{border:1px solid #263a58;border-radius:18px;padding:13px;margin-top:13px;background:#0b1728}"
        ".slotHead{display:flex;gap:10px;align-items:center;justify-content:space-between}.slotHead b{font-size:17px}"
        ".slotHead button{width:auto;margin:0;padding:9px 12px;border-radius:12px;background:#1f8f72;font-size:14px}"
        ".scan{display:grid;gap:9px;margin-top:12px}.net{margin:0;text-align:left;background:#172942;border:1px solid #36537c;"
        "font-weight:650}.muted{color:#91a3bf;font-size:14px;line-height:1.5}.ok{color:#7be0c3}.warn{color:#f7c873}"
        ".pill{display:inline-block;border:1px solid #36537c;border-radius:999px;padding:3px 8px;margin-left:5px;color:#bcd0ee;font-size:12px}"
        "</style></head><body><div class='wrap'><h2>WatchLite WiFi</h2>");
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_sendf_chunk(req,
        "<div class='card muted'>AP <b class='ok'>%s</b> / PASS <b>%s</b><br>"
        "AP 后台: <b>http://192.168.4.1/</b><br>"
        "STA <b>%s</b> %s<br>当前: <b>%s</b> / 默认: <b>%s</b><br>API key: <b>%s</b></div>",
        g_app.ap_ssid, AP_PASSWORD,
        g_app.sta_connected ? "connected" : "not connected",
        g_app.sta_connected ? g_app.sta_ip : "",
        active_ssid[0] ? active_ssid : "-",
        preferred_ssid[0] ? preferred_ssid : "auto",
        g_app.cfg.api_key[0] ? "set" : "not set");
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req,
        "<form class='card' method='post' action='/save'>"
        "<h3>WiFi</h3>"
        "<p id='wifiNow' class='muted'>扫描附近 WiFi，点一个网络填入下面指定槽位；填密码后保存重启。"
        "已保存的槽位可以直接点 Switch 立即切换。</p>"
        "<label>扫描填入到</label><select id='targetSlot'>");
    if (err != ESP_OK) {
        return err;
    }
    int target_slot = (g_app.preferred_wifi_slot >= 0 && g_app.preferred_wifi_slot < WIFI_SLOT_COUNT) ?
                      g_app.preferred_wifi_slot + 1 : 1;
    for (int i = 1; i <= WIFI_SLOT_COUNT; i++) {
        err = httpd_sendf_chunk(req, "<option value='%d' %s>WiFi %d</option>",
                                i, i == target_slot ? "selected" : "", i);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = httpd_resp_sendstr_chunk(req,
        "</select><button type='button' onclick='scanWifi()'>扫描附近 WiFi</button>"
        "<div id='scanResults' class='scan muted'>还没有扫描。</div>"
        "<label>默认使用</label><select name='preferred_slot'>");
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_sendf_chunk(req, "<option value='0' %s>自动: 按保存顺序</option>",
                            g_app.preferred_wifi_slot < 0 ? "selected" : "");
    if (err != ESP_OK) {
        return err;
    }
    const char *ssid_values[WIFI_SLOT_COUNT] = {ssid1, ssid2, ssid3, ssid4, ssid5};
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        err = httpd_sendf_chunk(req,
            "<option value='%d' %s>WiFi %d%s%s</option>",
            i + 1,
            g_app.preferred_wifi_slot == i ? "selected" : "",
            i + 1,
            ssid_values[i][0] ? ": " : "",
            ssid_values[i][0] ? ssid_values[i] : "");
        if (err != ESP_OK) {
            return err;
        }
    }
    err = httpd_resp_sendstr_chunk(req, "</select>");
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        err = httpd_sendf_chunk(req,
            "<div class='slot'><div class='slotHead'><b>WiFi %d%s</b>"
            "<button type='button' onclick='switchWifi(%d)'>Switch</button></div>"
            "<label>SSID</label><input name='ssid%d' maxlength='32' value=\"%s\">"
            "<label>Password</label><input name='password%d' maxlength='64' type='password' "
            "placeholder='空着=保留旧密码；换新SSID后空着=无密码'>"
            "<label>Static IP (blank = DHCP)</label><input name='static_ip%d' maxlength='15' value=\"%s\" placeholder='192.168.1.88'>"
            "<label>Netmask</label><input name='static_netmask%d' maxlength='15' value=\"%s\" placeholder='255.255.255.0'>"
            "<label>Gateway</label><input name='static_gateway%d' maxlength='15' value=\"%s\" placeholder='192.168.1.1'>"
            "<label>DNS 1</label><input name='dns1_%d' maxlength='15' value=\"%s\" placeholder='192.168.1.1'>"
            "<label>DNS 2</label><input name='dns2_%d' maxlength='15' value=\"%s\" placeholder='1.1.1.1'></div>",
            i + 1,
            active_slot == i ? " <span class='pill ok'>connected</span>" :
            (g_app.preferred_wifi_slot == i ? " <span class='pill'>default</span>" : ""),
            i + 1,
            i + 1, ssid_values[i],
            i + 1,
            i + 1, slot_ip[i],
            i + 1, slot_netmask[i],
            i + 1, slot_gateway[i],
            i + 1, slot_dns1[i],
            i + 1, slot_dns2[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = httpd_resp_sendstr_chunk(req,
        "<h3>AI</h3>"
        "<label>首页默认 AI</label><select name='ai_profile'>");
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < AI_PROFILE_COUNT; i++) {
        err = httpd_sendf_chunk(req, "<option value='%d' %s>%s%s</option>",
                                i + 1,
                                g_app.ai_profiles.active_index == i ? "selected" : "",
                                g_app.ai_profiles.profiles[i].name,
                                g_app.ai_profiles.profiles[i].api_key[0] ? " / key set" : "");
        if (err != ESP_OK) {
            return err;
        }
    }
    err = httpd_resp_sendstr_chunk(req, "</select>");
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < AI_PROFILE_COUNT; i++) {
        err = httpd_sendf_chunk(req,
            "<div class='slot'><div class='slotHead'><b>%s</b><span class='pill'>%s</span></div>"
            "<label>API URL</label><input name='ai_url%d' maxlength='159' value=\"%s\" "
            "placeholder='https://example.com/v1'>"
            "<label>API Key</label><input name='ai_key%d' maxlength='191' type='password' "
            "placeholder='blank = keep old key'>"
            "<label>Model</label><input name='ai_model%d' maxlength='63' value=\"%s\"></div>",
            g_app.ai_profiles.profiles[i].name,
            g_app.ai_profiles.profiles[i].api_key[0] ? "key set" : "no key",
            i + 1, ai_url[i],
            i + 1,
            i + 1, ai_model[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = httpd_sendf_chunk(req,
        "<label>AI Prompt</label><textarea name='starter_prompt' maxlength='159'>%s</textarea>"
        "<label>Market</label><select name='market_provider'>"
        "<option value='coingecko' %s>coingecko</option><option value='mock' %s>mock</option></select>"
        "<h3>Common</h3>"
        "<p class='muted'>每个 WiFi 的 Static IP/Gateway/DNS 在对应 WiFi 卡片里单独保存；留空就是 DHCP。</p>"
        "<label>Proxy Host (saved, experimental)</label><input name='proxy_host' maxlength='63' value=\"%s\" placeholder='phone hotspot IP'>"
        "<label>Proxy Port (saved, experimental)</label><input name='proxy_port' type='number' min='0' max='65535' value='%d'>"
        "<label>Auto Sleep Seconds</label><input name='sleep' type='number' min='0' max='86400' value='%d'>"
        "<label>Brightness Percent</label><input name='brightness' type='number' min='5' max='100' value='%d'>"
        "<label>Shake Wake</label><select name='motion_wake'>"
        "<option value='1' %s>enabled</option><option value='0' %s>disabled</option></select>"
        "<label>Shake Threshold mg</label><input name='motion_threshold' type='number' min='%d' max='%d' value='%d'>"
        "<button type='submit'>Save and Reboot</button></form>"
        "<div class='card muted warn'>静态 IP/Gateway/DNS 仍按原逻辑保存；WiFi 扫描和 Switch 不需要外网。</div>",
        prompt,
        strcasecmp(market_provider, "coingecko") == 0 ? "selected" : "",
        strcasecmp(market_provider, "mock") == 0 ? "selected" : "",
        proxy_host, g_app.cfg.proxy_port, g_app.cfg.auto_sleep_s,
        g_app.cfg.brightness_percent,
        g_app.cfg.motion_wake_enabled ? "selected" : "",
        g_app.cfg.motion_wake_enabled ? "" : "selected",
        MIN_MOTION_WAKE_THRESHOLD_MG, MAX_MOTION_WAKE_THRESHOLD_MG,
        g_app.cfg.motion_wake_threshold_mg);
    if (err != ESP_OK) {
        return err;
    }

    err = httpd_resp_sendstr_chunk(req,
        "<script>"
        "const $=s=>document.querySelector(s);"
        "function fillWifi(ssid,secure){const n=$('#targetSlot').value||'1';"
        "$('[name=ssid'+n+']').value=ssid;"
        "$('[name=preferred_slot]').value=n;"
        "const p=$('[name=password'+n+']');"
        "if(secure){p.placeholder='输入这个 WiFi 的密码';p.focus();}else{p.value='';p.placeholder='开放网络，无需密码';}"
        "}"
        "async function scanWifi(){const box=$('#scanResults');box.textContent='扫描中...';"
        "try{const r=await fetch('/api/wifi/scan',{cache:'no-store'});const j=await r.json();"
        "if(!j.ok){box.textContent='扫描失败: '+(j.error||'unknown');return;}"
        "box.innerHTML='';if(!j.networks||!j.networks.length){box.textContent='没有扫到 WiFi';return;}"
        "j.networks.forEach(n=>{const b=document.createElement('button');b.type='button';b.className='net';"
        "b.textContent=n.ssid+'  '+n.rssi+'dBm  '+(n.security||'');b.onclick=()=>fillWifi(n.ssid,n.secure);box.appendChild(b);});"
        "}catch(e){box.textContent='扫描失败: '+e;}}"
        "async function refreshStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});const s=await r.json();"
        "$('#wifiNow').innerHTML='当前: <b>'+(s.active_wifi_ssid||'-')+'</b> '+(s.sta_connected?s.sta_ip:'not connected')+"
        "' / 默认: <b>'+(s.preferred_wifi_ssid||'auto')+'</b>';}"
        "catch(e){}}"
        "async function switchWifi(slot){const box=$('#scanResults');box.textContent='正在切换到 WiFi '+slot+'...';"
        "try{const r=await fetch('/api/wifi/select?slot='+slot,{cache:'no-store'});const j=await r.json();"
        "box.textContent=j.ok?('已开始切换到 WiFi '+slot+'，稍等几秒看状态'):('切换失败: '+(j.error||'unknown'));"
        "if(j.ok){$('[name=preferred_slot]').value=String(slot);setTimeout(refreshStatus,1500);setTimeout(refreshStatus,4500);}}"
        "catch(e){box.textContent='切换失败: '+e;}}"
        "refreshStatus();"
        "</script></div></body></html>");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

static void form_update_string(char *body, const char *key, char *dst, size_t dst_size, bool keep_blank)
{
    char *work = malloc(strlen(body) + 1);
    char value[256] = {0};
    if (!work) {
        return;
    }
    strcpy(work, body);
    form_get_value(work, key, value, sizeof(value));
    free(work);
    if (value[0] || !keep_blank) {
        copy_string(dst, dst_size, value);
    }
}

static void form_update_string_if_present(char *body, const char *key, char *dst, size_t dst_size,
                                          bool keep_blank)
{
    if (!form_body_has_key(body, key)) {
        return;
    }
    form_update_string(body, key, dst, dst_size, keep_blank);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len > 12287) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }

    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    watch_lite_config_t next = g_app.cfg;
    ai_profile_store_t next_profiles = g_app.ai_profiles;
    next.magic = CONFIG_MAGIC;
    next.version = CONFIG_VERSION;
    next_profiles.magic = AI_PROFILE_MAGIC;
    next_profiles.version = AI_PROFILE_VERSION;

    char *work = malloc(strlen(body) + 1);
    char value[256] = {0};
    if (!work) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        char ssid_key[16] = {0};
        char password_key[16] = {0};
        char static_ip_key[24] = {0};
        char static_netmask_key[24] = {0};
        char static_gateway_key[24] = {0};
        char dns1_key[16] = {0};
        char dns2_key[16] = {0};
        char new_ssid[33] = {0};
        char new_password[65] = {0};
        snprintf(ssid_key, sizeof(ssid_key), "ssid%d", i + 1);
        snprintf(password_key, sizeof(password_key), "password%d", i + 1);
        snprintf(static_ip_key, sizeof(static_ip_key), "static_ip%d", i + 1);
        snprintf(static_netmask_key, sizeof(static_netmask_key), "static_netmask%d", i + 1);
        snprintf(static_gateway_key, sizeof(static_gateway_key), "static_gateway%d", i + 1);
        snprintf(dns1_key, sizeof(dns1_key), "dns1_%d", i + 1);
        snprintf(dns2_key, sizeof(dns2_key), "dns2_%d", i + 1);

        bool ssid_present = form_body_has_key(body, ssid_key);
        bool password_present = form_body_has_key(body, password_key);
        char *ssid_dst = config_wifi_ssid(&next, i);
        char *password_dst = config_wifi_password(&next, i);
        wifi_ip_config_t *net_dst = config_wifi_net(&next, i);
        bool ssid_changed = false;

        if (ssid_present && ssid_dst) {
            strcpy(work, body);
            form_get_value(work, ssid_key, new_ssid, sizeof(new_ssid));
            ssid_changed = strcmp(ssid_dst, new_ssid) != 0;
            copy_string(ssid_dst, 33, new_ssid);
        }

        if (password_present && password_dst) {
            strcpy(work, body);
            form_get_value(work, password_key, new_password, sizeof(new_password));
            if (new_password[0]) {
                copy_string(password_dst, 65, new_password);
            } else if (ssid_changed) {
                password_dst[0] = '\0';
            }
        } else if (ssid_changed && password_dst) {
            password_dst[0] = '\0';
        }

        if (net_dst) {
            form_update_string_if_present(body, static_ip_key, net_dst->static_ip,
                                          sizeof(net_dst->static_ip), false);
            form_update_string_if_present(body, static_netmask_key, net_dst->static_netmask,
                                          sizeof(net_dst->static_netmask), false);
            form_update_string_if_present(body, static_gateway_key, net_dst->static_gateway,
                                          sizeof(net_dst->static_gateway), false);
            form_update_string_if_present(body, dns1_key, net_dst->dns1,
                                          sizeof(net_dst->dns1), false);
            form_update_string_if_present(body, dns2_key, net_dst->dns2,
                                          sizeof(net_dst->dns2), false);
        }
    }
    form_update_string_if_present(body, "ssid", next.wifi_ssid, sizeof(next.wifi_ssid), false);
    form_update_string_if_present(body, "password", next.wifi_password, sizeof(next.wifi_password), true);
    for (int i = 0; i < AI_PROFILE_COUNT; i++) {
        char url_key[16] = {0};
        char api_key_key[16] = {0};
        char model_key[18] = {0};
        snprintf(url_key, sizeof(url_key), "ai_url%d", i + 1);
        snprintf(api_key_key, sizeof(api_key_key), "ai_key%d", i + 1);
        snprintf(model_key, sizeof(model_key), "ai_model%d", i + 1);
        form_update_string_if_present(body, url_key, next_profiles.profiles[i].server_url,
                                      sizeof(next_profiles.profiles[i].server_url), true);
        form_update_string_if_present(body, api_key_key, next_profiles.profiles[i].api_key,
                                      sizeof(next_profiles.profiles[i].api_key), true);
        form_update_string_if_present(body, model_key, next_profiles.profiles[i].model,
                                      sizeof(next_profiles.profiles[i].model), true);
    }
    strcpy(work, body);
    form_get_value(work, "ai_profile", value, sizeof(value));
    if (value[0]) {
        int requested_profile = atoi(value) - 1;
        next_profiles.active_index = (requested_profile >= 0 &&
                                      requested_profile < AI_PROFILE_COUNT) ?
                                     requested_profile : AI_PROFILE_CUSTOM_INDEX;
    }
    ai_profiles_sanitize(&next_profiles);
    const ai_profile_t *active_profile = &next_profiles.profiles[next_profiles.active_index];
    copy_string(next.server_url, sizeof(next.server_url), active_profile->server_url);
    copy_string(next.api_key, sizeof(next.api_key), active_profile->api_key);
    copy_string(next.model, sizeof(next.model), active_profile->model);
    form_update_string_if_present(body, "api_url", next.server_url, sizeof(next.server_url), true);
    form_update_string_if_present(body, "api_key", next.api_key, sizeof(next.api_key), true);
    form_update_string_if_present(body, "model", next.model, sizeof(next.model), true);
    form_update_string(body, "starter_prompt", next.starter_prompt, sizeof(next.starter_prompt), true);
    form_update_string(body, "market_provider", next.market_provider, sizeof(next.market_provider), true);
    form_update_string(body, "static_ip", next.static_ip, sizeof(next.static_ip), false);
    form_update_string(body, "static_netmask", next.static_netmask, sizeof(next.static_netmask), false);
    form_update_string(body, "static_gateway", next.static_gateway, sizeof(next.static_gateway), false);
    form_update_string(body, "dns1", next.dns1, sizeof(next.dns1), false);
    form_update_string(body, "dns2", next.dns2, sizeof(next.dns2), false);
    form_update_string(body, "proxy_host", next.proxy_host, sizeof(next.proxy_host), false);
    if (form_body_has_key(body, "static_ip") ||
        form_body_has_key(body, "static_netmask") ||
        form_body_has_key(body, "static_gateway") ||
        form_body_has_key(body, "dns1") ||
        form_body_has_key(body, "dns2")) {
        copy_string(next.wifi_net[0].static_ip, sizeof(next.wifi_net[0].static_ip), next.static_ip);
        copy_string(next.wifi_net[0].static_netmask, sizeof(next.wifi_net[0].static_netmask), next.static_netmask);
        copy_string(next.wifi_net[0].static_gateway, sizeof(next.wifi_net[0].static_gateway), next.static_gateway);
        copy_string(next.wifi_net[0].dns1, sizeof(next.wifi_net[0].dns1), next.dns1);
        copy_string(next.wifi_net[0].dns2, sizeof(next.wifi_net[0].dns2), next.dns2);
    }

    strcpy(work, body);
    form_get_value(work, "proxy_port", value, sizeof(value));
    next.proxy_port = value[0] ? atoi(value) : 0;

    strcpy(work, body);
    form_get_value(work, "sleep", value, sizeof(value));
    if (value[0]) {
        next.auto_sleep_s = atoi(value);
    }

    strcpy(work, body);
    form_get_value(work, "brightness", value, sizeof(value));
    if (value[0]) {
        next.brightness_percent = atoi(value);
    }

    strcpy(work, body);
    form_get_value(work, "motion_wake", value, sizeof(value));
    if (value[0]) {
        next.motion_wake_enabled = atoi(value) ? 1 : 0;
    }

    strcpy(work, body);
    form_get_value(work, "motion_threshold", value, sizeof(value));
    if (value[0]) {
        next.motion_wake_threshold_mg = atoi(value);
    }
    config_sanitize(&next);

    int next_preferred_slot = g_app.preferred_wifi_slot;
    strcpy(work, body);
    form_get_value(work, "preferred_slot", value, sizeof(value));
    if (value[0]) {
        int requested_slot = atoi(value) - 1;
        next_preferred_slot = (requested_slot >= 0 &&
                               requested_slot < WIFI_SLOT_COUNT &&
                               config_wifi_slot_used(&next, requested_slot)) ?
                              requested_slot : -1;
    } else if (next_preferred_slot >= 0 &&
               !config_wifi_slot_used(&next, next_preferred_slot)) {
        next_preferred_slot = -1;
    }
    free(work);

    esp_err_t err = config_save(&next);
    if (err != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    err = ai_profiles_save(&next_profiles);
    if (err != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save ai profiles failed");
        return ESP_FAIL;
    }
    err = wifi_preferred_slot_save(next_preferred_slot);
    if (err != ESP_OK) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save wifi slot failed");
        return ESP_FAIL;
    }

    g_app.cfg = next;
    g_app.ai_profiles = next_profiles;
    g_app.sta_configured = config_has_wifi(&g_app.cfg);
    g_app.preferred_wifi_slot = next_preferred_slot;
    free(body);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body style='font-family:sans-serif;background:#07111f;color:#eaf1ff'>"
                            "<h3>Saved. Watch is rebooting...</h3></body></html>");
    xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char active_ssid[48] = {0};
    char preferred_ssid[48] = {0};
    int active_slot = wifi_current_connected_slot();
    int preferred_slot = g_app.preferred_wifi_slot;
    const wifi_ip_config_t *active_net = config_wifi_net_const(&g_app.cfg, active_slot);
    int ai_elapsed_ms = (g_app.ai_start_inflight && g_app.ai_task_started_us > 0) ?
                        (int)((esp_timer_get_time() - g_app.ai_task_started_us) / 1000LL) : 0;
    if (active_slot >= 0 && active_slot < WIFI_SLOT_COUNT) {
        html_escape(config_wifi_ssid_const(&g_app.cfg, active_slot), active_ssid, sizeof(active_ssid));
    }
    if (preferred_slot >= 0 && preferred_slot < WIFI_SLOT_COUNT) {
        html_escape(config_wifi_ssid_const(&g_app.cfg, preferred_slot), preferred_ssid, sizeof(preferred_ssid));
    }

    char json[1408];
    snprintf(json, sizeof(json),
             "{\"ap_ssid\":\"%s\",\"ap_ip\":\"192.168.4.1\",\"sta_connected\":%s,\"sta_ip\":\"%s\","
             "\"active_wifi_slot\":%d,\"active_wifi_ssid\":\"%s\","
             "\"preferred_wifi_slot\":%d,\"preferred_wifi_ssid\":\"%s\","
             "\"active_static_ip\":\"%s\",\"active_gateway\":\"%s\",\"active_dns1\":\"%s\","
             "\"ai_profile\":\"%s\",\"api_url\":\"%s\",\"api_key_set\":%s,\"model\":\"%s\","
             "\"auto_sleep_s\":%d,\"brightness_percent\":%d,"
             "\"motion_wake_enabled\":%d,\"motion_wake_threshold_mg\":%d,"
             "\"market_polled_once\":%s,\"ai_state\":\"%s\",\"ai_elapsed_ms\":%d,"
             "\"ai_inflight\":%s,\"tts_inflight\":%s}",
             g_app.ap_ssid,
             g_app.sta_connected ? "true" : "false",
             g_app.sta_connected ? g_app.sta_ip : "",
             active_slot >= 0 ? active_slot + 1 : 0,
             active_ssid,
             preferred_slot >= 0 ? preferred_slot + 1 : 0,
             preferred_ssid,
             active_net ? active_net->static_ip : "",
             active_net ? active_net->static_gateway : "",
             active_net ? active_net->dns1 : "",
             ai_profile_active()->name,
             g_app.cfg.server_url,
             g_app.cfg.api_key[0] ? "true" : "false",
             g_app.cfg.model,
             g_app.cfg.auto_sleep_s,
             g_app.cfg.brightness_percent,
             g_app.cfg.motion_wake_enabled,
             g_app.cfg.motion_wake_threshold_mg,
             g_app.market_polled_once ? "true" : "false",
             g_app.ai_state,
             ai_elapsed_ms,
             g_app.ai_start_inflight ? "true" : "false",
             (g_app.ai_tts_inflight || g_app.ai_playback_inflight) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static const char *wifi_auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "SECURE";
    }
}

static void wifi_scan_sort_and_dedupe(wifi_ap_record_t *records, uint16_t *count)
{
    if (!records || !count) {
        return;
    }

    for (uint16_t i = 1; i < *count; i++) {
        wifi_ap_record_t key = records[i];
        int j = (int)i - 1;
        while (j >= 0 && records[j].rssi < key.rssi) {
            records[j + 1] = records[j];
            j--;
        }
        records[j + 1] = key;
    }

    uint16_t out = 0;
    for (uint16_t i = 0; i < *count; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        bool seen = false;
        for (uint16_t j = 0; j < out; j++) {
            if (strncmp((const char *)records[j].ssid, (const char *)records[i].ssid,
                        sizeof(records[i].ssid)) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            records[out++] = records[i];
        }
    }
    *count = out;
}

static esp_err_t wifi_ensure_scan_mode(void)
{
    if (!g_app.wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return wifi_enable_config_ap();
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = wifi_ensure_scan_mode();
    if (err != ESP_OK) {
        char error_json[96];
        snprintf(error_json, sizeof(error_json),
                 "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, error_json);
    }

    bool lock_taken = false;
    if (g_app.net_lock) {
        if (xSemaphoreTake(g_app.net_lock, pdMS_TO_TICKS(12000)) != pdTRUE) {
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"network busy\"}");
        }
        lock_taken = true;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    err = esp_wifi_scan_start(&scan_config, true);

    uint16_t ap_count = 0;
    wifi_ap_record_t records[WIFI_SCAN_MAX_RESULTS] = {0};
    if (err == ESP_OK) {
        (void)esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > WIFI_SCAN_MAX_RESULTS) {
            ap_count = WIFI_SCAN_MAX_RESULTS;
        }
        err = esp_wifi_scan_get_ap_records(&ap_count, records);
    }
    if (lock_taken) {
        xSemaphoreGive(g_app.net_lock);
    }

    if (err != ESP_OK) {
        char error_json[96];
        snprintf(error_json, sizeof(error_json),
                 "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, error_json);
    }

    wifi_scan_sort_and_dedupe(records, &ap_count);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = root ? cJSON_AddArrayToObject(root, "networks") : NULL;
    if (!root || !networks) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "count", ap_count);
    for (uint16_t i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "channel", records[i].primary);
        cJSON_AddNumberToObject(item, "auth", records[i].authmode);
        cJSON_AddBoolToObject(item, "secure", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddStringToObject(item, "security", wifi_auth_mode_name(records[i].authmode));
        cJSON_AddItemToArray(networks, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t send_err = httpd_resp_sendstr(req, json);
    free(json);
    return send_err;
}

static void wifi_select_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    bool lock_taken = false;
    if (g_app.net_lock) {
        xSemaphoreTake(g_app.net_lock, portMAX_DELAY);
        lock_taken = true;
    }

    g_app.sta_configured = config_has_wifi(&g_app.cfg);
    if (g_app.wifi_started && g_app.sta_configured &&
        slot >= 0 && slot < WIFI_SLOT_COUNT &&
        config_wifi_slot_used(&g_app.cfg, slot)) {
        (void)wifi_enable_config_ap();
        if (wifi_current_connected_slot() != slot) {
            (void)wifi_apply_sta_slot(slot);
            wifi_sta_disconnect_now();
            apply_ip_config_for_slot(slot);
            g_app.sta_connect_requested = true;
            g_app.pending_wifi_slot = slot;
            (void)esp_wifi_connect();
        } else {
            g_app.active_wifi_slot = slot;
        }
    }

    if (lock_taken) {
        xSemaphoreGive(g_app.net_lock);
    }
    ui_refresh_now();
    vTaskDelete(NULL);
}

static esp_err_t wifi_select_get_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char slot_text[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", slot_text, sizeof(slot_text)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing slot\"}");
    }

    int slot = atoi(slot_text) - 1;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (slot < 0 || slot >= WIFI_SLOT_COUNT || !config_wifi_slot_used(&g_app.cfg, slot)) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid slot\"}");
    }

    esp_err_t err = wifi_preferred_slot_save(slot);
    if (err == ESP_OK) {
        g_app.preferred_wifi_slot = slot;
    }
    if (err != ESP_OK) {
        char error_json[96];
        snprintf(error_json, sizeof(error_json),
                 "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, error_json);
    }

    bool already_connected = wifi_current_connected_slot() == slot;
    bool task_started = already_connected;
    if (!already_connected) {
        task_started = xTaskCreate(wifi_select_task, "wifi_select", 4096,
                                   (void *)(intptr_t)slot, 4, NULL) == pdPASS;
    }

    char ssid[80] = {0};
    html_escape(config_wifi_ssid_const(&g_app.cfg, slot), ssid, sizeof(ssid));
    char json[192];
    snprintf(json, sizeof(json),
             "{\"ok\":%s,\"slot\":%d,\"ssid\":\"%s\",\"already_connected\":%s,\"error\":\"%s\"}",
             task_started ? "true" : "false",
             slot + 1,
             ssid,
             already_connected ? "true" : "false",
             task_started ? "" : "no task");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t ui_debug_get_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char screen[32] = {0};
    char row[16] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "screen", screen, sizeof(screen));
        (void)httpd_query_key_value(query, "row", row, sizeof(row));
    }

    bool switched = false;
    if (screen[0]) {
        if (!bsp_display_lock(1000)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display lock failed");
            return ESP_FAIL;
        }

        if (strcmp(screen, "home") == 0) {
            ui_show_home();
            switched = true;
        } else if (strcmp(screen, "ai") == 0) {
            ui_show_ai();
            switched = true;
        } else if (strcmp(screen, "chart") == 0) {
            int row_index = row[0] ? atoi(row) : 0;
            if (row_index < 0) {
                row_index = 0;
            } else if (row_index >= MARKET_ROW_COUNT) {
                row_index = MARKET_ROW_COUNT - 1;
            }
            ui_show_chart(row_index);
            switched = true;
        }
        lv_refr_now(NULL);
        bsp_display_unlock();
    }

    char *ai_dialog = heap_caps_calloc(1, sizeof(g_app.ai_dialog), MALLOC_CAP_8BIT);
    char *ai_reply = heap_caps_calloc(1, sizeof(g_app.ai_reply), MALLOC_CAP_8BIT);
    char *ai_user = heap_caps_calloc(1, sizeof(g_app.ai_user), MALLOC_CAP_8BIT);
    char *json = heap_caps_calloc(1, 2048, MALLOC_CAP_8BIT);
    if (!ai_dialog || !ai_reply || !ai_user || !json) {
        free(ai_dialog);
        free(ai_reply);
        free(ai_user);
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }

    display_safe_text_copy(g_app.ai_dialog, ai_dialog, sizeof(g_app.ai_dialog));
    display_safe_text_copy(g_app.ai_reply, ai_reply, sizeof(g_app.ai_reply));
    display_safe_text_copy(g_app.ai_user, ai_user, sizeof(g_app.ai_user));

    snprintf(json, 2048,
             "{\"ok\":true,\"switched\":%s,\"screen\":\"%s\",\"ai_state\":\"%s\","
             "\"ai_inflight\":%s,\"tts_inflight\":%s,"
             "\"ai_user\":\"%s\",\"ai_reply\":\"%s\",\"ai_dialog\":\"%s\"}",
             switched ? "true" : "false",
             ui_current_screen_name(),
             g_app.ai_state,
             g_app.ai_start_inflight ? "true" : "false",
             (g_app.ai_tts_inflight || g_app.ai_playback_inflight) ? "true" : "false",
             ai_user,
             ai_reply,
             ai_dialog);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(ai_dialog);
    free(ai_reply);
    free(ai_user);
    free(json);
    return err;
}

static esp_err_t screen_bmp_get_handler(httpd_req_t *req)
{
    esp_err_t result = ESP_FAIL;
    lv_draw_buf_t *snap = NULL;
    uint8_t *bmp = NULL;

    if (!bsp_display_lock(2000)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "display lock failed");
        return ESP_FAIL;
    }

    lv_refr_now(NULL);
    snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();

    if (!snap || !snap->data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
        goto done;
    }

    uint32_t width = snap->header.w;
    uint32_t height = snap->header.h;
    uint32_t row_size = ((width * 3u) + 3u) & ~3u;
    uint32_t pixel_bytes = row_size * height;
    uint32_t file_size = 54u + pixel_bytes;

    bmp = heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
    if (!bmp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        goto done;
    }

    memset(bmp, 0, file_size);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = (uint8_t)(file_size & 0xFFu);
    bmp[3] = (uint8_t)((file_size >> 8) & 0xFFu);
    bmp[4] = (uint8_t)((file_size >> 16) & 0xFFu);
    bmp[5] = (uint8_t)((file_size >> 24) & 0xFFu);
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = (uint8_t)(width & 0xFFu);
    bmp[19] = (uint8_t)((width >> 8) & 0xFFu);
    bmp[20] = (uint8_t)((width >> 16) & 0xFFu);
    bmp[21] = (uint8_t)((width >> 24) & 0xFFu);
    bmp[22] = (uint8_t)(height & 0xFFu);
    bmp[23] = (uint8_t)((height >> 8) & 0xFFu);
    bmp[24] = (uint8_t)((height >> 16) & 0xFFu);
    bmp[25] = (uint8_t)((height >> 24) & 0xFFu);
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[34] = (uint8_t)(pixel_bytes & 0xFFu);
    bmp[35] = (uint8_t)((pixel_bytes >> 8) & 0xFFu);
    bmp[36] = (uint8_t)((pixel_bytes >> 16) & 0xFFu);
    bmp[37] = (uint8_t)((pixel_bytes >> 24) & 0xFFu);

    const uint8_t *src = snap->data;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *src_row = src + (size_t)y * snap->header.stride;
        uint8_t *dst_row = bmp + 54u + (size_t)(height - 1u - y) * row_size;
        for (uint32_t x = 0; x < width; ++x) {
            uint16_t px = ((uint16_t)src_row[x * 2u] << 8) | src_row[x * 2u + 1u];
            uint8_t r5 = (px >> 11) & 0x1Fu;
            uint8_t g6 = (px >> 5) & 0x3Fu;
            uint8_t b5 = px & 0x1Fu;
            dst_row[x * 3u + 2u] = (uint8_t)((r5 * 255u) / 31u);
            dst_row[x * 3u + 1u] = (uint8_t)((g6 * 255u) / 63u);
            dst_row[x * 3u + 0u] = (uint8_t)((b5 * 255u) / 31u);
        }
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    result = httpd_resp_send(req, (const char *)bmp, file_size);

done:
    if (snap) {
        lv_draw_buf_destroy(snap);
    }
    if (bmp) {
        heap_caps_free(bmp);
    }
    return result;
}

static esp_err_t voice_start_handler(httpd_req_t *req)
{
    bool started = direct_ai_start_request("http");
    httpd_resp_set_type(req, "application/json");
    if (started) {
        return httpd_resp_sendstr(req, "{\"ok\":true,\"state\":\"REC\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"state\":\"busy\"}");
}

static esp_err_t http_server_start(void)
{
    if (g_app.httpd) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 16384;

    esp_err_t err = httpd_start(&g_app.httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &index_uri);

    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &save_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &status_uri);

    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_get_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &wifi_scan_uri);

    httpd_uri_t wifi_select_uri = {
        .uri = "/api/wifi/select",
        .method = HTTP_GET,
        .handler = wifi_select_get_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &wifi_select_uri);

    httpd_uri_t ui_debug_uri = {
        .uri = "/api/ui",
        .method = HTTP_GET,
        .handler = ui_debug_get_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &ui_debug_uri);

    httpd_uri_t screen_bmp_uri = {
        .uri = "/api/screen.bmp",
        .method = HTTP_GET,
        .handler = screen_bmp_get_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &screen_bmp_uri);

    httpd_uri_t voice_start_get_uri = {
        .uri = "/api/voice/start",
        .method = HTTP_GET,
        .handler = voice_start_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &voice_start_get_uri);

    httpd_uri_t voice_start_post_uri = {
        .uri = "/api/voice/start",
        .method = HTTP_POST,
        .handler = voice_start_handler,
    };
    httpd_register_uri_handler(g_app.httpd, &voice_start_post_uri);
    return ESP_OK;
}

static bool str_to_ip4(const char *src, esp_ip4_addr_t *dst)
{
    return src && src[0] && dst && esp_netif_str_to_ip4(src, dst) == ESP_OK;
}

static void apply_dns_config_for_slot(int slot)
{
    if (!g_app.sta_netif) {
        return;
    }

    const wifi_ip_config_t *net = config_wifi_net_const(&g_app.cfg, slot);
    if (!net) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (str_to_ip4(net->dns1, &dns.ip.u_addr.ip4)) {
        (void)esp_netif_set_dns_info(g_app.sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (str_to_ip4(net->dns2, &dns.ip.u_addr.ip4)) {
        (void)esp_netif_set_dns_info(g_app.sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
    }
}

static void apply_ip_config_for_slot(int slot)
{
    if (!g_app.sta_netif) {
        return;
    }

    const wifi_ip_config_t *net = config_wifi_net_const(&g_app.cfg, slot);
    if (!net) {
        return;
    }

    esp_netif_ip_info_t ip = {0};
    if (!str_to_ip4(net->static_ip, &ip.ip) ||
        !str_to_ip4(net->static_netmask, &ip.netmask) ||
        !str_to_ip4(net->static_gateway, &ip.gw)) {
        esp_err_t err = esp_netif_dhcpc_start(g_app.sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "dhcp start failed: %s", esp_err_to_name(err));
        }
        apply_dns_config_for_slot(slot);
        return;
    }

    esp_err_t err = esp_netif_dhcpc_stop(g_app.sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcp stop failed: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(g_app.sta_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "static ip failed: %s", esp_err_to_name(err));
    }
    apply_dns_config_for_slot(slot);
}

static void configure_timezone_once(void)
{
    static bool timezone_configured;
    if (timezone_configured) {
        return;
    }
    timezone_configured = true;
    setenv("TZ", "CST-8", 1);
    tzset();
}

static void start_sntp_once(void)
{
    if (g_app.sntp_started) {
        return;
    }
    g_app.sntp_started = true;
    configure_timezone_once();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, "pool.ntp.org");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
    esp_sntp_setservername(2, "time.cloudflare.com");
#endif
    esp_sntp_init();
}

static int configured_wifi_slot_count(void)
{
    int count = 0;
    for (int i = 0; i < WIFI_SLOT_COUNT; i++) {
        if (config_wifi_slot_used(&g_app.cfg, i)) {
            count++;
        }
    }
    return count;
}

static int wifi_slot_at_try_index(int index)
{
    if (g_app.preferred_wifi_slot >= 0 &&
        g_app.preferred_wifi_slot < WIFI_SLOT_COUNT &&
        config_wifi_slot_used(&g_app.cfg, g_app.preferred_wifi_slot)) {
        if (index == 0) {
            return g_app.preferred_wifi_slot;
        }
        index--;
    }

    for (int slot = 0; slot < WIFI_SLOT_COUNT; slot++) {
        if (slot == g_app.preferred_wifi_slot || !config_wifi_slot_used(&g_app.cfg, slot)) {
            continue;
        }
        if (index == 0) {
            return slot;
        }
        index--;
    }
    return -1;
}

static int wifi_slot_from_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return -1;
    }
    for (int slot = 0; slot < WIFI_SLOT_COUNT; slot++) {
        const char *slot_ssid = config_wifi_ssid_const(&g_app.cfg, slot);
        if (slot_ssid && strcmp(slot_ssid, ssid) == 0) {
            return slot;
        }
    }
    return -1;
}

static int wifi_current_connected_slot(void)
{
    if (!g_app.sta_connected) {
        return -1;
    }
    if (g_app.active_wifi_slot >= 0 && g_app.active_wifi_slot < WIFI_SLOT_COUNT) {
        return g_app.active_wifi_slot;
    }

    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        int slot = wifi_slot_from_ssid((const char *)ap_info.ssid);
        if (slot >= 0) {
            g_app.active_wifi_slot = slot;
            return slot;
        }
    }
    return -1;
}

static esp_err_t wifi_apply_sta_slot(int slot)
{
    const char *ssid = config_wifi_ssid_const(&g_app.cfg, slot);
    const char *password = config_wifi_password_const(&g_app.cfg, slot);
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_cfg = {0};
    copy_string((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), ssid);
    copy_string((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), password);
    sta_cfg.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    return esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

static esp_err_t wifi_sta_connect_once(int timeout_ms)
{
    if (!g_app.wifi_started || !g_app.sta_configured) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_app.sta_connected) {
        return ESP_OK;
    }

    int slot_count = configured_wifi_slot_count();
    if (slot_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int slot_timeout_ms = timeout_ms / slot_count;
    if (slot_timeout_ms < 3000) {
        slot_timeout_ms = 3000;
    }

    esp_err_t last_err = ESP_ERR_TIMEOUT;
    for (int try_index = 0; try_index < slot_count; try_index++) {
        int slot = wifi_slot_at_try_index(try_index);
        if (slot < 0) {
            continue;
        }

        g_app.sta_connect_requested = false;
        (void)esp_wifi_disconnect();
        g_app.sta_connected = false;
        g_app.active_wifi_slot = -1;
        g_app.sta_ip[0] = '\0';
        vTaskDelay(pdMS_TO_TICKS(120));

        apply_ip_config_for_slot(slot);

        esp_err_t err = wifi_apply_sta_slot(slot);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sta slot %d config failed: %s", slot + 1, esp_err_to_name(err));
            last_err = err;
            continue;
        }

        ESP_LOGI(TAG, "trying WiFi slot %d ssid=%s", slot + 1,
                 config_wifi_ssid_const(&g_app.cfg, slot));
        g_app.sta_connect_requested = true;
        g_app.pending_wifi_slot = slot;
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            g_app.sta_connect_requested = false;
            ESP_LOGW(TAG, "sta slot %d connect start failed: %s", slot + 1, esp_err_to_name(err));
            last_err = err;
            continue;
        }

        int waited_ms = 0;
        int retry_ms = 0;
        while (!g_app.sta_connected && waited_ms < slot_timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited_ms += 200;
            retry_ms += 200;
            if (!g_app.sta_connected && retry_ms >= WIFI_CONNECT_RETRY_MS) {
                retry_ms = 0;
                ESP_LOGI(TAG, "retry WiFi slot %d ssid=%s", slot + 1,
                         config_wifi_ssid_const(&g_app.cfg, slot));
                err = esp_wifi_connect();
                if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "sta slot %d retry failed: %s",
                             slot + 1, esp_err_to_name(err));
                }
            }
        }
        if (g_app.sta_connected) {
            g_app.active_wifi_slot = slot;
            g_app.pending_wifi_slot = -1;
            return ESP_OK;
        }
        last_err = ESP_ERR_TIMEOUT;
    }

    g_app.sta_connect_requested = false;
    g_app.pending_wifi_slot = -1;
    g_app.active_wifi_slot = -1;
    return g_app.sta_connected ? ESP_OK : last_err;
}

static void wifi_sta_disconnect_now(void)
{
    g_app.sta_connect_requested = false;
    if (g_app.wifi_started && g_app.sta_configured) {
        (void)esp_wifi_disconnect();
    }
    g_app.sta_connected = false;
    g_app.pending_wifi_slot = -1;
    g_app.active_wifi_slot = -1;
    g_app.sta_ip[0] = '\0';
}

static void wifi_sta_disconnect_when_idle(void)
{
    if (VOICE_KEEP_STA_CONNECTED) {
        g_app.sta_connect_requested = false;
        ESP_LOGI(TAG, "wifi: keeping STA connected for voice debug");
        return;
    }
    wifi_sta_disconnect_now();
    (void)wifi_apply_power_save_mode();
}

static esp_err_t wifi_configure_ap(void)
{
    wifi_config_t ap_cfg = {0};
    copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), g_app.ap_ssid);
    ap_cfg.ap.ssid_len = strlen(g_app.ap_ssid);
    copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), AP_PASSWORD);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ap config failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t wifi_apply_power_save_mode(void)
{
    if (!g_app.wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }

    g_app.sta_configured = config_has_wifi(&g_app.cfg);
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (g_app.ap_config_enabled || !g_app.sta_configured) {
        mode = g_app.sta_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    } else {
        mode = WIFI_MODE_STA;
    }

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "wifi mode: %s config_ap=%d sta_cfg=%d",
                 mode == WIFI_MODE_APSTA ? "APSTA" :
                 mode == WIFI_MODE_AP ? "AP" :
                 mode == WIFI_MODE_STA ? "STA" : "NULL",
                 g_app.ap_config_enabled, g_app.sta_configured);
    }
    return err;
}

static esp_err_t wifi_enable_config_ap(void)
{
    if (!g_app.wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }
    g_app.ap_config_enabled = true;
    esp_err_t err = wifi_apply_power_save_mode();
    if (err == ESP_OK) {
        err = wifi_configure_ap();
    }
    if (err == ESP_OK) {
        (void)http_server_start();
        copy_string(g_app.market_status, sizeof(g_app.market_status), "CFG AP");
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_app.sta_configured && g_app.sta_connect_requested) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_app.sta_connected = false;
        g_app.active_wifi_slot = -1;
        g_app.sta_ip[0] = '\0';
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        g_app.ap_started = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        g_app.ap_started = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        g_app.last_activity_us = esp_timer_get_time();
        set_sleeping(false);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(g_app.sta_ip, sizeof(g_app.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        g_app.sta_connected = true;
        if (g_app.pending_wifi_slot >= 0 && g_app.pending_wifi_slot < WIFI_SLOT_COUNT) {
            g_app.active_wifi_slot = g_app.pending_wifi_slot;
            g_app.pending_wifi_slot = -1;
        }
        int slot = wifi_current_connected_slot();
        if (slot >= 0) {
            apply_dns_config_for_slot(slot);
        }
        start_sntp_once();
    }
    ui_refresh_now();
}

static esp_err_t wifi_start(void)
{
    WL_RETURN_ON_ERROR(esp_netif_init(), "netif init failed");
    WL_RETURN_ON_ERROR(esp_event_loop_create_default(), "event loop failed");

    g_app.sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    WL_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), "wifi init failed");
    WL_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                           wifi_event_handler, NULL, NULL),
                       "wifi event failed");
    WL_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                           wifi_event_handler, NULL, NULL),
                       "ip event failed");

    g_app.sta_configured = config_has_wifi(&g_app.cfg);
    g_app.ap_config_enabled = !g_app.sta_configured;
    wifi_mode_t mode = g_app.ap_config_enabled ?
                       (g_app.sta_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP) :
                       WIFI_MODE_STA;
    ESP_LOGI(TAG, "wifi start: configured=%d slots=%d preferred=%d mode=%s",
             g_app.sta_configured, configured_wifi_slot_count(),
             g_app.preferred_wifi_slot,
             mode == WIFI_MODE_APSTA ? "APSTA" :
             mode == WIFI_MODE_AP ? "AP" :
             mode == WIFI_MODE_STA ? "STA" : "NULL");
    WL_RETURN_ON_ERROR(esp_wifi_set_mode(mode), "wifi mode failed");

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        WL_RETURN_ON_ERROR(wifi_configure_ap(), "ap config failed");
    }

    if (g_app.sta_configured) {
        int first_slot = wifi_slot_at_try_index(0);
        if (first_slot < 0) {
            for (int slot = 0; slot < WIFI_SLOT_COUNT; slot++) {
                if (config_wifi_slot_used(&g_app.cfg, slot)) {
                    first_slot = slot;
                    break;
                }
            }
        }
        if (first_slot >= 0) {
            ESP_LOGI(TAG, "wifi start: first slot=%d ssid=%s",
                     first_slot + 1, config_wifi_ssid_const(&g_app.cfg, first_slot));
            apply_ip_config_for_slot(first_slot);
            WL_RETURN_ON_ERROR(wifi_apply_sta_slot(first_slot), "sta config failed");
        }
    }

    WL_RETURN_ON_ERROR(esp_wifi_start(), "wifi start failed");
    g_app.wifi_started = true;
    (void)esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    (void)esp_wifi_set_max_tx_power(32);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Watch Lite direct booting");
    configure_timezone_once();

    gpio_config_t amp_gpio = {
        .pin_bit_mask = 1ULL << BSP_POWER_AMP_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&amp_gpio);
    gpio_set_level(BSP_POWER_AMP_IO, 0);
    power_button_gpio_prepare_active();
    log_wakeup_reason();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    config_load(&g_app.cfg);
    (void)ai_profiles_load(&g_app.ai_profiles, &g_app.cfg);
    ai_profile_apply(g_app.ai_profiles.active_index, false);
    make_ap_ssid();
    app_state_set_defaults();
    log_config_summary();
    (void)market_cache_load();
    g_app.net_lock = xSemaphoreCreateMutex();
    if (!g_app.net_lock) {
        ESP_LOGW(TAG, "network lock unavailable");
    }
    g_app.audio_lock = xSemaphoreCreateMutex();
    if (!g_app.audio_lock) {
        ESP_LOGW(TAG, "audio lock unavailable");
    }
    g_app.last_activity_us = esp_timer_get_time();

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    (void)display;
    s_ai_text_filter_enabled = true;
    restore_display_brightness();

    if (bsp_display_lock(5000)) {
        ui_create_home();
        ui_create_ai();
        ui_create_chart();
        ui_show_home();
        lv_timer_create(ui_timer_cb, 1000, NULL);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }

    if (pmic_init() == ESP_OK) {
        battery_update_once();
    }

    esp_err_t wifi_err = wifi_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed: %s", esp_err_to_name(wifi_err));
    }
    if (g_app.ap_config_enabled) {
        http_server_start();
    }
    motion_task_start_once();
    boot_button_task_start_once();
    home_market_refresh_async();
    ui_refresh_now();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive sta=%d cfg=%d slots=%d ip=%s ap=%s sleep=%d market=%s",
                 g_app.sta_connected, g_app.sta_configured, configured_wifi_slot_count(),
                 g_app.sta_ip, g_app.ap_ssid, g_app.screen_sleeping, g_app.market_status);
    }
}

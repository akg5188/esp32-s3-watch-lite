#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "lvgl.h"

#define TAG "lvgl_test"

#define SAFE_X 18
#define SAFE_Y 24
#define SAFE_W 374
#define SAFE_H 454

static lv_obj_t *g_box;
static lv_obj_t *g_tick_label;
static int g_tick;

static void lvgl_test_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!g_box || !g_tick_label) {
        return;
    }

    int x = SAFE_X + 24 + ((g_tick * 18) % 286);
    lv_obj_set_pos(g_box, x, SAFE_Y + 316);

    char text[64];
    snprintf(text, sizeof(text), "LVGL tick %d  x=%d", g_tick, x);
    lv_label_set_text(g_tick_label, text);
    g_tick++;
}

static lv_obj_t *create_color_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 10, 0);
    return obj;
}

static void create_lvgl_test_ui(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07111F), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *safe = lv_obj_create(scr);
    lv_obj_set_pos(safe, SAFE_X, SAFE_Y);
    lv_obj_set_size(safe, SAFE_W, SAFE_H);
    lv_obj_set_style_bg_color(safe, lv_color_hex(0x0D1A2D), 0);
    lv_obj_set_style_bg_opa(safe, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(safe, lv_color_hex(0x274A69), 0);
    lv_obj_set_style_border_width(safe, 2, 0);
    lv_obj_set_style_radius(safe, 34, 0);
    lv_obj_set_style_pad_all(safe, 0, 0);
    lv_obj_clear_flag(safe, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(safe);
    lv_label_set_text(title, "WatchAI LVGL Display Test");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8F1FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);

    lv_obj_t *subtitle = lv_label_create(safe);
    lv_label_set_text(subtitle, "Rounded safe-area layout");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8FA3C2), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 24, 52);

    create_color_block(safe, 24, 92, 72, 94, 0xE94F37);
    create_color_block(safe, 108, 92, 72, 94, 0x23CE6B);
    create_color_block(safe, 192, 92, 72, 94, 0x2D7FF9);
    create_color_block(safe, 276, 92, 72, 94, 0xF4F7FF);

    lv_obj_t *card = lv_obj_create(safe);
    lv_obj_set_pos(card, 24, 210);
    lv_obj_set_size(card, 326, 92);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x10243C), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2B5B89), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 18, 0);

    lv_obj_t *card_label = lv_label_create(card);
    lv_label_set_text(card_label, "No content in clipped corners.\nButtons/text stay inside.");
    lv_obj_set_style_text_color(card_label, lv_color_hex(0xD8E7FF), 0);
    lv_obj_align(card_label, LV_ALIGN_CENTER, 0, 0);

    g_box = create_color_block(scr, SAFE_X + 24, SAFE_Y + 316, 48, 48, 0xFFB000);

    g_tick_label = lv_label_create(safe);
    lv_label_set_text(g_tick_label, "LVGL tick 0");
    lv_obj_set_style_text_color(g_tick_label, lv_color_hex(0x45D6C2), 0);
    lv_obj_align(g_tick_label, LV_ALIGN_BOTTOM_LEFT, 24, -34);

    lv_obj_t *corner_hint = lv_label_create(safe);
    lv_label_set_text(corner_hint, "Corners are intentionally empty");
    lv_obj_set_style_text_color(corner_hint, lv_color_hex(0x6F83A3), 0);
    lv_obj_align(corner_hint, LV_ALIGN_BOTTOM_RIGHT, -24, -12);

    lv_screen_load(scr);
    lv_timer_create(lvgl_test_timer_cb, 800, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "LVGL-only test firmware booting");

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_brightness_set(100);

    if (bsp_display_lock(5000)) {
        create_lvgl_test_ui();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL");
    }

    while (true) {
        ESP_LOGI(TAG, "alive tick=%d", g_tick);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

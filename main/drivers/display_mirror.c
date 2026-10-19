/**
 * @file display_mirror.c
 * @brief External ILI9341 second-screen mirror (see display_mirror.h).
 *
 * The built-in display (display.c) keeps a full RGB565 framebuffer of
 * everything it draws and exposes it via display_get_framebuffer(). This
 * module nearest-neighbour-scales that 240x135 source up to a 320x240
 * ILI9341 and pushes it over the SD card's SPI3 bus on a timer task, so
 * none of the existing UI code has to know the second screen exists.
 */

#include "display_mirror.h"

#if DISPLAY_MIRROR_ENABLED

#include "display.h"              // DISPLAY_WIDTH/HEIGHT, display_get_framebuffer()
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"      // managed component: espressif/esp_lcd_ili9341
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "DISP_MIRROR";

// ── Wiring (shared SD/SPI3 bus + own CS/DC/RST) ──────────────────────────────
#define MIRROR_SPI_HOST     SPI3_HOST   // same host screenshot.c uses for the SD
#define MIRROR_PIN_SCLK     40          // shared SD CLK
#define MIRROR_PIN_MOSI     14          // shared SD MOSI
#define MIRROR_PIN_MISO     39          // shared SD MISO (unused by panel, needed by bus)
#define MIRROR_PIN_CS       5
#define MIRROR_PIN_DC       6
#define MIRROR_PIN_RST      3

// ── Panel geometry / timing ─────────────────────────────────────────────────
#define MIRROR_W            320         // ILI9341 landscape width
#define MIRROR_H            240         // ILI9341 landscape height
#define MIRROR_SPI_FREQ_HZ  (26 * 1000 * 1000)  // TUNE: lower (e.g. 20MHz) if dupont wiring glitches
#define MIRROR_REFRESH_MS   100         // TUNE: ~10fps; raise if it contends with SD during wardrive

// ── Colour / orientation knobs (TUNE on first boot) ─────────────────────────
// If colours look wrong: red/blue swapped -> flip MIRROR_BGR; whole image
// inverted (photo-negative) -> flip MIRROR_INVERT. Image rotated/flipped ->
// adjust swap_xy / mirror args in display_mirror_init().
#define MIRROR_BGR          1           // 1 = BGR element order (typical red ILI9341 boards)
#define MIRROR_INVERT       0           // 1 = invert colours

#define SRC_W   DISPLAY_WIDTH   // 240
#define SRC_H   DISPLAY_HEIGHT  // 135

static esp_lcd_panel_handle_t s_panel = NULL;
static volatile bool s_enabled = true;
static uint16_t s_line[MIRROR_W];       // one output row, byte-swapped
static uint16_t s_sx_lut[MIRROR_W];     // src column for each output column
static uint8_t  s_sy_lut[MIRROR_H];     // src row for each output row (src height < 256)

static void mirror_task(void *arg)
{
    const uint16_t *fb = display_get_framebuffer();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MIRROR_REFRESH_MS));
        if (!s_enabled || s_panel == NULL || fb == NULL) continue;

        for (int oy = 0; oy < MIRROR_H; oy++) {
            const uint16_t *src_row = &fb[(int)s_sy_lut[oy] * SRC_W];
            for (int ox = 0; ox < MIRROR_W; ox++) {
                uint16_t px = src_row[s_sx_lut[ox]];
                // framebuffer stores un-swapped RGB565; panel wants big-endian
                s_line[ox] = (uint16_t)((px >> 8) | (px << 8));
            }
            esp_lcd_panel_draw_bitmap(s_panel, 0, oy, MIRROR_W, oy + 1, s_line);
        }
    }
}

esp_err_t display_mirror_init(void)
{
    ESP_LOGI(TAG, "Bringing up external ILI9341 mirror on SPI3 (shared SD bus)...");

    // Precompute nearest-neighbour scale LUTs (240x135 -> 320x240).
    for (int ox = 0; ox < MIRROR_W; ox++) s_sx_lut[ox] = (uint16_t)(ox * SRC_W / MIRROR_W);
    for (int oy = 0; oy < MIRROR_H; oy++) s_sy_lut[oy] = (uint8_t)(oy * SRC_H / MIRROR_H);

    // The SD bus may or may not be up yet (screenshot.c mounts lazily).
    // Initialise it here; tolerate INVALID_STATE if the SD got there first.
    spi_bus_config_t buscfg = {
        .mosi_io_num = MIRROR_PIN_MOSI,
        .miso_io_num = MIRROR_PIN_MISO,
        .sclk_io_num = MIRROR_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // Must cover both the panel's line blit and the SD's transfers, since
        // whichever driver inits the bus first wins this value (shared bus).
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(MIRROR_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = MIRROR_PIN_DC,
        .cs_gpio_num = MIRROR_PIN_CS,
        .pclk_hz = MIRROR_SPI_FREQ_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)MIRROR_SPI_HOST, &io_cfg, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel_io_spi failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = MIRROR_PIN_RST,
#if MIRROR_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_ili9341 failed: %s", esp_err_to_name(ret));
        s_panel = NULL;
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));     // landscape 320x240
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, true));  // 180° (panel mounted upside down)
#if MIRROR_INVERT
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Paint black so a fresh panel isn't noise until the first mirror frame.
    memset(s_line, 0, sizeof(s_line));
    for (int oy = 0; oy < MIRROR_H; oy++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, oy, MIRROR_W, oy + 1, s_line);
    }

    xTaskCreate(mirror_task, "disp_mirror", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "ILI9341 mirror ready (%dx%d <- %dx%d, %d MHz)",
             MIRROR_W, MIRROR_H, SRC_W, SRC_H, MIRROR_SPI_FREQ_HZ / 1000000);
    return ESP_OK;
}

void display_mirror_set_enabled(bool enabled) { s_enabled = enabled; }
bool display_mirror_is_present(void) { return s_panel != NULL; }

#else // !DISPLAY_MIRROR_ENABLED

#include "display_mirror.h"
esp_err_t display_mirror_init(void) { return ESP_OK; }
void display_mirror_set_enabled(bool enabled) { (void)enabled; }
bool display_mirror_is_present(void) { return false; }

#endif // DISPLAY_MIRROR_ENABLED

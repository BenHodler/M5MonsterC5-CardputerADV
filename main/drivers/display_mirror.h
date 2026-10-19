/**
 * @file display_mirror.h
 * @brief External ILI9341 second-screen mirror for the Cardputer ADV.
 *
 * Mirrors the built-in ST7789 (240x135) framebuffer onto an external
 * ILI9341 panel wired onto the SD card's SPI3 bus (shared SCLK/MOSI,
 * separate CS). Scales the source frame up to the ILI9341 and refreshes
 * on a low-priority background task so no UI draw path has to change.
 *
 * Hardware (PalPack dual-screen wiring):
 *   ILI9341 SCK  -> GPIO40  (shared with SD CLK)
 *   ILI9341 MOSI -> GPIO14  (shared with SD MOSI)
 *   ILI9341 CS   -> GPIO5
 *   ILI9341 DC   -> GPIO6
 *   ILI9341 RST  -> GPIO3
 *   ILI9341 VCC/LED jumped to 5V (backlight always on)
 *
 * NOTE: GPIO5/GPIO14 are the firmware's "LoRa CAP" reset/NSS pins. When
 * this mirror is compiled in, screenshot.c must NOT park those pins, or it
 * will fight the ILI9341's CS/MOSI. The DISPLAY_MIRROR_ENABLED guard below
 * handles that. This build targets hardware with NO SX1262 LoRa radio.
 */

#ifndef DISPLAY_MIRROR_H
#define DISPLAY_MIRROR_H

#include "esp_err.h"
#include <stdbool.h>

// Compile-time switch. Set to 0 to drop the mirror (and restore the normal
// LoRa-CAP pin parking in screenshot.c) without removing the files.
#define DISPLAY_MIRROR_ENABLED 1

/**
 * @brief Bring up the external ILI9341 on the shared SD/SPI3 bus and start
 *        the mirror refresh task. Call once, after display_init().
 * @return ESP_OK on success; ESP_FAIL if the panel could not be created.
 */
esp_err_t display_mirror_init(void);

/** @brief Enable/disable mirroring at runtime (panel stays initialised). */
void display_mirror_set_enabled(bool enabled);

/** @brief True once the ILI9341 panel has been created successfully. */
bool display_mirror_is_present(void);

#endif // DISPLAY_MIRROR_H

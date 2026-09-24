#pragma once

// Board support for the Sunton ESP32-3248S035C wall panel:
//   ST7796 320x480 SPI panel, GT911 capacitive touch, RGB status LED,
//   PWM backlight, CDS light sensor, speaker, microSD slot.
//
// The panel itself is driven through TFT_eSPI (configured in
// lib/TFT_eSPI/User_Setup.h); everything else is handled here. LVGL is brought
// up by lvgl_begin(), which owns the display and input driver registration.

#ifndef ESP32_3248S035C
#define ESP32_3248S035C
#endif

#if !defined(TFT_ORIENTATION_PORTRAIT) && !defined(TFT_ORIENTATION_LANDSCAPE) &&   \
    !defined(TFT_ORIENTATION_PORTRAIT_INV) && !defined(TFT_ORIENTATION_LANDSCAPE_INV)
#define TFT_ORIENTATION_LANDSCAPE
#endif

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#ifdef ESP32_3248S035C
#define TFT_WIDTH 320
#define TFT_HEIGHT 480
#define ST7796
#define ST7796_SPI_SCLK 14
#define ST7796_SPI_MOSI 13
#define ST7796_SPI_MISO 12
#define ST7796_PIN_CS 15
#define ST7796_PIN_DC 2
#define ST7796_SPI_FREQ 80000000
#define ST7796_PIN_BL 27
#define ST7796_PWM_CHANNEL_BL 12
#define ST7796_PWM_FREQ_BL 5000
#define ST7796_PWM_BITS_BL 8
#define ST7796_PWM_MAX_BL ((1 << ST7796_PWM_BITS_BL) - 1)
#define GT911
#define GT911_IIC_SDA 33
#define GT911_IIC_SCL 32
#define GT911_IIC_RST 25
#define GT911_I2C_SLAVE_ADDR 0x5D
#define GT911_MAX_CONTACTS 5
#define GT911_PRODUCT_ID1 0x8140
#define GT911_REG_COORD_ADDR 0x814E
#define GT911_TRACK_ID1 0x814F
#define GT911_PRODUCT_ID_LEN 4

#if !defined(TFT_ORIENTATION_PORTRAIT) && !defined(TFT_ORIENTATION_LANDSCAPE) && !defined(TFT_ORIENTATION_PORTRAIT_INV) && !defined(TFT_ORIENTATION_LANDSCAPE_INV)
#error Please define orientation: TFT_ORIENTATION_PORTRAIT, TFT_ORIENTATION_LANDSCAPE, TFT_ORIENTATION_PORTRAIT_INV or TFT_ORIENTATION_LANDSCAPE_INV
#endif

extern TwoWire i2c_gt911;
#endif

// Build in RGB LED
#define LED_PIN_R 4
#define LED_PIN_G 16
#define LED_PIN_B 17
// PWM channels for RGB
#define LED_PWM_FREQ 5000
#define LED_PWM_CHANNEL_R 13
#define LED_PWM_CHANNEL_G 14
#define LED_PWM_CHANNEL_B 15
#define LED_PWM_BITS 8
#define LED_PWM_MAX ((1 << LED_PWM_BITS) - 1)

// Photo resistor
#define CDS_PIN 34 // ANALOG_PIN_0

// Audio out
#define AUDIO_PIN 26

// TF Card
#define TF_PIN_CS 5
#define TF_PIN_MOSI 23
#define TF_PIN_SCLK 18
#define TF_PIN_MISC 19

// TFT_eSPI rotation that matches the orientation selected above.
//
// This constant is the single source of truth for orientation: the display
// driver programs the panel from it, and gt911_read_touches() rotates the touch
// controller's readings by the same value. The GT911 always reports in the
// panel's portrait frame (the setRotation(0) corner), so changing this number
// keeps touch and display in agreement on its own -- there is no second place
// to edit.
//
// If the image is upside down, change this. If the image is right but touch is
// inverted, the two have genuinely drifted apart and gt911_read_touches() is
// where to look.
#if defined(TFT_ORIENTATION_PORTRAIT)
#define DISPLAY_ROTATION 0
#elif defined(TFT_ORIENTATION_LANDSCAPE)
#define DISPLAY_ROTATION 1
#elif defined(TFT_ORIENTATION_PORTRAIT_INV)
#define DISPLAY_ROTATION 2
#else
#define DISPLAY_ROTATION 3
#endif

// The panel is physically 320x480; rotating swaps what the UI sees. Everything
// that lays out pixels must use these, not TFT_WIDTH/TFT_HEIGHT -- those stay
// the panel's own portrait dimensions and are what the touch transform is
// expressed in.
#if DISPLAY_ROTATION == 1 || DISPLAY_ROTATION == 3
#define SCREEN_WIDTH TFT_HEIGHT
#define SCREEN_HEIGHT TFT_WIDTH
#else
#define SCREEN_WIDTH TFT_WIDTH
#define SCREEN_HEIGHT TFT_HEIGHT
#endif

// Height in pixels of the LVGL render buffer, at 2 bytes per pixel across the
// full screen width: 12 lines of a 480px-wide landscape screen is ~11.5KB.
//
// Keep this small. The board has no PSRAM, so this competes directly with the
// WiFi stack and with mbedTLS, whose handshake needs tens of KB contiguous. A
// larger buffer only means fewer flush calls -- worth very little on a mostly
// static UI, and it will starve the MQTT connection long before it is missed.
#define LVGL_BUFFER_LINES 12

/// @brief Bring up the panel pins, backlight, RGB LED and touch controller.
/// Must be called before lvgl_begin().
void board_init();

/// @brief Initialise LVGL and register the display and touch drivers.
/// Call once, after board_init().
void lvgl_begin();

/// @brief Pump LVGL's timers. Call from loop() as often as possible.
void lvgl_tick();

void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
void set_led(uint8_t red, uint8_t green, uint8_t blue);
void set_led(uint32_t hex_code);

/// @brief Set the backlight level, 0 (off) to 255 (full).
void set_backlight(uint8_t brightness);

/// @brief Read the ambient light sensor, 0-4095. Higher is brighter.
uint16_t read_light_sensor();

/// @brief If not null, the pointed function is called every time a touch is detected.
extern void (*touch_callback)();

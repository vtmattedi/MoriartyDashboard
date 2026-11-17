#pragma once

// Board support for the Sunton ESP32-3248S035C panel, in its battery enclosure:
//   ST7796 320x480 SPI panel, GT911 capacitive touch, RGB status LED (held
//   off), PWM backlight, CDS light sensor, speaker, microSD slot -- plus the one
//   thing the enclosure adds, a battery divider on GPIO 35.
//
// Native ESP-IDF, no Arduino: the panel goes through esp_lcd's SPI panel-IO,
// touch through the I2C master driver, the backlight and LED through LEDC, and
// the two analogue inputs through the one-shot ADC driver with the chip's
// factory calibration. LVGL is brought up by lvgl_begin(), which owns the
// display and input driver registration.

#ifndef ESP32_3248S035C
#define ESP32_3248S035C
#endif

#if !defined(TFT_ORIENTATION_PORTRAIT) && !defined(TFT_ORIENTATION_LANDSCAPE) &&   \
    !defined(TFT_ORIENTATION_PORTRAIT_INV) && !defined(TFT_ORIENTATION_LANDSCAPE_INV)
#define TFT_ORIENTATION_PORTRAIT
#endif

#include <stdint.h>
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
// HSPI, on its native pins. 40MHz is what the panel was actually clocked at
// before the port: the old setup asked for 65MHz, which the SPI divider rounds
// down to this.
#define ST7796_SPI_HOST SPI2_HOST
#define ST7796_SPI_FREQ 40000000
#define ST7796_PIN_BL 27
#define ST7796_PWM_FREQ_BL 2500
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
#endif

// Built-in RGB LED, common-anode. Not used: board_init() holds all three pins
// high, which is off, and nothing turns it on -- a lit LED is battery spent on
// nothing. There is deliberately no function to drive it.
#define LED_PIN_R 4
#define LED_PIN_G 16
#define LED_PIN_B 17

// Photo resistor: ADC1 channel 6
#define CDS_PIN 34

// Battery sense, added with the enclosure: the cell's positive terminal through
// a 1M / 1M divider to GPIO 35 (ADC1 channel 7, input-only, on the P3 header).
// ADC1 on purpose -- ADC2 is unusable while the Wi-Fi driver is running.
//
// The pin therefore sees half the cell: 2.1V at a full 4.2V, inside the 11dB
// range. BAT_DIVIDER_MILLI is (Rtop + Rbottom) / Rbottom in thousandths, so a
// different pair of resistors, or trimming against a multimeter, is this one
// number.
//
// Two megohms draws about 2uA, which is why the values are that large -- and
// also why the reading needs help: 500k of source impedance cannot charge the
// ADC's sampling capacitor in time, and the pin reads low and noisy unless
// something holds the node steady. 100nF from GPIO 35 to ground does it.
#define BAT_PIN 35
#define BAT_DIVIDER_MILLI 2000

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
// full screen width: 12 lines of the 320px-wide portrait screen is ~7.7KB.
//
// Keep this small. The board has no PSRAM, so this competes directly with the
// WiFi stack and with mbedTLS, whose handshake needs tens of KB contiguous. A
// larger buffer only means fewer flush calls -- worth very little on a mostly
// static UI, and it will starve the MQTT connection long before it is missed.
#define LVGL_BUFFER_LINES 12

/// @brief Bring up the panel pins, backlight, light sensor and touch controller,
/// and hold the RGB LED off.
/// Must be called before lvgl_begin().
void board_init();

/// @brief Initialise LVGL and register the display and touch drivers.
/// Call once, after board_init().
void lvgl_begin();

/// @brief Pump LVGL's timers. Call from loop() as often as possible.
void lvgl_tick();


/// @brief Set the backlight level, 0 (off) to 255 (full).
void set_backlight(uint8_t brightness);

/// @brief Read the ambient light sensor, raw, 0-4095.
uint16_t read_light_sensor();

/// @brief Battery voltage in millivolts, already scaled back up through the
/// divider. Averaged over a burst of samples; around 0 with no cell fitted.
uint16_t read_battery_mv();

/// @brief Put the ST7796 into its sleep-in state, or bring it back. The panel
/// keeps its frame memory while asleep. Waking blocks for the 120ms the
/// controller needs before it accepts anything else. The backlight is separate:
/// see set_backlight().
void display_sleep(bool asleep);

/// @brief Light-sleep the CPU until the touch panel is touched.
///
/// Blocks for as long as that takes. The caller is expected to have already
/// switched off everything it owns -- radio, backlight, display -- because this
/// only stops the processor: it wakes on a timer a few times a second, asks the
/// GT911 over I2C whether anything happened, and goes back to sleep. RAM and
/// every peripheral's configuration survive, so returning from this is a
/// continuation, not a boot.
///
/// A timer and a poll rather than a wake-up pin, because this file knows of no
/// pin to use: the GT911's interrupt line is not among the connections it
/// drives, and a true deep sleep could only be woken through an RTC-capable
/// GPIO.
void board_sleep_until_touch();

/// @brief If not null, the pointed function is called every time a touch is detected.
extern void (*touch_callback)();

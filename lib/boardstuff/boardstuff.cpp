#include <boardstuff.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <lv_assert_port.h>

void lvgl_assert_failed(void)
{
    // Almost always a failed allocation (LV_USE_ASSERT_MALLOC). The heap numbers
    // are the useful part: printed now, because after the restart they are gone.
    Serial.printf("\n[lvgl] assert failed: free=%u largest=%u min=%u -- restarting\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)ESP.getMinFreeHeap());
    Serial.flush();
    esp_restart();
}

// Arduino-ESP32 3.x replaced the channel-based LEDC API with a pin-based one:
// channels are assigned automatically and ledcWrite() takes the pin. Both
// spellings are kept so this file survives a core downgrade.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#define PWM_ATTACH(pin, channel, freq, bits) ledcAttach((pin), (freq), (bits))
#define PWM_WRITE(pin, channel, duty) ledcWrite((pin), (duty))
#else
#define PWM_ATTACH(pin, channel, freq, bits)     \
    do                                           \
    {                                            \
        ledcSetup((channel), (freq), (bits));    \
        ledcAttachPin((pin), (channel));         \
    } while (0)
#define PWM_WRITE(pin, channel, duty) ledcWrite((channel), (duty))
#endif

static void lvgl_touch_init();

TwoWire i2c_gt911 = TwoWire(1);
void (*touch_callback)() = nullptr;

static TFT_eSPI tft;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *render_buf = nullptr;

void board_init()
{
    pinMode(ST7796_PIN_DC, OUTPUT); // Data or Command
    pinMode(ST7796_PIN_CS, OUTPUT); // Chip Select
    digitalWrite(ST7796_PIN_CS, HIGH);

    // Backlight starts dark on purpose: the panel holds whatever noise was in
    // its RAM at power-on, and lvgl_begin() raises the backlight only after it
    // has cleared the screen.
    pinMode(ST7796_PIN_BL, OUTPUT);
    PWM_ATTACH(ST7796_PIN_BL, ST7796_PWM_CHANNEL_BL, ST7796_PWM_FREQ_BL / 2, ST7796_PWM_BITS_BL);
    PWM_WRITE(ST7796_PIN_BL, ST7796_PWM_CHANNEL_BL, 0);

    // Setup RGB LED.  High is off
    PWM_ATTACH(LED_PIN_R, LED_PWM_CHANNEL_R, LED_PWM_FREQ, LED_PWM_BITS);
    PWM_ATTACH(LED_PIN_G, LED_PWM_CHANNEL_G, LED_PWM_FREQ, LED_PWM_BITS);
    PWM_ATTACH(LED_PIN_B, LED_PWM_CHANNEL_B, LED_PWM_FREQ, LED_PWM_BITS);
    set_led(0, 0, 0);

    // CDS light sensor. 11dB gives the full 0-3.3V range on the divider.
    //
    // Order matters, and getting it wrong is silent apart from one line at boot.
    // Arduino-ESP32 3.x attaches a pin to an ADC unit lazily, on the first
    // analogRead(); until then analogSetPinAttenuation() has no channel to
    // configure and fails with
    //
    //     [E][esp32-hal-adc.c:208] __analogChannelConfig():
    //         Pin is not configured as analog channel
    //
    // leaving the default 0dB attenuation in place -- so the sensor reads full
    // scale at about 1.1V instead of 3.3V and looks saturated in daylight. The
    // throwaway read below is what attaches it. Harmless on core 2.x, which
    // configures attenuation independently of any read.
    pinMode(CDS_PIN, INPUT);
    (void)analogRead(CDS_PIN);
    analogSetPinAttenuation(CDS_PIN, ADC_11db);

    // The GT911 samples its address straps while RST is released, so it has to
    // be pulsed before the first I2C transaction or the chip stays silent.
    pinMode(GT911_IIC_RST, OUTPUT);
    digitalWrite(GT911_IIC_RST, LOW);
    delay(10);
    digitalWrite(GT911_IIC_RST, HIGH);
    delay(50);

    i2c_gt911.begin(GT911_IIC_SDA, GT911_IIC_SCL);
    lvgl_touch_init();
}

struct __attribute__((packed)) GTPoint
{
    // 0x814F-0x8156, ... 0x8176 (5 points)
    uint8_t trackId;
    uint16_t x;
    uint16_t y;
    uint16_t area;
    uint8_t reserved;
};

void set_led(uint32_t hex_color)
{
    // Extract the red, green, and blue components from the hex color
    uint8_t red = (hex_color >> 16) & 0xFF;
    uint8_t green = (hex_color >> 8) & 0xFF;
    uint8_t blue = hex_color & 0xFF;

    set_led(red, green, blue);
}

void set_led(uint8_t red, uint8_t green, uint8_t blue)
{
    // The LED is common-anode: the PWM duty is inverted.
    PWM_WRITE(LED_PIN_R, LED_PWM_CHANNEL_R, LED_PWM_MAX - red);
    PWM_WRITE(LED_PIN_G, LED_PWM_CHANNEL_G, LED_PWM_MAX - green);
    PWM_WRITE(LED_PIN_B, LED_PWM_CHANNEL_B, LED_PWM_MAX - blue);
}

void set_backlight(uint8_t brightness)
{
    PWM_WRITE(ST7796_PIN_BL, ST7796_PWM_CHANNEL_BL, brightness);
}

uint16_t read_light_sensor()
{
    return analogRead(CDS_PIN);
}

static bool gt911_write_register(uint16_t reg, const uint8_t buf[], int len)
{
    i2c_gt911.beginTransmission(GT911_I2C_SLAVE_ADDR);
    if (!i2c_gt911.write(reg >> 8) || !i2c_gt911.write(reg & 0xFF))
        return false;

    auto sent = i2c_gt911.write(buf, len);
    i2c_gt911.endTransmission();
    return sent == len;
}

static bool gt911_read_register(uint16_t reg, uint8_t buf[], int len)
{
    i2c_gt911.beginTransmission(GT911_I2C_SLAVE_ADDR);
    if (!i2c_gt911.write(reg >> 8) || !i2c_gt911.write(reg & 0xFF))
        return false;

    i2c_gt911.endTransmission(false);
    auto requested = i2c_gt911.requestFrom(GT911_I2C_SLAVE_ADDR, len);
    if (requested != len)
        return false;

    while (i2c_gt911.available() && len--)
        *buf++ = i2c_gt911.read();

    return len == 0;
}

static int8_t gt911_num_points_available()
{
    uint8_t coord_addr;
    if (!gt911_read_register(GT911_REG_COORD_ADDR, &coord_addr, sizeof(coord_addr)))
    {
        log_e("Unable to read COORD_ADDR register");
        return 0;
    }

    if ((coord_addr & 0x80) && ((coord_addr & 0x0F) < GT911_MAX_CONTACTS))
    {
        uint8_t zero = 0;
        if (!gt911_write_register(GT911_REG_COORD_ADDR, &zero, sizeof(zero)))
        {
            log_e("Unable to reset COORD_ADDR register");
            return 0;
        }

        return coord_addr & 0x0F;
    }

    return 0;
}

static void lvgl_touch_init()
{
    uint8_t productId[GT911_PRODUCT_ID_LEN + 1] = {0};
    if (!gt911_read_register(GT911_PRODUCT_ID1, productId, GT911_PRODUCT_ID_LEN))
    {
        log_e("No GT911 touch device found");
        return;
    }

    log_i("Touch controller: %s", productId);
}

static bool gt911_read_touches(GTPoint *points, uint8_t numPoints = GT911_MAX_CONTACTS)
{
    if (!gt911_read_register(GT911_TRACK_ID1, (uint8_t *)points, sizeof(GTPoint) * numPoints))
    {
        log_e("Unable to read GTPoints");
        return false;
    }

    // The GT911 always reports in the panel's own portrait frame: x across
    // TFT_WIDTH, y down TFT_HEIGHT, origin at the corner TFT_eSPI calls
    // setRotation(0). Each case below rotates that into the frame the display
    // is currently in, so these transforms are paired with DISPLAY_ROTATION and
    // must be changed together.
    //
    // Note this is keyed on the rotation, not on the orientation macro: what
    // the touch has to match is the MADCTL the *display driver* programmed, and
    // that is a property of TFT_eSPI's rotation rather than of the board.
    for (uint8_t i = 0; i < numPoints; ++i)
    {
        // Clamp first: the controller occasionally reports a pixel or two
        // outside the panel at the very edges, and the mirrored subtractions
        // below would wrap that into a huge coordinate on these unsigned fields.
        const uint16_t tx = points[i].x < TFT_WIDTH ? points[i].x : (TFT_WIDTH - 1);
        const uint16_t ty = points[i].y < TFT_HEIGHT ? points[i].y : (TFT_HEIGHT - 1);

#if DISPLAY_ROTATION == 0
        points[i].x = tx;
        points[i].y = ty;
#elif DISPLAY_ROTATION == 1
        points[i].x = ty;
        points[i].y = (TFT_WIDTH - 1) - tx;
#elif DISPLAY_ROTATION == 2
        points[i].x = (TFT_WIDTH - 1) - tx;
        points[i].y = (TFT_HEIGHT - 1) - ty;
#elif DISPLAY_ROTATION == 3
        points[i].x = (TFT_HEIGHT - 1) - ty;
        points[i].y = tx;
#else
#error Unsupported DISPLAY_ROTATION
#endif
    }
    return true;
}

void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static int16_t last_x = 0, last_y = 0;
    // Ignore multi-touch
    auto points_available = gt911_num_points_available();
    if (points_available == 1)
    {
        GTPoint point;
        if (gt911_read_touches(&point, 1))
        {
            data->state = LV_INDEV_STATE_PR;
            last_x = data->point.x = point.x;
            last_y = data->point.y = point.y;
        }
        if (touch_callback)
        {
            (*touch_callback)();
        }
    }
    else
    {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
    }
}

/// LVGL hands us a rectangle of finished pixels; push it straight out over SPI.
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    const uint32_t w = (area->x2 - area->x1) + 1;
    const uint32_t h = (area->y2 - area->y1) + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // swap = true: LVGL keeps RGB565 in native (little-endian) order while the
    // panel expects big-endian, and TFT_eSPI does the byte swap in the DMA path.
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(drv);
}

void lvgl_begin()
{
    lv_init();

    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.fillScreen(TFT_BLACK);

    // Deliberately NOT MALLOC_CAP_DMA. TFT_eSPI is used here as a blocking SPI
    // writer (pushColors with no initDMA), so the buffer never needs to be
    // DMA-addressable -- and the DMA-capable pool is exactly what the WiFi
    // driver needs, so taking 10KB out of it would be pure loss.
    const size_t buffer_pixels = SCREEN_WIDTH * LVGL_BUFFER_LINES;
    render_buf = (lv_color_t *)heap_caps_malloc(buffer_pixels * sizeof(lv_color_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!render_buf)
    {
        // Half height rather than refusing to draw at all.
        const size_t fallback_pixels = SCREEN_WIDTH * (LVGL_BUFFER_LINES / 2);
        render_buf = (lv_color_t *)heap_caps_malloc(fallback_pixels * sizeof(lv_color_t),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        lv_disp_draw_buf_init(&draw_buf, render_buf, nullptr, fallback_pixels);
        log_e("LVGL render buffer shrunk to %d lines", LVGL_BUFFER_LINES / 2);
    }
    else
    {
        lv_disp_draw_buf_init(&draw_buf, render_buf, nullptr, buffer_pixels);
    }

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);
}

void lvgl_tick()
{
    lv_timer_handler();
}

#include <boardstuff.h>
#include <lv_assert_port.h>

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Native ESP-IDF throughout. The panel is driven with esp_lcd's SPI panel-IO
// layer and a small ST7796 command set of our own, rather than through a panel
// driver component: the init sequence and the rotation table below are the ones
// this exact board was brought up with, and keeping them means the port changes
// how bytes reach the controller, not which bytes.

static const char *TAG = "board";

void (*touch_callback)() = nullptr;

namespace
{
    esp_lcd_panel_io_handle_t panelIo = nullptr;
    i2c_master_bus_handle_t touchBus = nullptr;
    i2c_master_dev_handle_t touchDevice = nullptr;
    adc_oneshot_unit_handle_t adcUnit = nullptr;
    adc_cali_handle_t adcCalibration = nullptr;

    lv_disp_draw_buf_t drawBuffer;
    lv_disp_drv_t displayDriver;
    lv_color_t *renderBuffer = nullptr;
    lv_color_t *renderBuffer2 = nullptr;

    const int I2cTimeoutMs = 50;

    void delayMs(uint32_t ms)
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    uint32_t nowMs()
    {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

#pragma region "PWM"

    // One timer and one channel, for the backlight, in the low-speed group,
    // which every ESP32 variant has.
    const ledc_mode_t PwmMode = LEDC_LOW_SPEED_MODE;
    const ledc_channel_t BacklightChannel = LEDC_CHANNEL_0;

    void pwmTimer(ledc_timer_t timer, uint32_t hz)
    {
        ledc_timer_config_t config = {};
        config.speed_mode = PwmMode;
        config.duty_resolution = LEDC_TIMER_8_BIT;
        config.timer_num = timer;
        config.freq_hz = hz;
        config.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&config));
    }

    void pwmChannel(ledc_channel_t channel, ledc_timer_t timer, int gpio, uint32_t duty)
    {
        ledc_channel_config_t config = {};
        config.gpio_num = gpio;
        config.speed_mode = PwmMode;
        config.channel = channel;
        config.timer_sel = timer;
        config.duty = duty;
        ESP_ERROR_CHECK(ledc_channel_config(&config));
    }

    void pwmWrite(ledc_channel_t channel, uint32_t duty)
    {
        ledc_set_duty(PwmMode, channel, duty);
        ledc_update_duty(PwmMode, channel);
    }

#pragma endregion

#pragma region "ADC"

    // ADC1 only: ADC2 is unusable while the Wi-Fi driver is running.
    const adc_channel_t LightChannel = ADC_CHANNEL_6;   // GPIO 34
    const adc_channel_t BatteryChannel = ADC_CHANNEL_7; // GPIO 35

    void adcBegin()
    {
        adc_oneshot_unit_init_cfg_t unit = {};
        unit.unit_id = ADC_UNIT_1;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adcUnit));

        // 12dB (what the Arduino core called 11dB) gives the widest range the
        // ESP32 has, about 0.15-2.45V linear: enough for half a lithium cell.
        adc_oneshot_chan_cfg_t channel = {};
        channel.atten = ADC_ATTEN_DB_12;
        channel.bitwidth = ADC_BITWIDTH_12;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adcUnit, LightChannel, &channel));
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adcUnit, BatteryChannel, &channel));

        // The chip's factory calibration. Without it the raw count is off by
        // tens of millivolts and not linear near either rail.
        adc_cali_line_fitting_config_t calibration = {};
        calibration.unit_id = ADC_UNIT_1;
        calibration.atten = ADC_ATTEN_DB_12;
        calibration.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_line_fitting(&calibration, &adcCalibration) != ESP_OK)
        {
            adcCalibration = nullptr;
            ESP_LOGW(TAG, "no ADC calibration; voltages are approximate");
        }
    }

    int adcMillivolts(adc_channel_t channel)
    {
        int raw = 0;
        if (adc_oneshot_read(adcUnit, channel, &raw) != ESP_OK)
        {
            return 0;
        }
        int millivolts = 0;
        if (adcCalibration && adc_cali_raw_to_voltage(adcCalibration, raw, &millivolts) == ESP_OK)
        {
            return millivolts;
        }
        // Uncalibrated: a straight line over the nominal 12dB full scale.
        return (raw * 2450) / 4095;
    }

#pragma endregion

#pragma region "GT911"

    bool gt911Write(uint16_t reg, const uint8_t *data, size_t length)
    {
        uint8_t frame[2 + 8];
        if (length > sizeof(frame) - 2)
        {
            return false;
        }
        frame[0] = (uint8_t)(reg >> 8);
        frame[1] = (uint8_t)(reg & 0xFF);
        memcpy(frame + 2, data, length);
        return i2c_master_transmit(touchDevice, frame, 2 + length, I2cTimeoutMs) == ESP_OK;
    }

    bool gt911Read(uint16_t reg, uint8_t *data, size_t length)
    {
        const uint8_t address[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
        return i2c_master_transmit_receive(touchDevice, address, sizeof(address), data, length,
                                           I2cTimeoutMs) == ESP_OK;
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

    /// How many fingers the controller has a fresh report for. Reading a report
    /// clears it, which is what tells the GT911 to produce the next one.
    int8_t gt911PointsAvailable()
    {
        uint8_t status = 0;
        if (!gt911Read(GT911_REG_COORD_ADDR, &status, sizeof(status)))
        {
            return 0;
        }
        if ((status & 0x80) && ((status & 0x0F) < GT911_MAX_CONTACTS))
        {
            const uint8_t zero = 0;
            if (!gt911Write(GT911_REG_COORD_ADDR, &zero, sizeof(zero)))
            {
                return 0;
            }
            return status & 0x0F;
        }
        return 0;
    }

    /// What the GT911 has to say since it was last asked.
    ///   1  something touched the panel (press, move or release) -- now cleared
    ///   0  nothing
    ///  -1  the controller did not answer
    ///
    /// The status register latches: the controller sets its top bit when it has
    /// a report and leaves it set until the host writes 0. That is what makes
    /// polling it a few times a second good enough to wake on -- a tap far
    /// shorter than the poll interval is still sitting there on the next look.
    int gt911TakeActivity()
    {
        uint8_t status = 0;
        if (!gt911Read(GT911_REG_COORD_ADDR, &status, sizeof(status)))
        {
            return -1;
        }
        if (!(status & 0x80))
        {
            return 0;
        }
        const uint8_t zero = 0;
        gt911Write(GT911_REG_COORD_ADDR, &zero, sizeof(zero));
        return 1;
    }

    bool gt911ReadPoint(GTPoint &point)
    {
        if (!gt911Read(GT911_TRACK_ID1, (uint8_t *)&point, sizeof(point)))
        {
            return false;
        }

        // The GT911 always reports in the panel's own portrait frame: x across
        // TFT_WIDTH, y down TFT_HEIGHT, origin at the corner rotation 0 draws
        // from. Each case below rotates that into the frame the display is
        // currently in, so these transforms are paired with the MADCTL table
        // in displayBegin() and must be changed together.
        //
        // Clamp first: the controller occasionally reports a pixel or two
        // outside the panel at the very edges, and the mirrored subtractions
        // would wrap that into a huge coordinate on these unsigned fields.
        const uint16_t tx = point.x < TFT_WIDTH ? point.x : (TFT_WIDTH - 1);
        const uint16_t ty = point.y < TFT_HEIGHT ? point.y : (TFT_HEIGHT - 1);

#if DISPLAY_ROTATION == 0
        point.x = tx;
        point.y = ty;
#elif DISPLAY_ROTATION == 1
        point.x = ty;
        point.y = (TFT_WIDTH - 1) - tx;
#elif DISPLAY_ROTATION == 2
        point.x = (TFT_WIDTH - 1) - tx;
        point.y = (TFT_HEIGHT - 1) - ty;
#elif DISPLAY_ROTATION == 3
        point.x = (TFT_HEIGHT - 1) - ty;
        point.y = tx;
#else
#error Unsupported DISPLAY_ROTATION
#endif
        return true;
    }

    void touchBegin()
    {
        // The GT911 samples its address straps while RST is released, so it has
        // to be pulsed before the first I2C transaction or the chip stays silent.
        gpio_config_t reset = {};
        reset.pin_bit_mask = 1ULL << GT911_IIC_RST;
        reset.mode = GPIO_MODE_OUTPUT;
        gpio_config(&reset);
        gpio_set_level((gpio_num_t)GT911_IIC_RST, 0);
        delayMs(10);
        gpio_set_level((gpio_num_t)GT911_IIC_RST, 1);
        delayMs(50);

        i2c_master_bus_config_t bus = {};
        bus.i2c_port = I2C_NUM_0;
        bus.sda_io_num = (gpio_num_t)GT911_IIC_SDA;
        bus.scl_io_num = (gpio_num_t)GT911_IIC_SCL;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &touchBus));

        i2c_device_config_t device = {};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = GT911_I2C_SLAVE_ADDR;
        device.scl_speed_hz = 400000;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(touchBus, &device, &touchDevice));

        uint8_t productId[GT911_PRODUCT_ID_LEN + 1] = {0};
        if (!gt911Read(GT911_PRODUCT_ID1, productId, GT911_PRODUCT_ID_LEN))
        {
            ESP_LOGE(TAG, "no GT911 touch controller at 0x%02X", GT911_I2C_SLAVE_ADDR);
            return;
        }
        ESP_LOGI(TAG, "touch controller: %s", (const char *)productId);
    }

#pragma endregion

#pragma region "ST7796"

    // MIPI DCS, plus the ST7796's own extension commands in the init sequence.
    const uint8_t CmdSoftwareReset = 0x01;
    const uint8_t CmdSleepIn = 0x10;
    const uint8_t CmdSleepOut = 0x11;
    const uint8_t CmdDisplayOff = 0x28;
    const uint8_t CmdDisplayOn = 0x29;
    const uint8_t CmdColumnSet = 0x2A;
    const uint8_t CmdRowSet = 0x2B;
    const uint8_t CmdMemoryWrite = 0x2C;
    const uint8_t CmdMemoryAccess = 0x36;

    // Memory access control per rotation: mirror X, mirror Y, exchange axes,
    // and BGR colour order, which this panel is wired for.
    const uint8_t MadMy = 0x80, MadMx = 0x40, MadMv = 0x20, MadBgr = 0x08;
#if DISPLAY_ROTATION == 0
    const uint8_t MemoryAccess = MadMx | MadBgr;
#elif DISPLAY_ROTATION == 1
    const uint8_t MemoryAccess = MadMv | MadBgr;
#elif DISPLAY_ROTATION == 2
    const uint8_t MemoryAccess = MadMy | MadBgr;
#else
    const uint8_t MemoryAccess = MadMx | MadMy | MadMv | MadBgr;
#endif

    void command(uint8_t cmd)
    {
        esp_lcd_panel_io_tx_param(panelIo, cmd, nullptr, 0);
    }

    template <size_t N>
    void command(uint8_t cmd, const uint8_t (&data)[N])
    {
        esp_lcd_panel_io_tx_param(panelIo, cmd, data, N);
    }

    /// The SPI transfer of a flushed area has finished: tell LVGL it can have
    /// the buffer back. Runs in the SPI driver's interrupt, which is fine --
    /// lv_disp_flush_ready() only sets a flag.
    bool onFlushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *context)
    {
        lv_disp_flush_ready((lv_disp_drv_t *)context);
        return false;
    }

    void displayBegin()
    {
        spi_bus_config_t bus = {};
        bus.sclk_io_num = ST7796_SPI_SCLK;
        bus.mosi_io_num = ST7796_SPI_MOSI;
        bus.miso_io_num = ST7796_SPI_MISO;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = SCREEN_WIDTH * LVGL_BUFFER_LINES * (int)sizeof(lv_color_t);
        ESP_ERROR_CHECK(spi_bus_initialize(ST7796_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_spi_config_t io = {};
        io.cs_gpio_num = ST7796_PIN_CS;
        io.dc_gpio_num = ST7796_PIN_DC;
        io.spi_mode = 0;
        io.pclk_hz = ST7796_SPI_FREQ;
        io.trans_queue_depth = 4;
        io.lcd_cmd_bits = 8;
        io.lcd_param_bits = 8;
        io.on_color_trans_done = onFlushDone;
        io.user_ctx = &displayDriver;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ST7796_SPI_HOST, &io, &panelIo));

        // The panel's reset line is tied to the board reset -- there is no GPIO
        // for it -- so the controller is reset by command. The sequence and its
        // values are the ones TFT_eSPI's ST7796 driver uses (Bodmer, FreeBSD
        // licence), which is what this board ran on before the port.
        delayMs(120);
        command(CmdSoftwareReset);
        delayMs(120);
        command(CmdSleepOut);
        delayMs(120);

        command(0xF0, {0xC3}); // enable extension command set, part 1
        command(0xF0, {0x96}); // ... part 2
        command(CmdMemoryAccess, {MemoryAccess});
        command(0x3A, {0x55});             // 16 bits per pixel
        command(0xB4, {0x01});             // 1-dot inversion
        command(0xB6, {0x80, 0x02, 0x3B}); // display function: 480 lines
        command(0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}); // output timing
        command(0xC1, {0x06});             // power control 2
        command(0xC2, {0xA7});             // power control 3
        command(0xC5, {0x18});             // VCOM
        delayMs(120);
        command(0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B});
        command(0xE1, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B});
        delayMs(120);
        command(0xF0, {0x3C}); // disable extension command set, part 1
        command(0xF0, {0x69}); // ... part 2
        delayMs(120);
        command(CmdDisplayOn);
    }

    void setWindow(int x1, int y1, int x2, int y2)
    {
        const uint8_t columns[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
        const uint8_t rows[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
        command(CmdColumnSet, columns);
        command(CmdRowSet, rows);
    }

    /// LVGL hands over a rectangle of finished pixels; queue it on the SPI bus.
    /// onFlushDone() releases the buffer when the transfer completes.
    void flush(lv_disp_drv_t *, const lv_area_t *area, lv_color_t *pixels)
    {
        const size_t count = (size_t)(area->x2 - area->x1 + 1) * (size_t)(area->y2 - area->y1 + 1);

        // LVGL keeps RGB565 in native (little-endian) order -- LV_COLOR_16_SWAP
        // is 0, and the generated image assets are emitted for exactly that --
        // while the panel takes the high byte first. Swapped here, in place:
        // LVGL does not look at this buffer again until it is handed back.
        uint16_t *words = (uint16_t *)pixels;
        for (size_t i = 0; i < count; ++i)
        {
            words[i] = __builtin_bswap16(words[i]);
        }

        setWindow(area->x1, area->y1, area->x2, area->y2);
        esp_lcd_panel_io_tx_color(panelIo, CmdMemoryWrite, pixels, count * sizeof(lv_color_t));
    }

    void fillBlack()
    {
        // Through the render buffer, a strip at a time: there is no memory for
        // a full frame, and the panel powers up holding noise.
        const size_t stripPixels = SCREEN_WIDTH * LVGL_BUFFER_LINES;
        memset(renderBuffer, 0, stripPixels * sizeof(lv_color_t));
        for (int y = 0; y < SCREEN_HEIGHT; y += LVGL_BUFFER_LINES)
        {
            const int last = (y + LVGL_BUFFER_LINES > SCREEN_HEIGHT ? SCREEN_HEIGHT : y + LVGL_BUFFER_LINES) - 1;
            setWindow(0, y, SCREEN_WIDTH - 1, last);
            esp_lcd_panel_io_tx_color(panelIo, CmdMemoryWrite, renderBuffer,
                                      (size_t)SCREEN_WIDTH * (size_t)(last - y + 1) * sizeof(lv_color_t));
        }
        // A parameter write waits for the queued colour transfers to drain, so
        // this returns only once the screen is really black.
        command(0x00); // NOP
    }

#pragma endregion

    void touchRead(lv_indev_drv_t *, lv_indev_data_t *data)
    {
        static int16_t lastX = 0, lastY = 0;
        // Single touch only.
        if (gt911PointsAvailable() == 1)
        {
            GTPoint point;
            if (gt911ReadPoint(point))
            {
                data->state = LV_INDEV_STATE_PR;
                lastX = data->point.x = point.x;
                lastY = data->point.y = point.y;
            }
            if (touch_callback)
            {
                (*touch_callback)();
            }
        }
        else
        {
            data->point.x = lastX;
            data->point.y = lastY;
            data->state = LV_INDEV_STATE_REL;
        }
    }
}

void lvgl_assert_failed(void)
{
    // Almost always a failed allocation (LV_USE_ASSERT_MALLOC). The heap numbers
    // are the useful part: printed now, because after the restart they are gone.
    printf("\n[lvgl] assert failed: free=%u largest=%u min=%u -- restarting\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    fflush(stdout);
    esp_restart();
}

void board_init()
{
    // Backlight starts dark on purpose: the panel holds whatever noise was in
    // its RAM at power-on, and lvgl_begin() clears the screen before anything
    // raises the backlight.
    pwmTimer(LEDC_TIMER_0, ST7796_PWM_FREQ_BL);
    pwmChannel(BacklightChannel, LEDC_TIMER_0, ST7796_PIN_BL, 0);

    // The RGB LED is permanently off, to spare the battery. It is common-anode,
    // so off is the pin held HIGH -- as a plain output, not as PWM: PWM at full
    // duty is 255/256 at 8 bits, which leaves the pin low for one cycle in 256
    // and the LED visibly glowing a dim white. A GPIO output keeps its level
    // through light sleep, so the sleep stage needs nothing extra.
    gpio_config_t led = {};
    led.pin_bit_mask = (1ULL << LED_PIN_R) | (1ULL << LED_PIN_G) | (1ULL << LED_PIN_B);
    led.mode = GPIO_MODE_OUTPUT;
    gpio_config(&led);
    gpio_set_level((gpio_num_t)LED_PIN_R, 1);
    gpio_set_level((gpio_num_t)LED_PIN_G, 1);
    gpio_set_level((gpio_num_t)LED_PIN_B, 1);

    adcBegin();
    touchBegin();
}

void set_backlight(uint8_t brightness)
{
    pwmWrite(BacklightChannel, brightness);
}

uint16_t read_light_sensor()
{
    int raw = 0;
    adc_oneshot_read(adcUnit, LightChannel, &raw);
    return (uint16_t)raw;
}

uint16_t read_battery_mv()
{
    // Averaged because the divider is high impedance and single samples scatter.
    const int Samples = 16;
    uint32_t sum = 0;
    for (int i = 0; i < Samples; ++i)
    {
        sum += (uint32_t)adcMillivolts(BatteryChannel);
    }
    return (uint16_t)(((sum / Samples) * BAT_DIVIDER_MILLI) / 1000);
}

void display_sleep(bool asleep)
{
    // Display off then sleep in, and the reverse on the way back. Sleep-out
    // needs 120ms before the next command.
    if (asleep)
    {
        command(CmdDisplayOff);
        command(CmdSleepIn);
        delayMs(5);
    }
    else
    {
        command(CmdSleepOut);
        delayMs(120);
        command(CmdDisplayOn);
    }
}

void board_sleep_until_touch()
{
    // How often the processor surfaces to look at the touch controller. The
    // look itself is one I2C read, a millisecond or so, so this sets how long a
    // touch can go unanswered far more than it sets the current drawn.
    const uint64_t PollIntervalUs = 250 * 1000ULL;
    // A controller that stops answering must not be able to hold the panel
    // asleep for good with no way back but the reset button.
    const int MaxSilentPolls = 8;

    // Whatever asked for this may still have a finger on the glass ("Sleep
    // now" is a button). Wait for the panel to go quiet and swallow the report
    // of the release, or that report is the first thing the loop below sees.
    const uint32_t startedMs = nowMs();
    uint32_t quietSinceMs = startedMs;
    while ((nowMs() - quietSinceMs) < 150 && (nowMs() - startedMs) < 3000)
    {
        if (gt911TakeActivity() > 0)
        {
            quietSinceMs = nowMs();
        }
        delayMs(10);
    }

    fflush(stdout);
    // Give the UART time to drain: light sleep stops its clock mid-character.
    delayMs(20);

    // The LEDC timer driving the backlight stops with the clocks. Latch the pin
    // at the level it has now -- the caller set it dark -- rather than trust
    // what a stopped PWM generator leaves on it.
    gpio_hold_en((gpio_num_t)ST7796_PIN_BL);

    int silentPolls = 0;
    for (;;)
    {
        esp_sleep_enable_timer_wakeup(PollIntervalUs);
        esp_light_sleep_start();

        // One tick for everything else: this loop never blocks otherwise, and
        // an idle task that is never scheduled is what the task watchdog
        // exists to complain about.
        vTaskDelay(1);

        const int activity = gt911TakeActivity();
        if (activity > 0)
        {
            break;
        }
        silentPolls = activity < 0 ? silentPolls + 1 : 0;
        if (silentPolls >= MaxSilentPolls)
        {
            ESP_LOGE(TAG, "touch controller stopped answering; waking");
            break;
        }
    }

    gpio_hold_dis((gpio_num_t)ST7796_PIN_BL);
}

void lvgl_begin()
{
    lv_init();

    // DMA-capable, because the SPI driver sends straight from it. On this chip
    // that costs nothing: all of its internal data RAM is DMA-capable.
    const size_t bufferPixels = SCREEN_WIDTH * LVGL_BUFFER_LINES;
    renderBuffer = (lv_color_t *)heap_caps_malloc(bufferPixels * sizeof(lv_color_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(renderBuffer ? ESP_OK : ESP_ERR_NO_MEM);
    // Two strips, so LVGL draws the next one while the SPI DMA is still sending
    // the last. With one, every strip is drawn and then sent, strictly in turn,
    // and a full-screen redraw (any scroll) costs the sum of the two. A failed
    // second allocation is not fatal: it just goes back to one.
    renderBuffer2 = (lv_color_t *)heap_caps_malloc(bufferPixels * sizeof(lv_color_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    // The driver and its draw buffer are set up before the SPI panel-IO exists:
    // every colour transfer ends in onFlushDone(), which hands this driver to
    // lv_disp_flush_ready() -- including the transfers that clear the screen.
    lv_disp_drv_init(&displayDriver);
    lv_disp_draw_buf_init(&drawBuffer, renderBuffer, renderBuffer2, bufferPixels);
    displayDriver.hor_res = SCREEN_WIDTH;
    displayDriver.ver_res = SCREEN_HEIGHT;
    displayDriver.flush_cb = flush;
    displayDriver.draw_buf = &drawBuffer;

    displayBegin();
    fillBlack();

    lv_disp_drv_register(&displayDriver);

    static lv_indev_drv_t inputDriver;
    lv_indev_drv_init(&inputDriver);
    inputDriver.type = LV_INDEV_TYPE_POINTER;
    inputDriver.read_cb = touchRead;
    // A swipe is counted in pixels moved per read, and a read comes every
    // LV_INDEV_DEF_READ_PERIOD ms: at 15 ms a lazy swipe moves 2 px a read,
    // under the default minimum of 3, and was thrown away as not moving.
    inputDriver.gesture_min_velocity = 1;
    inputDriver.gesture_limit = 40;
    lv_indev_drv_register(&inputDriver);
}

void lvgl_tick()
{
    lv_timer_handler();
}

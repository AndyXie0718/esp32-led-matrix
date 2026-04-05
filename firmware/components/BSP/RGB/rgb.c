#include "rgb.h"

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/portmacro.h"
#include "led_strip.h"
#include "panel_config.h"

#define RGB_GPIO_PIN 20
#define RGB_LED_COUNT PANEL_LED_COUNT
#define RGB_SPI_HOST SPI2_HOST

static led_strip_handle_t s_strip = NULL;
static uint8_t s_global_r8 = 255;
static uint8_t s_global_g8 = 255;
static uint8_t s_global_b8 = 255;
static portMUX_TYPE s_color_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void apply_global_color(uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t gr;
    uint8_t gg;
    uint8_t gb;

    portENTER_CRITICAL(&s_color_mux);
    gr = s_global_r8;
    gg = s_global_g8;
    gb = s_global_b8;
    portEXIT_CRITICAL(&s_color_mux);

    *r = (uint8_t)(((uint16_t)(*r) * gr) / 255u);
    *g = (uint8_t)(((uint16_t)(*g) * gg) / 255u);
    *b = (uint8_t)(((uint16_t)(*b) * gb) / 255u);
}

static inline void limit_panel_max(uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t m = *r;
    if (*g > m) {
        m = *g;
    }
    if (*b > m) {
        m = *b;
    }

    if (m > PANEL_LED_VALUE_MAX) {
        *r = (uint8_t)(((uint16_t)(*r) * PANEL_LED_VALUE_MAX) / m);
        *g = (uint8_t)(((uint16_t)(*g) * PANEL_LED_VALUE_MAX) / m);
        *b = (uint8_t)(((uint16_t)(*b) * PANEL_LED_VALUE_MAX) / m);
    }
}

static inline void apply_global_and_panel_limit(uint8_t* r, uint8_t* g, uint8_t* b) {
    apply_global_color(r, g, b);
    limit_panel_max(r, g, b);
}

void rgb_init(void) {
    if (s_strip) {
        return;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO_PIN,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }};

    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = RGB_SPI_HOST,
        .flags = {
            .with_dma = true,
        },
    };

    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &s_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_strip));
    ESP_ERROR_CHECK(led_strip_refresh(s_strip));
}

void rgb_deinit(void) {
    if (!s_strip) {
        return;
    }

    led_strip_del(s_strip);
    s_strip = NULL;
}

void rgb_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip || index >= RGB_LED_COUNT) {
        return;
    }

    apply_global_and_panel_limit(&r, &g, &b);

    led_strip_set_pixel(s_strip, index, r, g, b);
}

void rgb_set_fast(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip || index >= RGB_LED_COUNT) {
        return;
    }
    apply_global_and_panel_limit(&r, &g, &b);
    led_strip_set_pixel(s_strip, index, r, g, b);
}

void rgb_clear(void) {
    if (!s_strip) {
        return;
    }
    led_strip_clear(s_strip);
}

void rgb_set_hsv(uint32_t index, uint16_t hue, uint16_t light) {
    if (!s_strip || index >= RGB_LED_COUNT) {
        return;
    }

    hue %= 360;
    light = (light > PANEL_LED_VALUE_MAX) ? PANEL_LED_VALUE_MAX : light;
    led_strip_set_pixel_hsv(s_strip, index, hue, 255, light);
}

void rgb_set_global_color8(uint8_t r8, uint8_t g8, uint8_t b8) {
    portENTER_CRITICAL(&s_color_mux);
    s_global_r8 = r8;
    s_global_g8 = g8;
    s_global_b8 = b8;
    portEXIT_CRITICAL(&s_color_mux);
}

void rgb_get_global_color8(uint8_t* r8, uint8_t* g8, uint8_t* b8) {
    if (!r8 || !g8 || !b8) {
        return;
    }
    portENTER_CRITICAL(&s_color_mux);
    *r8 = s_global_r8;
    *g8 = s_global_g8;
    *b8 = s_global_b8;
    portEXIT_CRITICAL(&s_color_mux);
}

void rgb_show(void) {
    if (!s_strip) {
        return;
    }

    esp_err_t err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        led_strip_refresh(s_strip);
    }
    esp_rom_delay_us(80);
}

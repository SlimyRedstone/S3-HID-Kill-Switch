/*
 * Register-level driver for the CH32V003 expander. Register map taken from
 * waveshare/custom_io_expander_ch32v003, minus the esp_io_expander layer.
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"

#include "ch32v003_exio.h"

static const char *TAG = "exio";

#define REG_DIRECTION   0x02
#define REG_OUTPUT      0x03
#define REG_INPUT       0x04

#define I2C_TIMEOUT_MS  100
#define DEFAULT_SCL_HZ  400000

struct ch32v003_exio {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t       lock;   /* guards the output shadow */
    uint8_t                 output; /* the chip cannot read its output back */
};

static esp_err_t write_reg(ch32v003_exio_handle_t h, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(h->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t ch32v003_exio_new(i2c_master_bus_handle_t bus, uint32_t scl_hz,
                            uint8_t dir, uint8_t levels,
                            ch32v003_exio_handle_t *out)
{
    ESP_RETURN_ON_FALSE(bus && out, ESP_ERR_INVALID_ARG, TAG, "bad args");

    ch32v003_exio_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    esp_err_t err = ESP_ERR_NO_MEM;
    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) {
        goto fail;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CH32V003_EXIO_ADDR,
        .scl_speed_hz    = scl_hz ? scl_hz : DEFAULT_SCL_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        goto fail;
    }

    /* Output first: a pin turning into an output starts at its final level. */
    err = write_reg(h, REG_OUTPUT, levels);
    if (err == ESP_OK) {
        err = write_reg(h, REG_DIRECTION, dir);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no answer at 0x%02X (%s)", CH32V003_EXIO_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(h->dev);
        goto fail;
    }

    h->output = levels;
    *out = h;
    ESP_LOGI(TAG, "ready: dir=0x%02X out=0x%02X", dir, levels);
    return ESP_OK;

fail:
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
    return err;
}

esp_err_t ch32v003_exio_set_level(ch32v003_exio_handle_t h, uint8_t pin, bool level)
{
    ESP_RETURN_ON_FALSE(h && pin < 8, ESP_ERR_INVALID_ARG, TAG, "bad args");

    xSemaphoreTake(h->lock, portMAX_DELAY);
    uint8_t next = level ? (h->output | (1u << pin)) : (h->output & ~(1u << pin));
    esp_err_t err = ESP_OK;
    if (next != h->output) {
        err = write_reg(h, REG_OUTPUT, next);
        if (err == ESP_OK) {
            h->output = next;
        }
    }
    xSemaphoreGive(h->lock);
    return err;
}

esp_err_t ch32v003_exio_get_inputs(ch32v003_exio_handle_t h, uint8_t *levels)
{
    ESP_RETURN_ON_FALSE(h && levels, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t reg = REG_INPUT;
    return i2c_master_transmit_receive(h->dev, &reg, 1, levels, 1, I2C_TIMEOUT_MS);
}

void ch32v003_exio_del(ch32v003_exio_handle_t h)
{
    if (h == NULL) {
        return;
    }
    i2c_master_bus_rm_device(h->dev);
    vSemaphoreDelete(h->lock);
    free(h);
}

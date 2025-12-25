#pragma once

#include <stddef.h>

#include "board_pins.h"
#include "hardware/gpio.h"
#include "sd_driver/SPI/my_spi.h"
#include "sd_driver/sd_card.h"

#ifndef SD_SPI_BAUD_RATE
#define SD_SPI_BAUD_RATE (12 * 1000 * 1000)
#endif

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static spi_t system_spi = {
    .hw_inst = SPI_INSTANCE(SPI_INSTANCE_NUM),
    .miso_gpio = SPI_MISO_PIN,
    .mosi_gpio = SPI_MOSI_PIN,
    .sck_gpio = SPI_SCK_PIN,
    .baud_rate = SD_SPI_BAUD_RATE,
    .spi_mode = 0,
    .no_miso_gpio_pull_up = false,
    .set_drive_strength = true,
    .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_8MA,
    .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_8MA,
    .use_static_dma_channels = false,
    .tx_dma = 0,
    .rx_dma = 0,
};

static sd_spi_if_t sd_spi_if = {
    .spi = &system_spi,
    .ss_gpio = SPI_CS_PIN,
    .set_drive_strength = true,
    .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_8MA,
    .state = {0},
};

static sd_card_t sd_cards[] = {
    {
        .type = SD_IF_SPI,
        .spi_if_p = &sd_spi_if,
        .use_card_detect = false,
        .card_detect_gpio = 0,
        .card_detected_true = 0,
        .card_detect_use_pull = false,
        .card_detect_pull_hi = false,
    },
};

size_t sd_get_num(void) {
    return COUNT_OF(sd_cards);
}

sd_card_t *sd_get_by_num(size_t num) {
    return (num < COUNT_OF(sd_cards)) ? &sd_cards[num] : NULL;
}

size_t spi_get_num(void) {
    return 1;
}

spi_t *spi_get_by_num(size_t num) {
    return (num == 0) ? &system_spi : NULL;
}

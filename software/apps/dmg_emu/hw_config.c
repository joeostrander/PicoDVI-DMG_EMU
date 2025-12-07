#include "hw_config.h"

#include <stddef.h>

#include "board_pins.h"
#include "diskio.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "spi.h"

#ifndef SD_SPI_BAUD_RATE
#define SD_SPI_BAUD_RATE (12 * 1000 * 1000)
#endif

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static spi_t system_spis[];

static void __not_in_flash_func(spi_dma_isr_0)(void)
{
    spi_irq_handler(&system_spis[0]);
}

static spi_t system_spis[] = {
    {
        .hw_inst = SPI_INSTANCE,
        .miso_gpio = SPI_MISO_PIN,
        .mosi_gpio = SPI_MOSI_PIN,
        .sck_gpio = SPI_SCK_PIN,
        .baud_rate = SD_SPI_BAUD_RATE,
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_8MA,
        .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_8MA,
        .dma_isr = spi_dma_isr_0,
    },
};

static sd_card_t sd_cards[] = {
    {
        .pcName = "0:",
        .spi = &system_spis[0],
        .ss_gpio = SPI_CS_PIN,
        .use_card_detect = false,
        .card_detect_gpio = 0,
        .card_detected_true = 0,
        .set_drive_strength = true,
        .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_8MA,
        .m_Status = STA_NOINIT | STA_NODISK,
    },
};

size_t spi_get_num(void) {
    return COUNT_OF(system_spis);
}

spi_t *spi_get_by_num(size_t num) {
    if (num >= spi_get_num()) {
        return NULL;
    }
    return &system_spis[num];
}

size_t sd_get_num(void) {
    return COUNT_OF(sd_cards);
}

sd_card_t *sd_get_by_num(size_t num) {
    if (num >= sd_get_num()) {
        return NULL;
    }
    return &sd_cards[num];
}

#pragma once

// Shared pin definitions for the Game Boy SD SPI bus
#define SPI_INSTANCE_NUM  1
#define SPI_MISO_PIN      8
#define SPI_CS_PIN        9
#define SPI_SCK_PIN       10
#define SPI_MOSI_PIN      11

// Default SD card SPI transfer rate after initialization (Hz)
#define SD_SPI_BAUD_RATE (16 * 1000 * 1000)

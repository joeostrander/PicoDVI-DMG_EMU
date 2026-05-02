#pragma once

// Controller mode selection.
// Exactly one transport should be enabled at a time.
#ifndef USE_BLUETOOTH_CONTROLLER
#define USE_BLUETOOTH_CONTROLLER       1
#endif

#define USE_METRO_RP2350 0  // Set to 1 to use Adafruit Metro RP2350 board pinout, 0 for custom board

#if USE_METRO_RP2350
// Adafruit Metro RP2350 board

// Set to 1 to enable NES Classic I2C-based controller support
// Set to 0 to use old-school shift register controller support
#ifndef USE_NES_CLASSIC_CONTROLLER
#define USE_NES_CLASSIC_CONTROLLER      1
#endif

// SPI Pins, etc. -- for SD card
#define SPI_INSTANCE_NUM            0
#define PIN_SPI_MISO                36
#define PIN_SPI_CS                  39
#define PIN_SPI_SCK                 34
#define PIN_SPI_MOSI                35

// I2C Pins, etc. -- for I2C controller
#define PIN_SDA                     20
#define PIN_SCL                     21
#define MY_I2C_INSTANCE             i2c0

// LED pin for debugging
#define PIN_LED                     23

#else
// Rev 1 of custom board (I may switch pinout to match Metro RP2350 later)

// Set to 1 to enable NES Classic I2C-based controller support
// Set to 0 to use old-school shift register controller support
#ifndef USE_NES_CLASSIC_CONTROLLER
#define USE_NES_CLASSIC_CONTROLLER      0
#endif

// SPI Pins, etc. -- for SD card
#define SPI_INSTANCE_NUM            1
#define PIN_SPI_MISO                8
#define PIN_SPI_CS                  9
#define PIN_SPI_SCK                 10
#define PIN_SPI_MOSI                11

// I2C Pins, etc. -- for I2C controller
#define PIN_SDA                     26
#define PIN_SCL                     27
//NEW??? #define PIN_SDA                     2
//NEW??? #define PIN_SCL                     3
#define MY_I2C_INSTANCE             i2c1

// LED pin for debugging
#define PIN_LED                     25

#endif

// Shift register pins -- for old-school NES controller
#define PIN_NES_DATA                7
#define PIN_NES_LATCH               6
#define PIN_NES_PULSE               5

// Default SD card SPI transfer rate after initialization (Hz)
#define SD_SPI_BAUD_RATE (16 * 1000 * 1000)

// I2C address for NES Classic controller
#define I2C_ADDRESS                 0x52

#if USE_BLUETOOTH_CONTROLLER && USE_NES_CLASSIC_CONTROLLER
#error "USE_BLUETOOTH_CONTROLLER and USE_NES_CLASSIC_CONTROLLER cannot both be enabled"
#endif


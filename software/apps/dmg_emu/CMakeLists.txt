# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(dmg_emu
    main.c
    colors.c
    audio/minigb_apu.c
    eeprom.c
    osd.c
    font_5x7.c
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(dmg_emu PRIVATE -Wall)

# Set resolution mode (choose 0/1/2)
# 0 = 640x480, horizontally scaled x4, vertically x3 --> 640x480 Full screen
# 1 = 800x600, horizontally scaled x4, vertically x4 --> 640x576 window
# 2 = 640x480, horizontally scaled x2, vertically x2 --> 320x288 window
set(RESOLUTION_MODE "0" CACHE STRING "Resolution mode: 0=640x480 x4/x3, 1=800x600 x4/x4, 2=640x480 x2/x2")
set_property(CACHE RESOLUTION_MODE PROPERTY STRINGS 0 1 2)

if(RESOLUTION_MODE STREQUAL "1")
    set(DVI_VERTICAL_REPEAT_VALUE 4)
elseif(RESOLUTION_MODE STREQUAL "2")
    set(DVI_VERTICAL_REPEAT_VALUE 2)
else()
    set(DVI_VERTICAL_REPEAT_VALUE 3)
endif()

target_compile_definitions(dmg_emu PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=${DVI_VERTICAL_REPEAT_VALUE}
    DVI_SYMBOLS_PER_WORD=2
    RESOLUTION_MODE=${RESOLUTION_MODE}
    PICO_USE_MALLOC_PANIC=0
    PICO_FLASH_SIZE_BYTES=0x200000  # (2097152, 2MB) - I need to define this or it may set to 4MB by default
)

target_link_libraries(dmg_emu
    pico_stdlib
    pico_stdio_uart
    pico_multicore
    pico_util
    libdvi
    libsprite
    hardware_pio
    hardware_i2c
    hardware_uart
    hardware_irq
    hardware_dma
    hardware_flash
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

if(TARGET hardware_rtc)
    target_link_libraries(dmg_emu hardware_rtc)
elseif(TARGET pico_aon_timer)
    target_link_libraries(dmg_emu pico_aon_timer)
endif()

# Ensure FatFs SPI headers are visible to this app
target_include_directories(dmg_emu PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/FatFs_SPI
    ${CMAKE_SOURCE_DIR}/ext/FatFs_SPI/include
    ${CMAKE_SOURCE_DIR}/ext/FatFs_SPI/ff15/source
    ${CMAKE_SOURCE_DIR}/ext/FatFs_SPI/sd_driver
)

# # Enable USB serial output for printf debugging
# pico_enable_stdio_usb(dmg_emu 1)
# pico_enable_stdio_uart(dmg_emu 0)

# Enable UART stdio and disable USB stdio
pico_enable_stdio_uart(dmg_emu 1)
pico_enable_stdio_usb(dmg_emu 0)
# Force printf to use UART0 instead of USB
# I'm setting these in main instead, before pico/stdlib.h is included
# set(PICO_DEFAULT_UART 0)       # Use UART0
# set(PICO_DEFAULT_UART_TX_PIN 0) # GPIO0 for TX
# set(PICO_DEFAULT_UART_RX_PIN 1) # GPIO1 for RX
# set(PICO_STDIO_DEFAULT_CRLF 1)  # Optional: convert \n to \r\n

target_include_directories(dmg_emu PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# create map/bin/hex file etc.
pico_add_extra_outputs(dmg_emu)

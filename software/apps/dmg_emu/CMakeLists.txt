# Replace TMDS with 10 bit UART (same baud rate):
# add_definitions(-DDVI_SERIAL_DEBUG=1)
# add_definitions(-DRUN_FROM_CRYSTAL)

add_executable(dmg_emu
    main.c
    shared_dma_handler.c
    colors.c
    audio/minigb_apu.c
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(dmg_emu PRIVATE -Wall)

# Set resolution mode (change this to switch between modes)
set(RESOLUTION_800x600 1)  # Set to 1 for 800x600, 0 for 640x480

# Automatically set DVI_VERTICAL_REPEAT based on resolution
if(RESOLUTION_800x600)
    set(DVI_VERTICAL_REPEAT_VALUE 4)
else()
    set(DVI_VERTICAL_REPEAT_VALUE 3)
endif()

target_compile_definitions(dmg_emu PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=${DVI_DEFAULT_SERIAL_CONFIG}
    DVI_VERTICAL_REPEAT=${DVI_VERTICAL_REPEAT_VALUE}
    DVI_SYMBOLS_PER_WORD=2
    RESOLUTION_800x600=${RESOLUTION_800x600}
)

target_link_libraries(dmg_emu
    pico_stdlib
    pico_multicore
    pico_util
    libdvi
    libsprite
    hardware_pio
    hardware_i2c
    hardware_irq
    hardware_dma
)

# Enable USB serial output for printf debugging
pico_enable_stdio_usb(dmg_emu 1)
pico_enable_stdio_uart(dmg_emu 0)

# Build pio programs
# pico_generate_pio_header(dmg_emu ${CMAKE_CURRENT_LIST_DIR}/video_capture.pio)
# pico_generate_pio_header(dmg_emu ${CMAKE_CURRENT_LIST_DIR}/dmg_emu_buttons.pio)


# create map/bin/hex file etc.
pico_add_extra_outputs(dmg_emu)

/*

TODO:

    Get HDMI (DVI) working on real TV - works in 640x480
    Possibly pull in some of the OSD features from PicoDVI-N64
    cleanup unused headers
    Add my version of OSD back in

*/

// Joe Ostrander
// 2025.12.06
// PicoDVI-DMG_EMU
//

// REMINDER: Always use cmake with:  -DPICO_COPY_TO_RAM=1

// #pragma GCC optimize("Os")
// #pragma GCC optimize("O2")
#pragma GCC optimize("O3")


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "hardware/vreg.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "audio_ring.h"  // For get_write_size and get_read_size

#include "mario.h"

#include "colors.h"
#include "hedley.h"

#define SAMPLE_FREQ                 32768
#define AUDIO_BUFFER_SIZE           2048
#define AUDIO_SAMPLE_RATE           SAMPLE_FREQ
#define ENABLE_SOUND                1
#define DMG_FRAME_DURATION_US       ((uint32_t)(1000000.0 / VERTICAL_SYNC + 0.5))
#define FRAME_CATCHUP_THRESHOLD_US  (DMG_FRAME_DURATION_US * 64u)
#define NES_CONTROLLER_INIT_DELAY_MS   2000u
#define NES_CONTROLLER_REINIT_DELAY_MS 1000u

#include "audio/minigb_apu.h"
#include "peanut_gb.h"
#include "roms/tetris_rom.h"
// #include "roms/super_mario_land_rom.h"

#define DMG_CLOCK_FREQ_INT        ((uint32_t)DMG_CLOCK_FREQ)
#define SCREEN_REFRESH_CYCLES_INT ((uint32_t)SCREEN_REFRESH_CYCLES)
#define MAX_AUDIO_SAMPLES_PER_FRAME ((uint32_t)(((uint64_t)AUDIO_SAMPLE_RATE * SCREEN_REFRESH_CYCLES_INT + DMG_CLOCK_FREQ_INT - 1) / DMG_CLOCK_FREQ_INT))


#define ENABLE_AUDIO                1  // Enable Peanut-GB audio path
#define ENABLE_PIO_DMG_BUTTONS      0
#define ENABLE_CPU_DMG_BUTTONS      0
#define ENABLE_OSD                  0  // Set to 1 to enable OSD code, 0 to disable (TODO)


#if ENABLE_AUDIO
static const int hdmi_n[6] = {4096, 6272, 6144, 3136, 4096, 6144};  // 32k, 44.1k, 48k, 22.05k, 16k, 24k
static uint16_t rate = SAMPLE_FREQ;
static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static audio_sample_t apu_frame_buffer[MAX_AUDIO_SAMPLES_PER_FRAME];
static uint64_t audio_sample_residual = 0;
static size_t audio_samples_for_frame(void);
static void pump_audio_samples(void);
static void write_samples_to_ring(const audio_sample_t *samples, size_t sample_count);
#endif

#define DEBUG_BUTTON_PRESS   // Illuminate LED on button presses

#define ONBOARD_LED_PIN             25

// GAMEBOY VIDEO INPUT (From level shifter)

#define HSYNC_PIN                   0
#define DATA_1_PIN                  1
#define DATA_0_PIN                  2
#define PIXEL_CLOCK_PIN             3
#define VSYNC_PIN                   4
#define DMG_READING_BUTTONS_PIN     5       // P15
#define DMG_READING_DPAD_PIN        6       // P14
#define DMG_OUTPUT_RIGHT_A_PIN      7       // P10
#define DMG_OUTPUT_UP_SELECT_PIN    8       // P12
#define DMG_OUTPUT_DOWN_START_PIN   9       // P13
#define DMG_OUTPUT_LEFT_B_PIN       10      // P11

// the bit order coincides with the pin order
#define BIT_RIGHT_A                 (1<<0)  // P10
#define BIT_UP_SELECT               (1<<1)  // P12
#define BIT_DOWN_START              (1<<2)  // P13
#define BIT_LEFT_B                  (1<<3)  // P11

// I2C Pins, etc. -- for I2C controller
#define SDA_PIN                     26
#define SCL_PIN                     27
#define I2C_ADDRESS                 0x52
i2c_inst_t* i2cHandle = i2c1;

// at 4x Game area will be 640x576 
#define DMG_PIXELS_X                160
#define DMG_PIXELS_Y                144
#define DMG_PIXEL_COUNT             (DMG_PIXELS_X*DMG_PIXELS_Y)

// Packed DMA buffers - 4 pixels per byte (2 bits each)
// This is the native format from the Game Boy (2 bits per pixel)
// Used by BOTH 640x480 and 800x600 modes for DMA capture AND display
#define PACKED_FRAME_SIZE (DMG_PIXEL_COUNT / 4)  // 5760 bytes (160×144 ÷ 4)
#define PACKED_LINE_STRIDE_BYTES (DMG_PIXELS_X / 4)
static uint8_t packed_buffer_0[PACKED_FRAME_SIZE] = {0};
static uint8_t packed_buffer_1[PACKED_FRAME_SIZE] = {0};

// Both modes now use packed buffers directly!
// TMDS encoder handles palette conversion and horizontal scaling
// - 640x480: 4× scaling with grayscale/color palette
// - 800x600: 5× scaling with RGB888 palette
static volatile uint8_t* packed_display_ptr = packed_buffer_0;
static uint8_t* packed_render_ptr = packed_buffer_1;

static struct gb_s gb;
static uint8_t rom_bank0[0x4000];
static uint8_t cart_ram[0x8000];

static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);
static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
static void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val);
static void update_emulator_inputs(void);
static void swap_display_buffers(void);
static void handle_palette_hotkeys(void);


typedef enum
{
    BUTTON_A = 0,
    BUTTON_B,
    BUTTON_SELECT,
    BUTTON_START,
    BUTTON_UP,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_HOME,
    BUTTON_COUNT
} controller_button_t;

typedef enum
{
    BUTTON_STATE_PRESSED = 0,
    BUTTON_STATE_UNPRESSED
} button_state_t;

#if ENABLE_PIO_DMG_BUTTONS
static PIO pio_dmg_emu_buttons = pio0;
// TODO static uint dmg_emu_buttons_sm = 3;  // Use SM3 (SM0, SM1, SM2 used by DVI TMDS channels)
static uint pio_buttons_out_value = 0;
#endif

static uint8_t button_states[BUTTON_COUNT];

#if RESOLUTION_800x600
#define FRAME_WIDTH 800
#define FRAME_HEIGHT 150    // (x4 via DVI_VERTICAL_REPEAT)

const struct dvi_timing __not_in_flash_func(dvi_timing_800x600p_60hz_280K) = {
	.h_sync_polarity   = false,
	.h_front_porch     = 44,
	.h_sync_width      = 128,
	.h_back_porch      = 88,
	.h_active_pixels   = 800,

	.v_sync_polarity   = false,
	.v_front_porch     = 2,        // increased from 1
	.v_sync_width      = 4,
	.v_back_porch      = 22,
	.v_active_lines    = 600,

	.bit_clk_khz       = 280000
};

#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_800x600p_60hz_280K
#else   // 640x480
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 160    // (×3 via DVI_VERTICAL_REPEAT = 480 lines)

#define VREG_VSEL VREG_VOLTAGE_1_10  // 252 MHz is comfortable at lower voltage
#define DVI_TIMING dvi_timing_640x480p_60hz
#endif

// // UART config on the last GPIOs
// #define UART_TX_PIN (28)
// #define UART_RX_PIN (29) /* not available on the pico */
// #define UART_ID     uart0
// #define BAUD_RATE   115200

#define RGB888_TO_RGB332(_r, _g, _b) \
    (                                \
        ((_r) & 0xE0)         |      \
        (((_g) & 0xE0) >>  3) |      \
        (((_b) & 0xC0) >>  6)        \
    )

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

struct dvi_inst dvi0;

// RGB888 palettes - shared by both 640x480 and 800x600 modes
// Store in flash to save RAM
const uint32_t palette__dmg_nso[4] = {
    0x8cad28,  // GB 0 = White (DMG green lightest)
    0x6c9421,  // GB 1 = Light gray
    0x426b29,  // GB 2 = Dark gray
    0x214231   // GB 3 = Black (DMG green darkest)
};

const uint32_t palette__gbp_nso[4] = {
    0xb5c69c,  // GB 0 = White (GBP lightest)
    0x8d9c7b,  // GB 1 = Light gray
    0x6c7251,  // GB 2 = Dark gray
    0x303820   // GB 3 = Black (GBP darkest)
};

const uint32_t* game_palette_rgb888 = palette__gbp_nso;

static void set_game_palette(int index);

#if RESOLUTION_800x600
// For 2bpp packed mode with DVI_SYMBOLS_PER_WORD=2:
// - Buffer contains packed 2bpp data (4 pixels per byte)
// - 160 pixels = 40 bytes per scanline
// - The TMDS encoder handles 4× horizontal scaling (160→640 pixels) with RGB888 palette
// - Plus 80 pixels of horizontal borders on each side (80+640+80 = 800 pixels total)
uint8_t line_buffer[DMG_PIXELS_X / 4] = {0};  // 40 bytes for 160 pixels packed

#else // 640x480
// For 2bpp packed mode with DVI_SYMBOLS_PER_WORD=2:
// - Buffer contains packed 2bpp data (4 pixels per byte)
// - 160 pixels = 40 bytes per scanline
// - The TMDS encoder handles 4× horizontal scaling (160→640 pixels)
// - No hardware doubling needed - encoder outputs full 640 pixels!
uint8_t line_buffer[DMG_PIXELS_X / 4] = {0};  // 40 bytes for 160 pixels packed

#endif // RESOLUTION


static void initialize_gpio(void);
#if ENABLE_PIO_DMG_BUTTONS
static void initialize_dmg_emu_buttons_pio_program(void);
#endif

#if ENABLE_CPU_DMG_BUTTONS
static void __no_inline_not_in_flash_func(gpio_callback)(uint gpio, uint32_t events);
#endif

static bool __no_inline_not_in_flash_func(nes_classic_controller)(void);
static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline);

void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    dvi_scanbuf_main_2bpp_gameboy(&dvi0);

    __builtin_unreachable();
}

static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline)
{
    static uint dmg_line_idx = 0;
    
    // scanlines are 0 to 143 (Game Boy native resolution, scaled 4× vertically to 576)
    
#if DVI_VERTICAL_REPEAT == 4
    // 600 lines / 4 = 150 scanlines
    // 144 rows of pixels
    // 150 - 144 = 6 extra lines
    // divide by 2 to center vertically = 3
    int offset = 3;
#else // DVI_VERTICAL_REPEAT == 3
    // 480 rows of pixels / 3 = 160 scanlines
    // 144 rows of pixels
    // 160 - 144 = 16 extra lines
    // divide by 2 to center vertically = 8
    int offset = 8;
#endif

    // Note:  First two scanlines are pushed before DVI start, so subtract 2 from offset
    offset -= 2;

    if ((scanline < offset) || (scanline >= (DMG_PIXELS_Y+offset)))
    {
        // Beyond game area - fill with black (all bits set = 0xFF for each pixel pair)
        // In 2bpp packed format: 0xFF = all pixels are value 3 (black/darkest color in palette)
        memset(line_buffer, 0xFF, sizeof(line_buffer));
        dmg_line_idx = 0;
    }
    else 
    {
        dmg_line_idx = scanline - offset;
        
        const uint8_t* packed_fb = (const uint8_t*)packed_display_ptr;
        if (packed_fb != NULL) {
            const uint8_t* packed_line = packed_fb + (dmg_line_idx * DMG_PIXELS_X / 4);  // 40 bytes per line            
            // Simply copy the packed 2bpp data directly!
            // No unpacking, no palette lookup - the TMDS encoder handles everything
            // Encoder will apply RGB888 palette and 4× horizontal scaling (160→640 pixels)
            // Plus 80 pixels of blank borders on each side for centering in 800 pixels
            memcpy(line_buffer, packed_line, sizeof(line_buffer));  // Copy 40 bytes
        } else {
            // No frame yet, fill with black
            memset(line_buffer, 0xFF, sizeof(line_buffer));
        }
    }

    const uint32_t *bufptr = (uint32_t*)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
    ;
}

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < sizeof(rom_bank0)) {
        return rom_bank0[addr];
    }
    if (addr < ACTIVE_ROM_LEN) {
        return ACTIVE_ROM_DATA[addr];
    }
    return 0xFF;
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < sizeof(cart_ram)) {
        return cart_ram[addr];
    }
    return 0xFF;
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    (void)gb;
    if (addr < sizeof(cart_ram)) {
        cart_ram[addr] = val;
    }
}

static void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val)
{
    (void)gb;
    printf("Peanut-GB error %d (val=%u)\n", gb_err, val);
    while (true) {
        tight_loop_contents();
    }
}

static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    (void)gb;
    if (line >= DMG_PIXELS_Y) {
        return;
    }

    uint8_t *dst = packed_render_ptr + (line * PACKED_LINE_STRIDE_BYTES);
    for (int x = 0, byte_idx = 0; x < DMG_PIXELS_X; x += 4, ++byte_idx) {
        const uint8_t p0 = pixels[x + 0] & 0x03;
        const uint8_t p1 = pixels[x + 1] & 0x03;
        const uint8_t p2 = pixels[x + 2] & 0x03;
        const uint8_t p3 = pixels[x + 3] & 0x03;
        dst[byte_idx] = (uint8_t)((p0 << 6) | (p1 << 4) | (p2 << 2) | p3);
    }
}

static void update_emulator_inputs(void)
{
    gb.direct.joypad_bits.a      = button_states[BUTTON_A];
    gb.direct.joypad_bits.b      = button_states[BUTTON_B];
    gb.direct.joypad_bits.select = button_states[BUTTON_SELECT];
    gb.direct.joypad_bits.start  = button_states[BUTTON_START];
    gb.direct.joypad_bits.up     = button_states[BUTTON_UP];
    gb.direct.joypad_bits.down   = button_states[BUTTON_DOWN];
    gb.direct.joypad_bits.left   = button_states[BUTTON_LEFT];
    gb.direct.joypad_bits.right  = button_states[BUTTON_RIGHT];
}

static void swap_display_buffers(void)
{
    __dmb();
    packed_display_ptr = packed_render_ptr;
    __dmb();
    packed_render_ptr = (packed_render_ptr == packed_buffer_0) ? packed_buffer_1 : packed_buffer_0;
}

static bool init_peanut_emulator(void)
{
    printf("ROM[0..3] = %02x %02x %02x %02x (len=%u)\n",
        ACTIVE_ROM_DATA[0], ACTIVE_ROM_DATA[1],
        ACTIVE_ROM_DATA[2], ACTIVE_ROM_DATA[3],
        ACTIVE_ROM_LEN);

    size_t rom0_bytes = ACTIVE_ROM_LEN < sizeof(rom_bank0) ? ACTIVE_ROM_LEN : sizeof(rom_bank0);
    memcpy(rom_bank0, ACTIVE_ROM_DATA, rom0_bytes);
    memset(cart_ram, 0xFF, sizeof(cart_ram));

    enum gb_init_error_e ret = gb_init(&gb,
        &gb_rom_read,
        &gb_cart_ram_read,
        &gb_cart_ram_write,
        &gb_error,
        NULL);

    if (ret != GB_INIT_NO_ERROR) {
        printf("gb_init failed: %d\n", ret);
        return false;
    }

    gb_init_lcd(&gb, lcd_draw_line);
    gb.direct.joypad = 0xFF;
#if ENABLE_AUDIO
    audio_init();
#endif
    return true;
}

static void run_emulator_frame(void)
{
    gb.gb_frame = 0;
    do {
        __gb_step_cpu(&gb);
        tight_loop_contents();
    } while (gb.gb_frame == 0);
}

static void handle_palette_hotkeys(void)
{
    static button_state_t last_left = BUTTON_STATE_UNPRESSED;
    static button_state_t last_right = BUTTON_STATE_UNPRESSED;

    if (button_states[BUTTON_SELECT] == BUTTON_STATE_PRESSED)
    {
        if ((button_states[BUTTON_LEFT] == BUTTON_STATE_UNPRESSED) && (last_left == BUTTON_STATE_PRESSED))
        {
            int index = get_scheme_index() - 1;
            if (index < 0)
            {
                index = NUMBER_OF_SCHEMES - 1;
            }
            set_game_palette(index);
        }

        if ((button_states[BUTTON_RIGHT] == BUTTON_STATE_UNPRESSED) && (last_right == BUTTON_STATE_PRESSED))
        {
            int index = get_scheme_index() + 1;
            if (index >= NUMBER_OF_SCHEMES)
            {
                index = 0;
            }
            set_game_palette(index);
        }
    }

    last_left = button_states[BUTTON_LEFT];
    last_right = button_states[BUTTON_RIGHT];
}

#if ENABLE_AUDIO
static size_t audio_samples_for_frame(void)
{
    audio_sample_residual += (uint64_t)AUDIO_SAMPLE_RATE * SCREEN_REFRESH_CYCLES_INT;
    size_t samples = (size_t)(audio_sample_residual / DMG_CLOCK_FREQ_INT);
    audio_sample_residual -= (uint64_t)samples * DMG_CLOCK_FREQ_INT;

    if (samples > MAX_AUDIO_SAMPLES_PER_FRAME)
    {
        samples = MAX_AUDIO_SAMPLES_PER_FRAME;
    }

    return samples;
}

static void write_samples_to_ring(const audio_sample_t *samples, size_t sample_count)
{
    audio_ring_t *ring = &dvi0.audio_ring;
    size_t available = get_write_size(ring, true);

    if (sample_count > available) {
        increase_read_pointer(ring, (uint32_t)(sample_count - available));
    }

    uint32_t offset = get_write_offset(ring);
    const uint32_t capacity = get_buffer_size(ring);
    audio_sample_t *buffer = get_buffer_top(ring);

    for (size_t i = 0; i < sample_count; ++i) {
        buffer[offset] = samples[i];
        offset = (offset + 1) % capacity;
    }

    set_write_offset(ring, offset);
}

static void pump_audio_samples(void)
{
    const size_t sample_count = audio_samples_for_frame();
    if (sample_count == 0)
    {
        return;
    }

    const size_t byte_count = sample_count * sizeof(audio_sample_t);
    audio_callback(NULL, (uint8_t *)apu_frame_buffer, (int)byte_count);
    write_samples_to_ring(apu_frame_buffer, sample_count);
}
#endif

int main(void)
{
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        button_states[i] = BUTTON_STATE_UNPRESSED;
    }

    stdio_init_all();
    sleep_ms(3000);  // Give USB more time to enumerate

    for (int i = 0; i < 5; i++) {
        printf("\n\n=== PicoDVI-DMG_EMU Starting (attempt %d) ===\n", i + 1);
        stdio_flush();
        sleep_ms(100);
    }

    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    memcpy(packed_buffer_0, mario_packed_160x144, PACKED_FRAME_SIZE);
    memcpy(packed_buffer_1, packed_buffer_0, PACKED_FRAME_SIZE);
    packed_display_ptr = packed_buffer_0;
    packed_render_ptr = packed_buffer_1;

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    set_game_palette(SCHEME_SGB_4H);

    uint32_t *bufptr = (uint32_t *)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

#if ENABLE_AUDIO
    int offset;
    if (rate == 48000) {
        offset = 2;
    } else if (rate == 44100) {
        offset = 1;
    } else if (rate == 24000) {
        offset = 5;
    } else if (rate == 22050) {
        offset = 3;
    } else if (rate == 16000) {
        offset = 4;
    } else {
        offset = 0;
    }
    memset(audio_buffer, 0, sizeof(audio_buffer));
    int cts = dvi0.timing->bit_clk_khz * hdmi_n[offset] / (rate / 100) / 128;
    dvi_get_blank_settings(&dvi0)->top = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, rate, cts, hdmi_n[offset]);
    increase_write_pointer(&dvi0.audio_ring, AUDIO_BUFFER_SIZE / 2);
    printf("Audio buffer pre-filled to %d samples (50%%)\n", AUDIO_BUFFER_SIZE / 2);
#endif

    printf("Starting Core 1 (DVI output)...\n");
    multicore_launch_core1(core1_main);
    printf("Core 1 running - now consuming video!\n");

    printf("Initializing GPIO...\n");
    initialize_gpio();
#if ENABLE_PIO_DMG_BUTTONS
    printf("Initializing PIO programs for DMG controller...\n");
    initialize_dmg_emu_buttons_pio_program();
#endif

#if ENABLE_CPU_DMG_BUTTONS
    gpio_set_irq_enabled_with_callback(DMG_READING_DPAD_PIN, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &gpio_callback);
    gpio_set_irq_enabled(DMG_READING_BUTTONS_PIN, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
#endif // ENABLE_CPU_DMG_BUTTONS

    if (!init_peanut_emulator()) {
        while (1) {
            tight_loop_contents();
        }
    }
    printf("Peanut-GB initialized - entering main loop\n");

    absolute_time_t next_frame_time = make_timeout_time_us(DMG_FRAME_DURATION_US);

    while (true)
    {
        nes_classic_controller();
        update_emulator_inputs();
        run_emulator_frame();
    #if ENABLE_AUDIO
        pump_audio_samples();
    #endif
        swap_display_buffers();
        handle_palette_hotkeys();

#if ENABLE_PIO_DMG_BUTTONS
        pio_sm_put(pio_dmg_emu_buttons, dmg_emu_buttons_sm, pio_buttons_out_value);
#endif

#if ENABLE_CPU_DMG_BUTTONS
        // Keep legacy GPIO mirroring in sync if ever re-enabled
        nes_classic_controller();
#endif

        sleep_until(next_frame_time);
        absolute_time_t now = get_absolute_time();
        int64_t now_us = (int64_t)to_us_since_boot(now);
        int64_t target_us = (int64_t)to_us_since_boot(next_frame_time);
        int64_t behind_us = now_us - target_us;
        next_frame_time = delayed_by_us(next_frame_time, DMG_FRAME_DURATION_US);
        if (behind_us > FRAME_CATCHUP_THRESHOLD_US)
        {
            next_frame_time = delayed_by_us(now, DMG_FRAME_DURATION_US);
        }
    }

    __builtin_unreachable();
}

#if ENABLE_PIO_DMG_BUTTONS
static void initialize_dmg_emu_buttons_pio_program(void)
{
    static const uint start_in_pin = DMG_READING_BUTTONS_PIN;
    static const uint start_out_pin = DMG_OUTPUT_RIGHT_A_PIN;

    // Get first free state machine in PIO 0
    dmg_emu_buttons_sm = pio_claim_unused_sm(pio_dmg_emu_buttons, true);

    // Add PIO program to PIO instruction memory. SDK will find location and
    // return with the memory offset of the program.
    uint offset = pio_add_program(pio_dmg_emu_buttons, &dmg_emu_buttons_program);

    // Calculate the PIO clock divider
    // float div = (float)clock_get_hz(clk_sys) / pio_freq;
    float div = (float)2;

    // Initialize the program using the helper function in our .pio file
    dmg_emu_buttons_program_init(pio_dmg_emu_buttons, dmg_emu_buttons_sm, offset, start_in_pin, start_out_pin, div);

    // Start running our PIO program in the state machine
    pio_sm_set_enabled(pio_dmg_emu_buttons, dmg_emu_buttons_sm, true);
}
#endif

static void initialize_gpio(void)
{    
    //Onboard LED
    gpio_init(ONBOARD_LED_PIN);
    gpio_set_dir(ONBOARD_LED_PIN, GPIO_OUT);
    gpio_put(ONBOARD_LED_PIN, 0);

    // Gameboy video signal inputs
    gpio_init(VSYNC_PIN);
    gpio_init(PIXEL_CLOCK_PIN);
    gpio_init(DATA_0_PIN);
    gpio_init(DATA_1_PIN);
    gpio_init(HSYNC_PIN);

    //Initialize I2C port at 400 kHz
    i2c_init(i2cHandle, 400 * 1000);

    // Initialize I2C pins
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SCL_PIN);
    gpio_pull_up(SDA_PIN);

#if ENABLE_CPU_DMG_BUTTONS
    gpio_init(DMG_OUTPUT_RIGHT_A_PIN);
    gpio_set_dir(DMG_OUTPUT_RIGHT_A_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_RIGHT_A_PIN, 1);

    gpio_init(DMG_OUTPUT_LEFT_B_PIN);
    gpio_set_dir(DMG_OUTPUT_LEFT_B_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_LEFT_B_PIN, 1);

    gpio_init(DMG_OUTPUT_UP_SELECT_PIN);
    gpio_set_dir(DMG_OUTPUT_UP_SELECT_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_UP_SELECT_PIN, 1);

    gpio_init(DMG_OUTPUT_DOWN_START_PIN);
    gpio_set_dir(DMG_OUTPUT_DOWN_START_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_DOWN_START_PIN, 1);

    gpio_init(DMG_READING_DPAD_PIN);
    gpio_set_dir(DMG_READING_DPAD_PIN, GPIO_IN);

    gpio_init(DMG_READING_BUTTONS_PIN);
    gpio_set_dir(DMG_READING_BUTTONS_PIN, GPIO_IN);
#endif // ENABLE_CPU_DMG_BUTTONS
}

// static bool nes_classic_controller(void)
static bool __no_inline_not_in_flash_func(nes_classic_controller)(void)
{
    static uint32_t last_micros = 0;
    static bool initialized = false;
    static uint8_t i2c_buffer[16] = {0};
    static absolute_time_t next_init_time = {0};
    static bool waiting_for_init = false;
    static uint32_t pending_init_delay_ms = NES_CONTROLLER_INIT_DELAY_MS;

    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 20000)   // probably longer than it needs to be, NES Classic queries about every 5ms
        return false;

    if (!initialized)
    {
        absolute_time_t now = get_absolute_time();
        if (!waiting_for_init)
        {
            next_init_time = delayed_by_us(now, pending_init_delay_ms * 1000u);
            waiting_for_init = true;
            return false;
        }

        if (absolute_time_diff_us(now, next_init_time) > 0)
        {
            return false;
        }

        waiting_for_init = false;
        pending_init_delay_ms = NES_CONTROLLER_REINIT_DELAY_MS;

        i2c_buffer[0] = 0xF0;
        i2c_buffer[1] = 0x55;
        (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 2, false);
        sleep_ms(10);

        i2c_buffer[0] = 0xFB;
        i2c_buffer[1] = 0x00;
        (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 2, false);
        sleep_ms(20);

        initialized = true;
        last_micros = time_us_32();
        return false;
    }

    last_micros = current_micros;

    i2c_buffer[0] = 0x00;
    (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 1, false);   // false - finished with bus
    sleep_us(300);  // NES Classic uses about 330uS
    int ret = i2c_read_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 8, false);
    if (ret < 0)
    {
        last_micros = time_us_32();
        return false;
    }
        
    bool valid = false;
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        if ((i < 4) && (i2c_buffer[i] != 0xFF))
            valid = true;

        if (valid)
        {
            if (i == 4)
            {
                button_states[BUTTON_START] = (~i2c_buffer[i] & (1<<2)) > 0 ? 0 : 1;
                button_states[BUTTON_SELECT] = (~i2c_buffer[i] & (1<<4)) > 0 ? 0 : 1;
                button_states[BUTTON_DOWN] = (~i2c_buffer[i] & (1<<6)) > 0 ? 0 : 1;
                button_states[BUTTON_RIGHT] = (~i2c_buffer[i] & (1<<7)) > 0 ? 0 : 1;

                button_states[BUTTON_HOME] = (~i2c_buffer[i] & (1<<3)) > 0 ? 0 : 1;
            }
            else if (i == 5)
            {
                button_states[BUTTON_UP] = (~i2c_buffer[i] & (1<<0)) > 0 ? 0 : 1;
                button_states[BUTTON_LEFT] = (~i2c_buffer[i] & (1<<1)) > 0 ? 0 : 1;
                button_states[BUTTON_A] = (~i2c_buffer[i] & (1<<4)) > 0 ? 0 : 1;
                button_states[BUTTON_B] = (~i2c_buffer[i] & (1<<6)) > 0 ? 0 : 1;
            }
        }
    }

    if (!valid )
    {
        initialized = false;
        waiting_for_init = false;
        pending_init_delay_ms = NES_CONTROLLER_REINIT_DELAY_MS;
        last_micros = time_us_32();
        return false;
    }

#ifdef DEBUG_BUTTON_PRESS
    uint8_t buttondown = 0;
    for (i = 0; i < BUTTON_COUNT; i++)
    {
        if (button_states[i] == BUTTON_STATE_PRESSED)
        {
            buttondown = 1;
        }
    }
    gpio_put(ONBOARD_LED_PIN, buttondown);
#endif

    uint8_t pins_dpad = 0;
    uint8_t pins_other = 0;
    if (button_states[BUTTON_A] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_RIGHT_A;

    if (button_states[BUTTON_B] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_LEFT_B;
    
    if (button_states[BUTTON_SELECT] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_UP_SELECT;
    
    if (button_states[BUTTON_START] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_DOWN_START;

    if (button_states[BUTTON_UP] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_UP_SELECT;

    if (button_states[BUTTON_DOWN] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_DOWN_START;
    
    if (button_states[BUTTON_LEFT] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_LEFT_B;
    
    if (button_states[BUTTON_RIGHT] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_RIGHT_A;

#if ENABLE_PIO_DMG_BUTTONS
    uint8_t pio_report = ~((pins_dpad << 4) | (pins_other&0xF));
    pio_buttons_out_value = (uint32_t)pio_report;
#endif

    return true;
}

#if ENABLE_CPU_DMG_BUTTONS
static void __no_inline_not_in_flash_func(gpio_callback)(uint gpio, uint32_t events)
{
#if ENABLE_VIDEO_CAPTURE && USE_VSYNC_IRQ
    // Handle VSYNC IRQ for video capture (GPIO 4) - ONLY IN IRQ MODE
    if (gpio == VSYNC_PIN) {
        video_capture_handle_vsync_irq(events);
        return;  // VSYNC handled, done
    }
#endif

    // Prevent controller input to game if OSD is visible
#if ENABLE_OSD
    if (OSD_is_enabled())
        return;
#endif // ENABLE_OSD

    if(gpio==DMG_READING_DPAD_PIN)
    {
        if (events & GPIO_IRQ_EDGE_FALL)   // Send DPAD states on low
        {
            if (gpio_get(DMG_READING_BUTTONS_PIN) == 1)
            {

                gpio_put(DMG_OUTPUT_RIGHT_A_PIN, button_states[BUTTON_RIGHT]);
                gpio_put(DMG_OUTPUT_LEFT_B_PIN, button_states[BUTTON_LEFT]);
                gpio_put(DMG_OUTPUT_UP_SELECT_PIN, button_states[BUTTON_UP]);
                gpio_put(DMG_OUTPUT_DOWN_START_PIN, button_states[BUTTON_DOWN]);
            }
        }

        if (events & GPIO_IRQ_EDGE_RISE)   // Send BUTTON states on low
        {
            // When P15 pin goes high, read cycle is complete, send all high
            if(gpio_get(DMG_READING_BUTTONS_PIN) == 1)
            {
                gpio_put(DMG_OUTPUT_RIGHT_A_PIN, 1);
                gpio_put(DMG_OUTPUT_LEFT_B_PIN, 1);
                gpio_put(DMG_OUTPUT_UP_SELECT_PIN, 1);
                gpio_put(DMG_OUTPUT_DOWN_START_PIN, 1);
            }
        }
    }

    if(gpio==DMG_READING_BUTTONS_PIN)
    {
        if (events & GPIO_IRQ_EDGE_FALL)   // Send BUTTON states on low
        {
            gpio_put(DMG_OUTPUT_RIGHT_A_PIN, button_states[BUTTON_A]);
            gpio_put(DMG_OUTPUT_LEFT_B_PIN, button_states[BUTTON_B]);
            gpio_put(DMG_OUTPUT_UP_SELECT_PIN, button_states[BUTTON_SELECT]);
            gpio_put(DMG_OUTPUT_DOWN_START_PIN, button_states[BUTTON_START]);

            // Prevent in-game reset lockup
            // If A,B,Select and Start are all pressed, release them!
            if ((button_states[BUTTON_A] | button_states[BUTTON_B] | button_states[BUTTON_SELECT]| button_states[BUTTON_START])==0)
            {
                button_states[BUTTON_A] = 1;
                button_states[BUTTON_B] = 1;
                button_states[BUTTON_SELECT] = 1;
                button_states[BUTTON_START] = 1;
            }
        }
    }
}
#endif // ENABLE_CPU_DMG_BUTTONS

// Palette support for both 640x480 and 800x600 modes
static void set_game_palette(int index)
{
    if ((index <0) || index >= NUMBER_OF_SCHEMES)
        return;

    set_scheme_index(index);
    game_palette_rgb888 = (uint32_t*)get_scheme();

    // Set RGB888 palette pointer for 2bpp palette mode
    // Works for both 640x480 (no borders) and 800x600 (with borders)
    dvi_get_blank_settings(&dvi0)->palette_rgb888 = game_palette_rgb888;
}
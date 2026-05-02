// Joe Ostrander
// 2025.12.06
// PicoDVI-DMG_EMU
//

// Keep these defines at the top before including pico headers
#define PICO_DEFAULT_UART_BAUD_RATE 115200
#define PICO_DEFAULT_UART_TX_PIN    0
#define PICO_DEFAULT_UART_RX_PIN    1
#define PICO_DEFAULT_UART 1
// #define PICO_STDIO_DEFAULT_CRLF 1

// REMINDER: Always use cmake with:  -DPICO_COPY_TO_RAM=1

// #pragma GCC optimize("Os")
// #pragma GCC optimize("O2")
#pragma GCC optimize("O3")


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <hardware/watchdog.h>
#include <stdint.h>
#include <unistd.h>
#include "hardware/vreg.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"
#include "pico/platform.h"
#include "pico/stdio_uart.h"
#if USE_BLUETOOTH_CONTROLLER
#include "pico/cyw43_arch.h"
#include "blue_host.h"
#endif

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "audio_ring.h"  // For get_write_size and get_read_size

#include "eeprom.h" // emulated with flash :)
#include "hardware/flash.h"
#include "pico/bootrom.h"

#include "colors.h"
#include "hedley.h"
#include "board_defs.h"
#include "sdcard.h"
#include "ff.h"
#include "f_util.h"
#include "diskio.h"

#include "video_defs.h"
#include "osd.h"
#include "font_5x7.h"

// Set to 1 to enable HOME button to reset into USB mass storage mode for easier programming
#define HOME_RESETS_TO_BOOTLOADER       0  

#define SAMPLE_FREQ                     32000
#define AUDIO_BUFFER_SIZE               1024
#define AUDIO_SAMPLE_RATE               SAMPLE_FREQ
#define ENABLE_SOUND                    1
#define DMG_FRAME_DURATION_US           ((uint32_t)(1000000.0 / VERTICAL_SYNC + 0.5))
#define FRAME_CATCHUP_THRESHOLD_US      (DMG_FRAME_DURATION_US * 64u)

// For *NES Classic* I2C-based controller
#define NES_CONTROLLER_INIT_DELAY_MS    2000u
#define NES_CONTROLLER_REINIT_DELAY_MS  1000u

#if USE_BLUETOOTH_CONTROLLER
#define BT_PAIR_BUTTON_GPIO             2u
#define BT_PAIRING_TIMEOUT_MS           60000u
#define BT_PAIR_LONG_PRESS_MS           3000u
#define BT_CFG_MAGIC                    0xBCu
#define BT_CFG_VERSION                  1u
#define BT_CFG_INDEX_MAGIC              240u
#define BT_CFG_INDEX_VERSION            241u
#define BT_CFG_INDEX_CONTROLLER_TYPE    242u
#define BT_CFG_INDEX_REMOTE_ADDR        243u
#define BT_CFG_INDEX_CHECKSUM           249u
#endif

#include "audio/minigb_apu.h"
#include "peanut_gb.h"
#include "roms/controller_test_rom.h"

#define DMG_CLOCK_FREQ_INT        ((uint32_t)DMG_CLOCK_FREQ)
#define SCREEN_REFRESH_CYCLES_INT ((uint32_t)SCREEN_REFRESH_CYCLES)
#define MAX_AUDIO_SAMPLES_PER_FRAME ((uint32_t)(((uint64_t)AUDIO_SAMPLE_RATE * SCREEN_REFRESH_CYCLES_INT + DMG_CLOCK_FREQ_INT - 1) / DMG_CLOCK_FREQ_INT))

#define ENABLE_AUDIO                1  // Enable Peanut-GB audio path
#define ENABLE_OSD                  1  // Set to 1 to enable OSD code, 0 to disable
#ifndef ENABLE_SD_CARD
#define ENABLE_SD_CARD              1  // Set to 0 to skip SD init/menu and use built-in ROM only
#endif
#define ENABLE_SD_STATS_LOG         0  // Set to 1 to print periodic SD cache hit/miss stats
#define ENABLE_HEAP_LOG             0  // Set to 1 to print free-heap checkpoints

#define MAX_SD_ROM_FILE_BYTES       (ROM_BANK_SIZE * 512u)
#define MAX_SD_ROM_HEAP_BYTES       (320u * 1024u)  // allow 256 KB ROMs to heap-load
#define SD_HEAP_SAFETY_MARGIN_BYTES (8u * 1024u)
#define SD_ROM_CACHE_SLOTS          8u    // larger cache to cut bank thrash on streamed carts
#define SD_STREAM_CHUNK_BYTES       4096u // balanced chunk to reduce overhead without long stalls
#define ENABLE_SD_HEAP_LOAD         1   // 0 = always stream from SD to save RAM, 1 = allow heap copy when small enough
#define MAX_SD_ROM_LIST             128u // max ROM entries to list from SD
#define MAX_SD_ROM_PATH_LEN         192u
#define MENU_MAX_LABEL_CHARS        24u
#define MENU_LINE_HEIGHT            8
#define MENU_COLOR_BG               0u
#define MENU_COLOR_FG               3u
#define MENU_COLOR_HL_BG            2u
#define MENU_COLOR_HL_FG            1u
#define MENU_COLOR_TITLE            3u

#define BIT_IS_CLEAR(value, bit)    (((value) & (1U << (bit))) == 0)


#if ENABLE_AUDIO
static const int hdmi_n[6] = {4096, 6272, 6144, 3136, 4096, 6144};  // 32k, 44.1k, 48k, 22.05k, 16k, 24k
static uint16_t rate = SAMPLE_FREQ;
static audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
static audio_sample_t apu_frame_buffer[MAX_AUDIO_SAMPLES_PER_FRAME];
static uint64_t audio_sample_residual = 0;
#endif

// #define DEBUG_BUTTON_PRESS


i2c_inst_t* i2cHandle = MY_I2C_INSTANCE;

//********************************************************************************
// TYPEDEFS AND STRUCTS
//********************************************************************************
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
    OSD_LINE_COLOR_SCHEME = 0,
    // OSD_LINE_BORDER_COLOR,
    OSD_LINE_FRAME_BLENDING,
    OSD_LINE_AUDIO_RESET,
    OSD_LINE_RESET_DEVICE,
    OSD_LINE_SAVE_SETTINGS,
    OSD_LINE_EXIT,
    OSD_LINE_COUNT
} osd_line_t;

typedef enum
{
    BUTTON_STATE_PRESSED = 0,
    BUTTON_STATE_UNPRESSED
} button_state_t;

typedef enum
{
    ROM_SOURCE_BUILTIN = 0,
    ROM_SOURCE_SD_STREAM,
    ROM_SOURCE_SD_HEAP
} rom_source_t;

typedef struct
{
    uint32_t bank_index;
    size_t bytes_valid;
    bool valid;
    uint8_t data[ROM_BANK_SIZE];
} sd_rom_cache_slot_t;

typedef struct
{
    FIL handle;
    bool open;
    size_t size_bytes;
    uint32_t bank_count;
    uint32_t next_replace_slot;
} sd_rom_stream_state_t;

typedef enum
{
    SAVE_INDEX_SCHEME = 0,
    SAVE_INDEX_FRAME_BLENDING
} save_position_t;

typedef enum
{
    DISABLE_MASK_NONE = 0,
    DISABLE_MASK_MASS_STORAGE,
    DISABLE_MASK_PICOBOOT
} interface_disable_mask_t;

typedef enum
{
    RESTART_NORMAL = 0,
    RESTART_MASS_STORAGE,
} restart_option_t;
//********************************************************************************
// PRIVATE VARIABLES
//********************************************************************************
// Packed DMA buffers - 4 pixels per byte (2 bits each)
// This is the native format from the Game Boy (2 bits per pixel)
// Used by BOTH 640x480 and 800x600 modes for DMA capture AND display
static uint8_t packed_buffer_0[PACKED_FRAME_SIZE] = {0};
static uint8_t packed_buffer_1[PACKED_FRAME_SIZE] = {0};
static uint8_t packed_buffer_previous[PACKED_FRAME_SIZE] = {0};  // prior frame for blending
static uint8_t store_lut[256];                                    // precomputed ghost store values
static volatile bool frame_blending_enabled = false;

// TMDS encoder handles palette conversion and horizontal scaling
static volatile uint8_t* packed_display_ptr = packed_buffer_0;
static uint8_t* packed_render_ptr = packed_buffer_1;

static sd_rom_cache_slot_t sd_rom_cache[SD_ROM_CACHE_SLOTS];
static char sd_rom_list[MAX_SD_ROM_LIST][MAX_SD_ROM_PATH_LEN];
static uint32_t sd_rom_list_count = 0;
#if ENABLE_SD_STATS_LOG
static uint32_t sd_cache_hits = 0;
static uint32_t sd_cache_misses = 0;
static uint32_t sd_cache_log_frames = 0;
#endif

static struct gb_s gb;
static uint8_t rom_bank0[0x4000];
static uint8_t cart_ram[0x8000];
static sd_card_t *mounted_sd_card = NULL;
static bool sd_filesystem_ready = false;
static bool sd_rom_discovered = false;
static char sd_rom_path[256] = {0};
static const uint8_t *active_rom_data = ACTIVE_ROM_DATA;
static size_t active_rom_length = ACTIVE_ROM_LEN;
static rom_source_t active_rom_source = ROM_SOURCE_BUILTIN;
static volatile bool gb_faulted = false;
static struct
{
    enum gb_error_e code;
    uint16_t val;
    uint16_t pc;
    uint16_t sp;
    uint16_t rom_bank;
} gb_fault_info = {0};
static uint8_t *sd_rom_heap = NULL;
static size_t sd_rom_heap_size = 0;
static sd_rom_stream_state_t sd_rom_stream = {
    .open = false,
    .size_bytes = 0,
    .bank_count = 0,
    .next_replace_slot = 0
};

static uint8_t button_states[BUTTON_COUNT];
static uint8_t button_states_previous[BUTTON_COUNT];

#if USE_BLUETOOTH_CONTROLLER
static bt_host_gamepad_report_t bt_latest_report;
static bool bt_report_valid = false;
static bool bt_controller_initialized = false;
static bt_host_config_t bt_runtime_config;
#endif


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


uint8_t line_buffer[DMG_PIXELS_X / 4] = {0};  // 40 bytes for 160 pixels packed

static restart_option_t restart_option = RESTART_NORMAL;

// Duplicated from tmds_encode.c
static const __unused uint32_t __scratch_x("tmds_table") tmds_table[] = {
#include "tmds_table.h"
};

//********************************************************************************
// PRIVATE FUNCTION PROTOTYPES
//********************************************************************************
static void core1_main(void);
static void __no_inline_not_in_flash_func(prepare_scanline_2bpp_gameboy)(struct dvi_inst *inst, const uint8_t *packed_scanbuf);
static void __not_in_flash_func(tmds_encode_2bpp_packed_gameboy)(const uint8_t *packed_pixbuf,
                                    uint32_t *symbuf_r,
                                    uint32_t *symbuf_g,
                                    uint32_t *symbuf_b,
                                    size_t output_words,
                                    uint32_t horizontal_repeat,
                                    size_t input_pixels,
                                    const uint32_t *palette_rgb888);
static void init_frame_blending_luts(void);
static bool mount_sd_card(void);
static bool filename_is_rom(const char *filename);
static void clear_sd_rom_list(void);
static bool add_rom_to_list(const char *directory, const char *filename);
static uint32_t scan_directory_for_roms(const char *directory);
static bool build_sd_rom_list(void);
static const char *path_basename(const char *path);
// static void print_sd_rom_list(void);
static inline void set_pixel_2bpp(uint8_t *buf, int x, int y, uint8_t color);
static void fill_buffer_2bpp(uint8_t *buf, uint8_t color);
static void draw_glyph_5x7(uint8_t *buf, int x, int y, const glyph_5x7_t *glyph, uint8_t fg, uint8_t bg);
static void draw_text_line(uint8_t *buf, int x, int y, const char *text, uint8_t fg, uint8_t bg);
static void format_rom_label(const char *path, char *out, size_t out_len);
static void render_rom_menu(uint32_t selected_index);
static bool sd_rom_selection_menu(char *selected_path, size_t selected_len);
static void boot_checkpoint(const char *label);
static void invalidate_sd_rom_cache(void);
static void close_sd_rom_stream(void);
static void free_sd_rom_heap(void);
static size_t estimate_free_heap_bytes(void);
static void log_free_heap(const char *tag);
static bool ensure_audio_ready(void);
static bool sd_stream_load_bank(uint32_t bank_index, sd_rom_cache_slot_t *slot);
static uint8_t sd_stream_read_byte(size_t addr);
// static bool discover_sd_rom(void);
static void reset_active_rom_to_builtin(void);
static bool load_sd_rom_file(const char *path);
static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline);
static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
static void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val);
static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);
static void update_emulator_inputs(void);
static void swap_display_buffers(void);
static bool init_peanut_emulator(void);
static void run_emulator_frame(void);
static void initialize_gpio(void);
static bool __no_inline_not_in_flash_func(game_controller)(void);
static void set_game_palette(int index);
static void sd_stream_chunk_yield(void);
static bool button_is_pressed(controller_button_t button);
static bool button_was_released(controller_button_t button);
static bool command_check(void);
static void button_state_save_previous(void);
static void reset_button_states(void);
static void save_settings(void);
static void reset_pico(restart_option_t restart_option);
static void load_settings(void);
static void update_osd(void);
static void restart_audio_pipeline(void);
static void reset_audio_ring_prefill(size_t fill_samples);

#if USE_BLUETOOTH_CONTROLLER
static bool init_bluetooth_controller(void);
static void bluetooth_report_callback(const bt_host_gamepad_report_t *report);
static void bluetooth_pairing_complete_callback(const uint8_t remote_addr[6]);
static void bluetooth_pairing_reset_callback(void);
static bool bt_load_paired_controller(bt_host_config_t *config);
static bool bt_save_paired_controller(const bt_host_config_t *config);
static void bt_clear_paired_controller(void);
static uint8_t bt_config_checksum(const uint8_t remote_addr[6], uint8_t controller_type);
static bool bt_address_is_unset(const uint8_t remote_addr[6]);
static bool bt_controller_type_supported(uint8_t controller_type);
static inline bool bt_hat_has_direction(uint8_t hat, controller_button_t button);
static inline bool bt_button_pressed(controller_button_t button);
#endif


#if ENABLE_AUDIO
static size_t audio_samples_for_frame(void);
static void pump_audio_samples(void);
static void write_samples_to_ring(const audio_sample_t *samples, size_t sample_count);
#endif

//********************************************************************************
// PRIVATE FUNCTIONS
//********************************************************************************
static void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    while (true)
    {
        const uint8_t *scanbuf = NULL;
        if (queue_try_remove_u32(&dvi0.q_colour_valid, (uint32_t*)&scanbuf))
        {
            prepare_scanline_2bpp_gameboy(&dvi0, scanbuf);
            queue_add_blocking_u32(&dvi0.q_colour_free, (uint32_t*)&scanbuf);
        }
    }
}

static void __no_inline_not_in_flash_func(prepare_scanline_2bpp_gameboy)(struct dvi_inst *inst, const uint8_t *packed_scanbuf)
{
    static uint scanline_idx = 0;

    uint32_t *tmdsbuf = NULL;
    queue_remove_blocking_u32(&inst->q_tmds_free, &tmdsbuf);
    uint pixwidth = inst->timing->h_active_pixels;             // e.g., 800
    uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;  // e.g., 400 when SPW=2

    static const uint32_t default_palette[4] = {
        0xb5c69c, 0x8d9c7b, 0x6c7251, 0x303820
    };
    const uint32_t *palette = (const uint32_t*)inst->blank_settings.palette_rgb888;
    if (palette == NULL) {
        palette = default_palette;
    }

    const uint current_scanline = scanline_idx;
    scanline_idx = (scanline_idx + 1) % SCANLINE_COUNT;

    const bool in_active_window =
        (current_scanline >= VERTICAL_OFFSET) &&
        (current_scanline < (DMG_PIXELS_Y + VERTICAL_OFFSET));

    if (!in_active_window || packed_scanbuf == NULL)
    {
        // Force full black using TMDS zero symbol (not palette-derived) for porches/blank lines
        const uint32_t black_word = tmds_table[0];
        for (uint word_idx = 0; word_idx < words_per_channel; ++word_idx) {
            tmdsbuf[2 * words_per_channel + word_idx] = black_word;
            tmdsbuf[1 * words_per_channel + word_idx] = black_word;
            tmdsbuf[0 * words_per_channel + word_idx] = black_word;
        }
    } 
    else 
    {
        tmds_encode_2bpp_packed_gameboy(
            packed_scanbuf,
            tmdsbuf + 2 * words_per_channel,  // Red
            tmdsbuf + 1 * words_per_channel,  // Green
            tmdsbuf + 0 * words_per_channel,  // Blue
            words_per_channel,
            HORIZONTAL_SCALE,
            DMG_PIXELS_X,
            palette);
    }

    queue_add_blocking_u32(&inst->q_tmds_valid, &tmdsbuf);
}
                                     
// Flexible 2bpp packed encoder with runtime RGB888 palette support
// Input: packed 2bpp data (4 pixels per byte, 40 bytes = 160 pixels per scanline)
// Output: RGB TMDS symbols with centered 4× horizontal scaling (160→640 pixels) + optional borders
// Works for any resolution: calculates borders automatically based on output_words
//   - 640x480: output_words=320 → 0px borders, 640 game pixels (160×4)
//   - 800x600: output_words=400 → 80px borders each side, 640 game pixels (160×4)
// With DVI_SYMBOLS_PER_WORD=2: pixels = output_words × 2
// 2bpp packed encoder for 800x600 resolution with RGB888 palette support
// Input: 40 bytes (160 GB pixels packed as 2bpp)
// Output: 800 pixels (80 black border + 640 game area + 80 black border)
// With DVI_SYMBOLS_PER_WORD=2: 800 pixels = 400 words per channel
static void __not_in_flash_func(tmds_encode_2bpp_packed_gameboy)(
    const uint8_t *packed_pixbuf,    // Input: packed pixels (e.g., 40 bytes = 160 pixels)
    uint32_t *symbuf_r,              // Output: Red channel TMDS symbols
    uint32_t *symbuf_g,              // Output: Green channel TMDS symbols
    uint32_t *symbuf_b,              // Output: Blue channel TMDS symbols
    size_t output_words,             // Number of output words per channel (pixels = output_words × DVI_SYMBOLS_PER_WORD)
    uint32_t horizontal_repeat,      // Horizontal scale factor (e.g., 4 for x4, 2 for x2)
    size_t input_pixels,             // Number of source pixels in the line (e.g., 160)
    const uint32_t *palette_rgb888   // Palette: 4 RGB888 colors (0xRRGGBB format)
)
{
    // Build TMDS symbol lookup tables from RGB888 palette at runtime
    // This happens once per scanline, but it's only 12 lookups total (4 colors × 3 channels)
    uint32_t tmds_palette_red[4];
    uint32_t tmds_palette_green[4];
    uint32_t tmds_palette_blue[4];

    for (int i = 0; i < 4; i++)
    {
        uint32_t color = palette_rgb888[i];
        uint8_t r8 = (color >> 16) & 0xFF;
        uint8_t g8 = (color >> 8) & 0xFF;
        uint8_t b8 = color & 0xFF;
        
        // Convert 8-bit to 6-bit indices for tmds_table lookup
        tmds_palette_red[i]   = tmds_table[r8 >> 2];
        tmds_palette_green[i] = tmds_table[g8 >> 2];
        tmds_palette_blue[i]  = tmds_table[b8 >> 2];
    }
  
    // Get black color for borders (darkest color in palette)
    const uint32_t black_word = tmds_table[0];
  
    const uint8_t *src = packed_pixbuf;
    const size_t packed_bytes = input_pixels / 4;  // 4 pixels per packed byte
    const uint32_t words_per_pixel = horizontal_repeat / DVI_SYMBOLS_PER_WORD;  // each word = 2 pixels
  
  // Calculate horizontal layout based on output_words
    // Compute active area from input pixel count and horizontal repeat factor
    const size_t game_words = (input_pixels * horizontal_repeat) / DVI_SYMBOLS_PER_WORD;
  
    // Calculate border width on each side (centered)
    // For 640x480: output_words=320, border=0
    // For 800x600: output_words=400, border=40 words (80 pixels)
    size_t border_words = (output_words > game_words) ? (output_words - game_words) / 2 : 0;
    
    size_t word_idx = 0;

    // LEFT BORDER (if any)
    for (size_t i = 0; i < border_words; i++)
    {
        symbuf_r[word_idx] = black_word;
        symbuf_g[word_idx] = black_word;
        symbuf_b[word_idx] = black_word;
        word_idx++;
    }
  
    // GAME AREA: input_pixels × horizontal_repeat
    // Process each input byte (contains 4 packed pixels)
    // Each pixel gets replicated horizontal_repeat× for horizontal scaling
    for (size_t byte_idx = 0; byte_idx < packed_bytes; byte_idx++) 
    {
        uint8_t packed_byte = src[byte_idx];
      
        // Extract and process each of the 4 pixels in this byte
        for (int pixel_in_byte = 0; pixel_in_byte < 4; pixel_in_byte++)
        {
            // Extract 2-bit pixel value (MSB first: bits 7-6, 5-4, 3-2, 1-0)
            uint shift = (3 - pixel_in_byte) * 2;
            uint8_t pixel_2bpp = (packed_byte >> shift) & 0x03;
            
            // Get TMDS symbol pair for this color
            uint32_t word_r = tmds_palette_red[pixel_2bpp];
            uint32_t word_g = tmds_palette_green[pixel_2bpp];
            uint32_t word_b = tmds_palette_blue[pixel_2bpp];
            
            // Replicate this pixel horizontally: two TMDS symbols per word
            for (uint32_t repeat = 0; repeat < words_per_pixel; repeat++)
            {
                symbuf_r[word_idx] = word_r;
                symbuf_g[word_idx] = word_g;
                symbuf_b[word_idx] = word_b;
                word_idx++;
            }
        }
    }
  
    // RIGHT BORDER: Fill remaining words with black
    while (word_idx < output_words)
    {
        symbuf_r[word_idx] = black_word;
        symbuf_g[word_idx] = black_word;
        symbuf_b[word_idx] = black_word;
        word_idx++;
    }
}

// Initialize frame blending lookup tables for ultra-fast processing
// Called once at startup to precompute all 256×256 byte combinations
// This implements the exact logic from old_code.c:
//   Blend: new_value == 0 ? new_value|*pixel_old : new_value
//   Store: new_value > 0 ? 2 : 0  (brightens ghosts by storing gray)
// Used by BOTH 640x480 and 800x600 modes
static void init_frame_blending_luts(void)
{
    // Build the store_lut first (what to save for next frame's ghost)
    // This implements: *pixel_old++ = new_value > 0 ? 2 : 0;
    // For each byte, convert: non-white pixels → gray (2), white → white (0)
    // The "2" (gray) value creates the "brightened" ghost effect
    for (int curr = 0; curr < 256; curr++) {
        uint8_t result = 0;
        for (int pixel = 0; pixel < 4; pixel++) {
            int shift = (3 - pixel) * 2;
            uint8_t p = (curr >> shift) & 0x03;
            // Non-white (1,2,3) becomes gray (2), white (0) stays white (0)
            // This is the "brighten up the previous frame" logic!
            uint8_t store_p = (p > 0) ? 2 : 0;
            result |= (store_p << shift);
        }
        store_lut[curr] = result;
    }
      // Note: blend_lut removed to save 64KB RAM (65,536 bytes)
    // Blending is now calculated inline in the frame processing loop
}

static bool mount_sd_card(void)
{
    printf("[SD] mount_sd_card entry\n");

    if (sd_filesystem_ready && mounted_sd_card != NULL && mounted_sd_card->state.mounted) {
        printf("SD mount skipped: already mounted.\n");
        return true;
    }

    sd_card_t *card = sd_get_by_num(0);
    if (card == NULL) {
        printf("SD mount skipped: no slot configured\n");
        return false;
    }

    printf("Initializing SD card interface...\n");

    if (!sd_init_driver()) {
        printf("SD mount failed: sd_init_driver() did not succeed\n");
        return false;
    }

    int status = card->init(card);
    if (status & STA_NODISK) {
        printf("SD mount failed: no card detected (status=0x%02x)\n", status);
        return false;
    }

    if (status & STA_NOINIT) {
        printf("SD mount failed: card did not initialize (status=0x%02x)\n", status);
        return false;
    }

    const char *drive_prefix = sd_get_drive_prefix(card);
    printf("Mounting FatFs volume on %s\n", (drive_prefix != NULL && drive_prefix[0] != '\0') ? drive_prefix : "<null>");

    FRESULT mount_result = f_mount(&card->state.fatfs, drive_prefix, 1);
    if (mount_result != FR_OK) {
        printf("f_mount failed (%d: %s)\n", mount_result, FRESULT_str(mount_result));
        card->state.mounted = false;
        mounted_sd_card = NULL;
        sd_filesystem_ready = false;
        return false;
    }

    if (drive_prefix != NULL && drive_prefix[0] != '\0') {
        FRESULT chdrive_result = f_chdrive(drive_prefix);
        if (chdrive_result != FR_OK) {
            printf("f_chdrive failed (%d: %s) for %s\n",
                   chdrive_result,
                   FRESULT_str(chdrive_result),
                   drive_prefix);
            f_mount(NULL, drive_prefix, 0);
            card->state.mounted = false;
            mounted_sd_card = NULL;
            sd_filesystem_ready = false;
            return false;
        }
    }

    FRESULT chdir_result = f_chdir("/");
    if (chdir_result != FR_OK) {
        printf("f_chdir('/') failed (%d: %s)\n", chdir_result, FRESULT_str(chdir_result));
        if (drive_prefix != NULL && drive_prefix[0] != '\0') {
            f_mount(NULL, drive_prefix, 0);
        }
        card->state.mounted = false;
        mounted_sd_card = NULL;
        sd_filesystem_ready = false;
        return false;
    }

    card->state.mounted = true;
    mounted_sd_card = card;
    sd_filesystem_ready = true;
    sd_rom_discovered = false;
    sd_rom_path[0] = '\0';

    printf("SD filesystem ready.\n");
    return true;
}

static bool filename_is_rom(const char *filename)
{
    if (filename == NULL) {
        return false;
    }

    const char *dot = strrchr(filename, '.');
    if ((dot == NULL) || (dot[1] == '\0')) {
        return false;
    }

    const char *ext = dot + 1;
    const char first = (char)tolower((unsigned char)ext[0]);
    const char second = (char)tolower((unsigned char)ext[1]);

    if ((first != 'g') || (second != 'b')) {
        return false;
    }

    if (ext[2] == '\0') {
        return true;  // .gb
    }

    const char third = (char)tolower((unsigned char)ext[2]);
    return (third == 'c') && (ext[3] == '\0');  // .gbc
}

// static bool find_first_rom_in_directory(const char *directory, char *out_path, size_t out_len)
// {
//     if ((directory == NULL) || (out_path == NULL) || (out_len == 0)) {
//         return false;
//     }

//     size_t dir_len = strlen(directory);
//     bool directory_has_sep = (dir_len > 0) && (directory[dir_len - 1] == '/' || directory[dir_len - 1] == '\\');

//     DIR dir;
//     FILINFO info;
//     memset(&info, 0, sizeof(info));
//     FRESULT fr = f_opendir(&dir, directory);
//     if (fr != FR_OK) {
//         return false;
//     }

//     bool found = false;
//     while (true) {
//         fr = f_readdir(&dir, &info);
//         if ((fr != FR_OK) || (info.fname[0] == '\0')) {
//             break;
//         }

//         if (info.fattrib & AM_DIR) {
//             continue;
//         }

//         const char *name = (const char *)info.fname;
// #if defined(FF_USE_LFN) && (FF_USE_LFN != 0)
//         if ((name == NULL || name[0] == '\0') && info.altname[0] != '\0') {
//             name = (const char *)info.altname;
//         }
// #endif
//         if ((name == NULL) || !filename_is_rom(name)) {
//             continue;
//         }

//         const char *fmt = directory_has_sep ? "%s%s" : "%s/%s";
//         int needed = snprintf(out_path, out_len, fmt, directory, name);
//         if ((needed > 0) && ((size_t)needed < out_len)) {
//             found = true;
//         }
//         break;
//     }

//     FRESULT close_result = f_closedir(&dir);
//     if ((close_result != FR_OK) && !found) {
//         printf("f_closedir failed (%d: %s) while scanning %s\n", close_result, FRESULT_str(close_result), directory);
//     }
//     return found;
// }

// static void log_roms_in_directory(const char *directory)
// {
//     if ((directory == NULL) || !sd_filesystem_ready) {
//         return;
//     }

//     DIR dir;
//     FILINFO info;
//     memset(&info, 0, sizeof(info));
//     FRESULT fr = f_opendir(&dir, directory);
//     if (fr != FR_OK) {
//         printf("ROM scan: unable to open %s (%d: %s)\n", directory, fr, FRESULT_str(fr));
//         return;
//     }

//     printf("ROM scan: %s\n", directory);
//     bool any = false;

//     while (true) {
//         fr = f_readdir(&dir, &info);
//         if (fr != FR_OK) {
//             printf("  readdir failed (%d: %s)\n", fr, FRESULT_str(fr));
//             break;
//         }
//         if (info.fname[0] == '\0') {
//             break;
//         }
//         if (info.fattrib & AM_DIR) {
//             continue;
//         }

//         const char *name = (const char *)info.fname;
// #if defined(FF_USE_LFN) && (FF_USE_LFN != 0)
//         if ((name == NULL || name[0] == '\0') && info.altname[0] != '\0') {
//             name = (const char *)info.altname;
//         }
// #endif
//         if ((name == NULL) || !filename_is_rom(name)) {
//             continue;
//         }

//         any = true;
//         printf("  %s\n", name);
//     }

//     if (!any) {
//         printf("  <no ROMs>\n");
//     }

//     FRESULT close_result = f_closedir(&dir);
//     if (close_result != FR_OK) {
//         printf("  close failed (%d: %s)\n", close_result, FRESULT_str(close_result));
//     }
// }

// static void dump_sd_rom_inventory(void)
// {
//     if (!sd_filesystem_ready) {
//         printf("ROM scan skipped: filesystem not ready\n");
//         return;
//     }

//     const char *search_paths[] = {
//         "0:/ROMS",
//         "0:/"
//     };

//     for (size_t i = 0; i < sizeof(search_paths)/sizeof(search_paths[0]); ++i) {
//         log_roms_in_directory(search_paths[i]);
//     }
// }

static void clear_sd_rom_list(void)
{
    sd_rom_list_count = 0;
    for (uint32_t i = 0; i < MAX_SD_ROM_LIST; ++i) {
        sd_rom_list[i][0] = '\0';
    }
}

static bool add_rom_to_list(const char *directory, const char *filename)
{
    if ((directory == NULL) || (filename == NULL) || (sd_rom_list_count >= MAX_SD_ROM_LIST)) {
        return false;
    }

    const size_t dir_len = strlen(directory);
    const bool has_sep = (dir_len > 0) && (directory[dir_len - 1] == '/' || directory[dir_len - 1] == '\\');
    char composed[MAX_SD_ROM_PATH_LEN];
    const char *fmt = has_sep ? "%s%s" : "%s/%s";
    int written = snprintf(composed, sizeof(composed), fmt, directory, filename);
    if ((written <= 0) || ((size_t)written >= sizeof(composed))) {
        return false;
    }

    // Avoid duplicates (same path)
    for (uint32_t i = 0; i < sd_rom_list_count; ++i) {
        if (strcmp(sd_rom_list[i], composed) == 0) {
            return false;
        }
    }

    strncpy(sd_rom_list[sd_rom_list_count], composed, MAX_SD_ROM_PATH_LEN - 1);
    sd_rom_list[sd_rom_list_count][MAX_SD_ROM_PATH_LEN - 1] = '\0';
    sd_rom_list_count++;
    return true;
}

static uint32_t scan_directory_for_roms(const char *directory)
{
    if ((directory == NULL) || !sd_filesystem_ready) {
        return 0;
    }

    DIR dir;
    FILINFO info;
    memset(&info, 0, sizeof(info));
    FRESULT fr = f_opendir(&dir, directory);
    if (fr != FR_OK) {
        printf("ROM scan: unable to open %s (%d: %s)\n", directory, fr, FRESULT_str(fr));
        return 0;
    }

    uint32_t added = 0;
    while (sd_rom_list_count < MAX_SD_ROM_LIST) {
        fr = f_readdir(&dir, &info);
        if ((fr != FR_OK) || (info.fname[0] == '\0')) {
            break;
        }

        if (info.fattrib & AM_DIR) {
            continue;
        }

        const char *name = (const char *)info.fname;
#if defined(FF_USE_LFN) && (FF_USE_LFN != 0)
        if ((name == NULL || name[0] == '\0') && info.altname[0] != '\0') {
            name = (const char *)info.altname;
        }
#endif
        if ((name == NULL) || !filename_is_rom(name)) {
            continue;
        }

        if (add_rom_to_list(directory, name)) {
            added++;
        }
    }

    FRESULT close_result = f_closedir(&dir);
    if (close_result != FR_OK) {
        printf("ROM scan: close failed (%d: %s)\n", close_result, FRESULT_str(close_result));
    }
    return added;
}

static bool build_sd_rom_list(void)
{
    if (!sd_filesystem_ready) {
        return false;
    }

    clear_sd_rom_list();

    const char *search_paths[] = {
        "0:/ROMS",
        "0:/"
    };

    for (size_t i = 0; i < sizeof(search_paths)/sizeof(search_paths[0]); ++i) 
    {
        scan_directory_for_roms(search_paths[i]);
        if (sd_rom_list_count >= MAX_SD_ROM_LIST) {
            break;
        }
    }

    if (sd_rom_list_count > 0) {
        strncpy(sd_rom_path, sd_rom_list[0], sizeof(sd_rom_path) - 1);
        sd_rom_path[sizeof(sd_rom_path) - 1] = '\0';
        sd_rom_discovered = true;
        return true;
    }

    sd_rom_discovered = false;
    sd_rom_path[0] = '\0';
    return false;
}

static const char *path_basename(const char *path)
{
    if (path == NULL) {
        return "";
    }

    const char *slash_fwd = strrchr(path, '/');
    const char *slash_back = strrchr(path, '\\');

    const char *base = NULL;
    if (slash_fwd != NULL) {
        base = slash_fwd;
    }
    if (slash_back != NULL && (slash_back > base)) {
        base = slash_back;
    }

    if (base == NULL) {
        return path;
    }
    return base + 1;
}

// static void print_sd_rom_list(void)
// {
//     if (sd_rom_list_count == 0) {
//         printf("<no SD ROMs found>\n");
//         return;
//     }
//
//     printf("SD ROMs (%lu found, showing up to %u):\n", (unsigned long)sd_rom_list_count, (unsigned int)MAX_SD_ROM_LIST);
//     for (uint32_t i = 0; i < sd_rom_list_count; ++i) {
//         printf("  [%02lu] %s\n", (unsigned long)i, sd_rom_list[i]);
//     }
// }





static inline void set_pixel_2bpp(uint8_t *buf, int x, int y, uint8_t color)
{
    if ((buf == NULL) || (x < 0) || (x >= DMG_PIXELS_X) || (y < 0) || (y >= DMG_PIXELS_Y)) {
        return;
    }

    size_t idx = (size_t)y * PACKED_LINE_STRIDE_BYTES + (size_t)(x >> 2);
    uint shift = (3u - (uint)(x & 3)) * 2u;
    uint8_t mask = (uint8_t)(0x03u << shift);
    buf[idx] = (uint8_t)((buf[idx] & ~mask) | ((color & 0x03u) << shift));
}

static void fill_buffer_2bpp(uint8_t *buf, uint8_t color)
{
    if (buf == NULL) {
        return;
    }

    uint8_t packed = (uint8_t)(((color & 0x03u) << 6) | ((color & 0x03u) << 4) | ((color & 0x03u) << 2) | (color & 0x03u));
    memset(buf, packed, PACKED_FRAME_SIZE);
}

static void draw_glyph_5x7(uint8_t *buf, int x, int y, const glyph_5x7_t *glyph, uint8_t fg, uint8_t bg)
{
    if ((buf == NULL) || (glyph == NULL)) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        uint8_t bits = glyph->rows[row];
        for (int col = 0; col < 5; ++col) {
            const bool on = (bits & (0x10 >> col)) != 0;
            set_pixel_2bpp(buf, x + col, y + row, on ? fg : bg);
        }
        set_pixel_2bpp(buf, x + 5, y + row, bg); // 1px spacing
    }
}

static void draw_text_line(uint8_t *buf, int x, int y, const char *text, uint8_t fg, uint8_t bg)
{
    if ((buf == NULL) || (text == NULL)) {
        return;
    }

    int cursor_x = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (cursor_x >= DMG_PIXELS_X) {
            break;
        }
            const glyph_5x7_t *glyph = font5x7_lookup(text[i]);
        draw_glyph_5x7(buf, cursor_x, y, glyph, fg, bg);
        cursor_x += 6; // 5 pixels plus spacing
    }
}

static void format_rom_label(const char *path, char *out, size_t out_len)
{
    if ((out == NULL) || (out_len == 0)) {
        return;
    }

    out[0] = '\0';
    if (path == NULL) {
        return;
    }

    const char *base = path_basename(path);
    char scratch[64];
    strncpy(scratch, base, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';

    char *dot = strrchr(scratch, '.');
    if (dot != NULL) {
        *dot = '\0';
    }

    size_t o = 0;
    for (size_t i = 0; scratch[i] != '\0' && o < (out_len - 1) && o < (MENU_MAX_LABEL_CHARS - 1); ++i) {
        char c = (char)toupper((unsigned char)scratch[i]);
        bool allowed = ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' ||
            c == '(' || c == ')' || c == '\'' || c == ',' || c == '!' ||
            c == '[' || c == ']');
        if (!allowed) {
            c = ' ';
        }
        out[o++] = c;
    }

    if (o == 0) {
        strncpy(out, "<BLANK>", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    out[o] = '\0';
}

static void render_rom_menu(uint32_t selected_index)
{
    uint8_t *buf = packed_render_ptr;
    fill_buffer_2bpp(buf, MENU_COLOR_BG);

    draw_text_line(buf, 4, 4, "SELECT ROM", MENU_COLOR_TITLE, MENU_COLOR_BG);

    const uint32_t visible_rows = (DMG_PIXELS_Y - 28) / MENU_LINE_HEIGHT; // currently 14 rows
    const uint32_t page = (selected_index / visible_rows);
    uint32_t page_start = page * visible_rows;

    for (uint32_t row = 0; row < visible_rows && (page_start + row) < sd_rom_list_count; ++row) {
        uint32_t idx = page_start + row;
        char label[MENU_MAX_LABEL_CHARS];
        format_rom_label(sd_rom_list[idx], label, sizeof(label));
        const bool selected = (idx == selected_index);
        uint8_t fg = selected ? MENU_COLOR_HL_FG : MENU_COLOR_FG;
        uint8_t bg = selected ? MENU_COLOR_HL_BG : MENU_COLOR_BG;
        draw_text_line(buf, 8, 16 + (int)row * MENU_LINE_HEIGHT, label, fg, bg);
    }

    draw_text_line(buf, 4, DMG_PIXELS_Y - 12, "L/R=PAGE  A/START=LOAD", MENU_COLOR_FG, MENU_COLOR_BG);

    // Overlay OSD text, if enabled
    OSD_render(buf);

    swap_display_buffers();
}

static bool sd_rom_selection_menu(char *selected_path, size_t selected_len)
{
    if ((selected_path == NULL) || (selected_len == 0) || (sd_rom_list_count == 0)) {
        return false;
    }

    uint32_t index = 0;
    // absolute_time_t auto_select_deadline = make_timeout_time_ms(4000);
    render_rom_menu(index);

    while (true) {
        game_controller();

        bool redraw = false;
        bool needs_render = false;

        // If a hotkey command happened, don't check buttons here
        if (!command_check())
        {
            if (button_was_released(BUTTON_UP)) {
                index = (index == 0) ? (sd_rom_list_count - 1) : (index - 1);
                redraw = true;
            }
            if (button_was_released(BUTTON_DOWN)) {
                index = (index + 1u) % sd_rom_list_count;
                redraw = true;
            }
            if (button_was_released(BUTTON_LEFT)) {
                uint32_t visible_rows = (DMG_PIXELS_Y - 28) / MENU_LINE_HEIGHT;
                uint32_t page = index / visible_rows;
                if (page == 0) {
                    // wrap to last page
                    uint32_t last_page = (sd_rom_list_count - 1) / visible_rows;
                    index = last_page * visible_rows;
                } else {
                    index = (page - 1) * visible_rows;
                }
                if (index >= sd_rom_list_count) {
                    index = sd_rom_list_count - 1;
                }
                redraw = true;
            }
            if (button_was_released(BUTTON_RIGHT)) {
                uint32_t visible_rows = (DMG_PIXELS_Y - 28) / MENU_LINE_HEIGHT;
                uint32_t page = index / visible_rows;
                uint32_t last_page = (sd_rom_list_count - 1) / visible_rows;
                if (page >= last_page) {
                    index = 0;
                } else {
                    index = (page + 1) * visible_rows;
                }
                if (index >= sd_rom_list_count) {
                    index = sd_rom_list_count - 1;
                }
                redraw = true;
            }
            needs_render = redraw;

            bool confirm = false;
            if (button_was_released(BUTTON_A)) {
                confirm = true;
            }
            if (button_was_released(BUTTON_START)) {
                confirm = true;
            }
            if (confirm) {
                strncpy(selected_path, sd_rom_list[index], selected_len - 1);
                selected_path[selected_len - 1] = '\0';
                render_rom_menu(index);
                return true;
            }

            if (button_was_released(BUTTON_HOME)) {
                return false;
            }
        }
        else
        {
            // Hotkey command changed OSD state; redraw to reflect updates.
            needs_render = true;
        }

        button_state_save_previous();
        
        if (OSD_is_enabled()) {
            update_osd();
            needs_render = true;
        }

        if (needs_render) {
            render_rom_menu(index);
        }



        // if (time_reached(auto_select_deadline)) {
        //     strncpy(selected_path, sd_rom_list[index], selected_len - 1);
        //     selected_path[selected_len - 1] = '\0';
        //     render_rom_menu(index);
        //     return true;
        // }

        sleep_ms(40);
    }
}

static void boot_checkpoint(const char *label)
{
    if (label == NULL) {
        return;
    }

    printf("[BOOT] %s\n", label);
}

static void invalidate_sd_rom_cache(void)
{
    for (uint32_t i = 0; i < SD_ROM_CACHE_SLOTS; ++i) {
        sd_rom_cache[i].valid = false;
        sd_rom_cache[i].bank_index = 0;
        sd_rom_cache[i].bytes_valid = 0;
    }
    sd_rom_stream.next_replace_slot = 0;
}

static void close_sd_rom_stream(void)
{
    if (sd_rom_stream.open) {
        f_close(&sd_rom_stream.handle);
        sd_rom_stream.open = false;
    }

    sd_rom_stream.size_bytes = 0;
    sd_rom_stream.bank_count = 0;
    invalidate_sd_rom_cache();
}

static void free_sd_rom_heap(void)
{
    if (sd_rom_heap != NULL) {
        free(sd_rom_heap);
        sd_rom_heap = NULL;
        sd_rom_heap_size = 0;
    }
}

static size_t estimate_free_heap_bytes(void)
{
    extern char __StackLimit;
    void *current_break = sbrk(0);
    if (current_break == (void *)-1 || current_break == NULL) {
        return 0;
    }

    uintptr_t stack_limit = (uintptr_t)&__StackLimit;
    uintptr_t heap_end = (uintptr_t)current_break;
    if (heap_end >= stack_limit) {
        return 0;
    }

    return (size_t)(stack_limit - heap_end);
}

static void log_free_heap(const char *tag)
{
#if ENABLE_HEAP_LOG
    size_t free_bytes = estimate_free_heap_bytes();
    printf("[MEM] %s free ~= %lu bytes\n", (tag != NULL) ? tag : "heap", (unsigned long)free_bytes);
#else
    (void)tag;
#endif
}

static bool ensure_audio_ready(void)
{
#if ENABLE_AUDIO
    static uint32_t last_check_ms = 0;
    static bool audio_started = false;
    static bool deadline_initialized = false;
    static absolute_time_t audio_guard_deadline;

    if (audio_started) {
        return true;
    }

    if (!deadline_initialized) {
        audio_guard_deadline = delayed_by_ms(get_absolute_time(), 30000); // guard only first 10s
        deadline_initialized = true;
    }

    // After the guard window, stop intervening to avoid runtime churn.
    if (absolute_time_diff_us(get_absolute_time(), audio_guard_deadline) <= 0) {
        audio_started = true;
        return true;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    // Check at most every 50 ms to avoid extra work in the hot path.
    if ((last_check_ms == 0) || ((now_ms - last_check_ms) >= 50u)) {
        const size_t low_watermark = AUDIO_BUFFER_SIZE / 10; // minimal guard against underrun
        const size_t lock_watermark = AUDIO_BUFFER_SIZE / 3; // once above this, consider audio stable
        size_t read_size = get_read_size(&dvi0.audio_ring, false);

        if (read_size >= lock_watermark) {
            audio_started = true;
            return true;
        }

        if (read_size < low_watermark) {
            // Only top up to smooth dips; avoid pointer resets that cause audible artifacts.
            for (int i = 0; i < 8; ++i) {
                pump_audio_samples();
            }
            read_size = get_read_size(&dvi0.audio_ring, false);
            if (read_size >= lock_watermark) {
                audio_started = true;
                return true;
            }
        }

        last_check_ms = now_ms;
    }

    return audio_started;
#endif
    return true;
}

static void reset_audio_ring_prefill(size_t fill_samples)
{
#if ENABLE_AUDIO
    const size_t max_fill = AUDIO_BUFFER_SIZE - 1; // leave one slot empty for ring logic
    size_t target_fill = (fill_samples > max_fill) ? max_fill : fill_samples;

    audio_sample_residual = 0;
    memset(audio_buffer, 0, sizeof(audio_buffer));
    set_read_offset(&dvi0.audio_ring, 0);
    set_write_offset(&dvi0.audio_ring, 0);
    set_write_offset(&dvi0.audio_ring, (uint32_t)target_fill);

    dvi0.audio_sample_pos = 0;
    dvi0.left_audio_sample_count = 0;
    dvi0.audio_frame_count = 0;
#else
    (void)fill_samples;
#endif
}

static void restart_audio_pipeline(void)
{
#if ENABLE_AUDIO
    printf("[AUDIO] restart requested: resetting ring and refilling.\n");
    reset_audio_ring_prefill(AUDIO_BUFFER_SIZE - 8);
    for (int i = 0; i < 16; ++i) {
        pump_audio_samples();
    }
    sleep_ms(2);
#endif
}

static bool sd_stream_load_bank(uint32_t bank_index, sd_rom_cache_slot_t *slot)
{
    if (!sd_rom_stream.open) {
        return false;
    }

    if (bank_index >= sd_rom_stream.bank_count) {
        return false;
    }

    size_t offset = (size_t)bank_index * ROM_BANK_SIZE;
    FRESULT fr = f_lseek(&sd_rom_stream.handle, (FSIZE_t)offset);
    if (fr != FR_OK) {
        printf("SD ROM cache seek failed (bank=%lu): %s (%d)\n",
               (unsigned long)bank_index, FRESULT_str(fr), fr);
        close_sd_rom_stream();
        return false;
    }

    size_t bytes_to_read = sd_rom_stream.size_bytes - offset;
    if (bytes_to_read > ROM_BANK_SIZE) {
        bytes_to_read = ROM_BANK_SIZE;
    }

    uint8_t *dst = slot->data;
    size_t remaining = bytes_to_read;
    int attempt = 0;
    while (remaining > 0) {
        size_t chunk = (remaining > SD_STREAM_CHUNK_BYTES) ? SD_STREAM_CHUNK_BYTES : remaining;
        UINT chunk_read = 0;
        fr = f_read(&sd_rom_stream.handle, dst, (UINT)chunk, &chunk_read);
        if ((fr != FR_OK) || (chunk_read != chunk)) {
            if (attempt == 0) {
                // Retry once on short/failed read
                attempt++;
                printf("SD ROM cache read retry (bank=%lu chunk=%u): %s (%d) read=%u\n",
                       (unsigned long)bank_index, (unsigned int)chunk, FRESULT_str(fr), fr, (unsigned int)chunk_read);
                continue;
            }
            printf("SD ROM cache read failed (bank=%lu chunk=%u): %s (%d) read=%u\n",
                   (unsigned long)bank_index, (unsigned int)chunk, FRESULT_str(fr), fr, (unsigned int)chunk_read);
            close_sd_rom_stream();
            return false;
        }

        dst += chunk_read;
        remaining -= chunk_read;

        if (remaining > 0) {
            sd_stream_chunk_yield();
        }
    }

    if (bytes_to_read < ROM_BANK_SIZE) {
        memset(slot->data + bytes_to_read, 0xFF, ROM_BANK_SIZE - bytes_to_read);
    }

    slot->bank_index = bank_index;
    slot->bytes_valid = bytes_to_read;
    slot->valid = true;
    return true;
}

static uint8_t sd_stream_read_byte(size_t addr)
{
    if (!sd_rom_stream.open) {
        return 0xFF;
    }

    if (addr >= sd_rom_stream.size_bytes) {
        return 0xFF;
    }

    uint32_t bank_index = (uint32_t)(addr / ROM_BANK_SIZE);
    uint32_t bank_offset = (uint32_t)(addr % ROM_BANK_SIZE);

    for (uint32_t i = 0; i < SD_ROM_CACHE_SLOTS; ++i) {
        sd_rom_cache_slot_t *slot = &sd_rom_cache[i];
        if (slot->valid && slot->bank_index == bank_index) {
            if (bank_offset >= slot->bytes_valid) {
                return 0xFF;
            }
#if ENABLE_SD_STATS_LOG
            sd_cache_hits++;
#endif
            return slot->data[bank_offset];
        }
    }

#if ENABLE_SD_STATS_LOG
    sd_cache_misses++;
#endif

    sd_rom_cache_slot_t *slot = &sd_rom_cache[sd_rom_stream.next_replace_slot];
    if (!sd_stream_load_bank(bank_index, slot)) {
        printf("SD stream load failed for bank=%lu addr=%lu\n", (unsigned long)bank_index, (unsigned long)addr);
        return 0xFF;
    }
    sd_rom_stream.next_replace_slot = (sd_rom_stream.next_replace_slot + 1u) % SD_ROM_CACHE_SLOTS;

    if (bank_offset >= slot->bytes_valid) {
        printf("SD stream bank offset beyond valid data (bank=%lu offset=%lu valid=%lu)\n",
               (unsigned long)bank_index, (unsigned long)bank_offset, (unsigned long)slot->bytes_valid);
        return 0xFF;
    }
    return slot->data[bank_offset];
}

// static bool discover_sd_rom(void)
// {
//     if (!sd_filesystem_ready) {
//         return false;
//     }

//     dump_sd_rom_inventory();

//     const char *search_paths[] = {
//         "0:/ROMS",
//         "0:/"
//     };

//     for (size_t i = 0; i < sizeof(search_paths)/sizeof(search_paths[0]); ++i) {
//         if (find_first_rom_in_directory(search_paths[i], sd_rom_path, sizeof(sd_rom_path))) {
//             printf("Found SD ROM: %s\n", sd_rom_path);
//             sd_rom_discovered = true;
//             boot_checkpoint("discover_sd_rom about to return true");
//             return true;
//         }
//     }

//     printf("No .gb/.gbc files found on SD card (checked /ROMS and root).\n");
//     sd_rom_discovered = false;
//     sd_rom_path[0] = '\0';
//     return false;
// }

static void reset_active_rom_to_builtin(void)
{
    close_sd_rom_stream();
    active_rom_data = ACTIVE_ROM_DATA;
    active_rom_length = ACTIVE_ROM_LEN;
    active_rom_source = ROM_SOURCE_BUILTIN;
    gb_faulted = false;
    close_sd_rom_stream();
    free_sd_rom_heap();
}

static bool load_sd_rom_file(const char *path)
{
    boot_checkpoint("load_sd_rom_file entry");
    if (!sd_filesystem_ready || path == NULL || path[0] == '\0') {
        printf("SD ROM load skipped: filesystem not ready or path missing\n");
        return false;
    }

    close_sd_rom_stream();
    free_sd_rom_heap();

    printf("SD ROM load: opening %s\n", path);

    FIL temp_file;
    FRESULT fr = f_open(&temp_file, path, FA_READ);
    if (fr != FR_OK) {
        printf("SD ROM load failed to open %s (%d: %s)\n", path, fr, FRESULT_str(fr));
        return false;
    }
    boot_checkpoint("SD ROM file opened");

    printf("[TRACE] FIL starting cluster=%lu, objsize=%llu\n",
           (unsigned long)temp_file.obj.sclust,
           (unsigned long long)temp_file.obj.objsize);
    FSIZE_t file_size = f_size(&temp_file);
    printf("[TRACE] f_size returned %lu\n", (unsigned long)file_size);
    f_close(&temp_file);

    fr = f_open(&sd_rom_stream.handle, path, FA_READ);
    if (fr != FR_OK) {
        printf("SD ROM load failed to reopen %s (%d: %s)\n", path, fr, FRESULT_str(fr));
        return false;
    }

    sd_rom_stream.open = true;
    sd_rom_stream.size_bytes = (size_t)file_size;
    boot_checkpoint("SD ROM file size read");

    if ((sd_rom_stream.size_bytes == 0) || (sd_rom_stream.size_bytes > MAX_SD_ROM_FILE_BYTES)) {
        printf("SD ROM load aborted: size %lu bytes (limit %u)\n",
               (unsigned long)sd_rom_stream.size_bytes,
               (unsigned int)MAX_SD_ROM_FILE_BYTES);
        close_sd_rom_stream();
        return false;
    }

    size_t rom_size_bytes = sd_rom_stream.size_bytes;

#if ENABLE_SD_HEAP_LOAD
    size_t approx_free_heap = estimate_free_heap_bytes();
    bool rom_within_heap_limit = (rom_size_bytes <= MAX_SD_ROM_HEAP_BYTES);
    size_t required_with_margin = rom_size_bytes + SD_HEAP_SAFETY_MARGIN_BYTES;

    if (!rom_within_heap_limit) {
        printf("SD ROM heap load skipped: %lu bytes exceeds limit (%u)\n",
               (unsigned long)rom_size_bytes, (unsigned int)MAX_SD_ROM_HEAP_BYTES);
    }

    if (rom_within_heap_limit) {
        printf("Heap try: need ~%lu (incl. margin), free ~%lu\n",
               (unsigned long)required_with_margin, (unsigned long)approx_free_heap);
        if (required_with_margin > approx_free_heap) {
            printf("Heap load skipped: insufficient headroom, will stream from SD.\n");
        } else {
        boot_checkpoint("Attempting heap load");
        uint8_t *heap_buffer = (uint8_t *)malloc(rom_size_bytes);
        if (heap_buffer != NULL) {
            boot_checkpoint("Heap buffer allocated");
            size_t total_read = 0;
            while (total_read < rom_size_bytes) {
                UINT chunk_read = 0;
                size_t remaining = rom_size_bytes - total_read;
                size_t to_request = (remaining > 4096) ? 4096 : remaining;
                FRESULT heap_read = f_read(&sd_rom_stream.handle,
                                           heap_buffer + total_read,
                                           (UINT)to_request,
                                           &chunk_read);
                if ((heap_read != FR_OK) || (chunk_read == 0)) {
                    printf("SD ROM heap read error (%d: %s) after %u bytes\n",
                           heap_read, FRESULT_str(heap_read), (unsigned int)total_read);
                    free(heap_buffer);
                    heap_buffer = NULL;
                    break;
                }
                total_read += chunk_read;
            }

            if (heap_buffer != NULL && total_read == rom_size_bytes) {
                boot_checkpoint("SD ROM heap copy complete");
                sd_rom_heap = heap_buffer;
                sd_rom_heap_size = rom_size_bytes;
                size_t copy_len = (rom_size_bytes < sizeof(rom_bank0)) ? rom_size_bytes : sizeof(rom_bank0);
                memcpy(rom_bank0, sd_rom_heap, copy_len);
                if (copy_len < sizeof(rom_bank0)) {
                    memset(rom_bank0 + copy_len, 0xFF, sizeof(rom_bank0) - copy_len);
                }
                close_sd_rom_stream();
                active_rom_source = ROM_SOURCE_SD_HEAP;
                active_rom_data = sd_rom_heap;
                active_rom_length = rom_size_bytes;
                gb_faulted = false;
                printf("Loaded SD ROM into heap (%lu bytes)\n", (unsigned long)rom_size_bytes);
                return true;
            }
        } else {
            printf("SD ROM heap alloc failed for %lu bytes - falling back to streaming\n",
                   (unsigned long)rom_size_bytes);
        }

        if (sd_rom_heap == NULL) {
            boot_checkpoint("Heap path failed; rewinding file");
            free_sd_rom_heap();
            FRESULT rewind_res = f_lseek(&sd_rom_stream.handle, 0);
            if (rewind_res != FR_OK) {
                printf("SD ROM load failed while rewinding after heap attempt (%d: %s)\n",
                       rewind_res, FRESULT_str(rewind_res));
                close_sd_rom_stream();
                return false;
            }
        }
        }
    }
#else
    printf("SD ROM heap load disabled (streaming from SD to save RAM).\n");
#endif

    sd_rom_stream.bank_count = (uint32_t)((sd_rom_stream.size_bytes + ROM_BANK_SIZE - 1u) / ROM_BANK_SIZE);
    if (sd_rom_stream.bank_count == 0) {
        printf("SD ROM load aborted: unable to determine bank count\n");
        close_sd_rom_stream();
        return false;
    }
    boot_checkpoint("SD ROM stream initialized");

    printf("Streaming SD ROM (%lu bytes) with %u banks; cache slots=%u\n",
           (unsigned long)sd_rom_stream.size_bytes,
           (unsigned int)sd_rom_stream.bank_count,
           (unsigned int)SD_ROM_CACHE_SLOTS);

    invalidate_sd_rom_cache();
    FRESULT seek_res = f_lseek(&sd_rom_stream.handle, 0);
    if (seek_res != FR_OK) {
        printf("SD ROM load failed while seeking to start (%d: %s)\n", seek_res, FRESULT_str(seek_res));
        close_sd_rom_stream();
        return false;
    }

    size_t rom0_bytes = (sd_rom_stream.size_bytes < sizeof(rom_bank0)) ? sd_rom_stream.size_bytes : sizeof(rom_bank0);
    UINT bytes_read = 0;
    FRESULT read_res = f_read(&sd_rom_stream.handle, rom_bank0, (UINT)rom0_bytes, &bytes_read);
    if ((read_res != FR_OK) || (bytes_read != rom0_bytes)) {
        printf("SD ROM load failed while reading bank 0 (%d: %s)\n", read_res, FRESULT_str(read_res));
        close_sd_rom_stream();
        return false;
    }

    printf("SD ROM load: copied %u bytes into bank 0\n", (unsigned int)bytes_read);

    char title_buf[17];
    for (size_t i = 0; i < sizeof(title_buf) - 1; ++i) {
        uint8_t c = rom_bank0[0x134 + i];
        if ((c < 0x20) || (c > 0x7E)) {
            c = '.';
        }
        title_buf[i] = (char)c;
    }
    title_buf[16] = '\0';

    uint8_t cart_type = rom_bank0[0x147];
    uint8_t rom_code = rom_bank0[0x148];
    uint8_t ram_code = rom_bank0[0x149];
    uint8_t version = rom_bank0[0x14C];
    printf("ROM header: title=\"%s\" cart=0x%02x rom_code=0x%02x ram_code=0x%02x version=0x%02x\n",
           title_buf, cart_type, rom_code, ram_code, version);

    if (rom0_bytes < sizeof(rom_bank0)) {
        memset(rom_bank0 + rom0_bytes, 0xFF, sizeof(rom_bank0) - rom0_bytes);
    }

    active_rom_source = ROM_SOURCE_SD_STREAM;
    active_rom_data = NULL;
    active_rom_length = sd_rom_stream.size_bytes;
    gb_faulted = false;

    // printf("Streaming SD ROM (%lu bytes) from %s\n", (unsigned long)sd_rom_stream.size_bytes, path);

    // uint8_t probe = sd_stream_read_byte(0x4000);
    // printf("[SD] Bank1 probe = 0x%02x\n", probe);
    /* temporarily disable USB flush here */

    boot_checkpoint("load_sd_rom_file about to return true");
    return true;
}

static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline)
{
    const bool in_active_window =
        (scanline >= VERTICAL_OFFSET) && (scanline < (DMG_PIXELS_Y + VERTICAL_OFFSET));

    const uint8_t* packed_fb = (const uint8_t*)packed_display_ptr;
    const uint32_t *bufptr = NULL;
    if (in_active_window && (packed_fb != NULL))
    {
        uint dmg_line_idx = scanline - VERTICAL_OFFSET;
        const uint8_t* packed_line = packed_fb + (dmg_line_idx * DMG_PIXELS_X / 4);  // 40 bytes per line
        memcpy(line_buffer, packed_line, sizeof(line_buffer));  // Copy 40 bytes
        bufptr = (uint32_t*)line_buffer;
    }

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
    if (active_rom_source == ROM_SOURCE_SD_STREAM) {
        return sd_stream_read_byte((size_t)addr);
    }
    if ((active_rom_data != NULL) && (addr < active_rom_length)) {
        return active_rom_data[addr];
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
    gb_fault_info.code = gb_err;
    gb_fault_info.val = val;
    gb_fault_info.pc = gb->cpu_reg.pc.reg;
    gb_fault_info.sp = gb->cpu_reg.sp.reg;
    gb_fault_info.rom_bank = gb->selected_rom_bank;
    gb_faulted = true;
    printf("Peanut-GB error %d val=0x%04x pc=0x%04x sp=0x%04x rom_bank=%u source=%d\n",
           gb_err,
           (unsigned int)val,
           (unsigned int)gb_fault_info.pc,
           (unsigned int)gb_fault_info.sp,
           (unsigned int)gb_fault_info.rom_bank,
           (int)active_rom_source);
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
    if (OSD_is_enabled()) 
    {
        // When OSD is active, block emulator inputs to avoid accidental gameplay actions
        gb.direct.joypad = 0xFF;
        return;
    }
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
    if (active_rom_source == ROM_SOURCE_BUILTIN || active_rom_source == ROM_SOURCE_SD_HEAP) {
        size_t rom0_bytes = (active_rom_length < sizeof(rom_bank0)) ? active_rom_length : sizeof(rom_bank0);
        if ((active_rom_data != NULL) && (rom0_bytes > 0)) {
            memcpy(rom_bank0, active_rom_data, rom0_bytes);
        }
        if (rom0_bytes < sizeof(rom_bank0)) {
            memset(rom_bank0 + rom0_bytes, 0xFF, sizeof(rom_bank0) - rom0_bytes);
        }
    } else if (!sd_rom_stream.open) {
        memset(rom_bank0, 0xFF, sizeof(rom_bank0));
    }

    const char *rom_source_label = "builtin";
    if (active_rom_source == ROM_SOURCE_SD_STREAM) {
        rom_source_label = "SD-stream";
    } else if (active_rom_source == ROM_SOURCE_SD_HEAP) {
        rom_source_label = "SD-heap";
    }
    printf("ROM[0..3] src=%s = %02x %02x %02x %02x (len=%u)\n",
        rom_source_label,
        rom_bank0[0],
        rom_bank0[1],
        rom_bank0[2],
        rom_bank0[3],
        (unsigned int)active_rom_length);

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
        if (gb_faulted) {
            return;
        }
    } while (gb.gb_frame == 0);
}

static void initialize_gpio(void)
{    
    //Debug LED
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

#if USE_BLUETOOTH_CONTROLLER
    // Pair button is read by Bluetooth host helper for startup/long-press re-pair.
    gpio_init(BT_PAIR_BUTTON_GPIO);
    gpio_set_dir(BT_PAIR_BUTTON_GPIO, GPIO_IN);
    gpio_pull_up(BT_PAIR_BUTTON_GPIO);
#elif USE_NES_CLASSIC_CONTROLLER
    //Initialize I2C port at 400 kHz
    i2c_init(i2cHandle, 400 * 1000);

    // Initialize I2C pins
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SCL);
    gpio_pull_up(PIN_SDA);
#else
    /* Clock, normally HIGH */
    gpio_init(PIN_NES_PULSE);
    gpio_set_dir(PIN_NES_PULSE, GPIO_OUT);
    gpio_put(PIN_NES_PULSE, 1);

    /* Latch, normally LOW */
    gpio_init(PIN_NES_LATCH);
    gpio_set_dir(PIN_NES_LATCH, GPIO_OUT);
    gpio_put(PIN_NES_LATCH, 0);

    /* Data, reads normally high */
    gpio_init(PIN_NES_DATA);
    gpio_set_dir(PIN_NES_DATA, GPIO_IN);
    // Optionally add pullup
    gpio_pull_up(PIN_NES_DATA);
#endif
}

#if USE_BLUETOOTH_CONTROLLER
static uint8_t bt_config_checksum(const uint8_t remote_addr[6], uint8_t controller_type)
{
    uint8_t checksum = 0x5Au;

    for (uint8_t i = 0; i < 6u; ++i) {
        checksum ^= remote_addr[i];
        checksum = (uint8_t)((checksum << 1) | (checksum >> 7));
    }

    checksum ^= controller_type;
    return checksum;
}

static bool bt_address_is_unset(const uint8_t remote_addr[6])
{
    for (uint8_t i = 0; i < 6u; ++i) {
        if (remote_addr[i] != 0u) {
            return false;
        }
    }

    return true;
}

static bool bt_controller_type_supported(uint8_t controller_type)
{
    switch (controller_type) {
        case BT_HOST_CONTROLLER_GENERIC:
        case BT_HOST_CONTROLLER_PS4:
        case BT_HOST_CONTROLLER_EIGHT_BITDO:
            return true;
        default:
            return false;
    }
}

static bool bt_load_paired_controller(bt_host_config_t *config)
{
    uint8_t magic = 0;
    uint8_t version = 0;
    uint8_t controller_type = 0;
    uint8_t remote_addr[6] = {0};
    uint8_t checksum = 0;

    if (config == NULL) {
        return false;
    }

    if (EEPROM_read(BT_CFG_INDEX_MAGIC, &magic) != EEPROM_SUCCESS ||
        EEPROM_read(BT_CFG_INDEX_VERSION, &version) != EEPROM_SUCCESS ||
        EEPROM_read(BT_CFG_INDEX_CONTROLLER_TYPE, &controller_type) != EEPROM_SUCCESS ||
        EEPROM_read(BT_CFG_INDEX_CHECKSUM, &checksum) != EEPROM_SUCCESS) {
        return false;
    }

    for (uint8_t i = 0; i < 6u; ++i) {
        if (EEPROM_read((uint8_t)(BT_CFG_INDEX_REMOTE_ADDR + i), &remote_addr[i]) != EEPROM_SUCCESS) {
            return false;
        }
    }

    if (magic != BT_CFG_MAGIC || version != BT_CFG_VERSION) {
        return false;
    }

    if (!bt_controller_type_supported(controller_type)) {
        return false;
    }

    if (bt_address_is_unset(remote_addr)) {
        return false;
    }

    if (checksum != bt_config_checksum(remote_addr, controller_type)) {
        return false;
    }

    memcpy(config->remote_addr, remote_addr, sizeof(config->remote_addr));
    config->controller_type = (bt_host_controller_type_t)controller_type;
    return true;
}

static bool bt_save_paired_controller(const bt_host_config_t *config)
{
    uint8_t controller_type;
    uint8_t checksum;

    if (config == NULL) {
        return false;
    }

    controller_type = (uint8_t)config->controller_type;
    checksum = bt_config_checksum(config->remote_addr, controller_type);

    if (!bt_controller_type_supported(controller_type)) {
        return false;
    }

    if (bt_address_is_unset(config->remote_addr)) {
        return false;
    }

    if (EEPROM_write(BT_CFG_INDEX_MAGIC, BT_CFG_MAGIC) != EEPROM_SUCCESS ||
        EEPROM_write(BT_CFG_INDEX_VERSION, BT_CFG_VERSION) != EEPROM_SUCCESS ||
        EEPROM_write(BT_CFG_INDEX_CONTROLLER_TYPE, controller_type) != EEPROM_SUCCESS) {
        return false;
    }

    for (uint8_t i = 0; i < 6u; ++i) {
        if (EEPROM_write((uint8_t)(BT_CFG_INDEX_REMOTE_ADDR + i), config->remote_addr[i]) != EEPROM_SUCCESS) {
            return false;
        }
    }

    if (EEPROM_write(BT_CFG_INDEX_CHECKSUM, checksum) != EEPROM_SUCCESS) {
        return false;
    }

    return EEPROM_commit() == EEPROM_SUCCESS;
}

static void bt_clear_paired_controller(void)
{
    for (uint8_t i = 0; i < 6u; ++i) {
        (void)EEPROM_write((uint8_t)(BT_CFG_INDEX_REMOTE_ADDR + i), 0u);
    }

    (void)EEPROM_write(BT_CFG_INDEX_CONTROLLER_TYPE, (uint8_t)BT_HOST_CONTROLLER_GENERIC);
    (void)EEPROM_write(BT_CFG_INDEX_MAGIC, 0u);
    (void)EEPROM_write(BT_CFG_INDEX_VERSION, 0u);
    (void)EEPROM_write(BT_CFG_INDEX_CHECKSUM, 0u);
    (void)EEPROM_commit();
}

static void bluetooth_report_callback(const bt_host_gamepad_report_t *report)
{
    if (report == NULL) {
        return;
    }

    bt_latest_report = *report;
    bt_report_valid = true;
}

static void bluetooth_pairing_complete_callback(const uint8_t remote_addr[6])
{
    if (remote_addr == NULL) {
        return;
    }

    memcpy(bt_runtime_config.remote_addr, remote_addr, sizeof(bt_runtime_config.remote_addr));

    printf("Bluetooth paired: %02X:%02X:%02X:%02X:%02X:%02X\n",
           remote_addr[0],
           remote_addr[1],
           remote_addr[2],
           remote_addr[3],
           remote_addr[4],
           remote_addr[5]);

    if (bt_save_paired_controller(&bt_runtime_config)) {
        printf("Saved paired controller to EEPROM\n");
    } else {
        printf("Failed to save paired controller to EEPROM\n");
    }
}

static void bluetooth_pairing_reset_callback(void)
{
    memset(bt_runtime_config.remote_addr, 0, sizeof(bt_runtime_config.remote_addr));
    bt_clear_paired_controller();
    bt_report_valid = false;
    reset_button_states();
    printf("Bluetooth pairing reset requested (saved pairing cleared)\n");
}

static bool init_bluetooth_controller(void)
{
    bool pair_button_held;
    bool has_saved_controller;

    memset(&bt_runtime_config, 0, sizeof(bt_runtime_config));
    bt_runtime_config.controller_type = BT_HOST_CONTROLLER_GENERIC;
    bt_runtime_config.pairing_timeout_ms = BT_PAIRING_TIMEOUT_MS;
    bt_runtime_config.pair_button_gpio = BT_PAIR_BUTTON_GPIO;
    bt_runtime_config.pair_button_active_low = true;
    bt_runtime_config.pair_button_long_press_ms = BT_PAIR_LONG_PRESS_MS;

    sleep_ms(20);
    pair_button_held = gpio_get(BT_PAIR_BUTTON_GPIO) == 0;
    has_saved_controller = bt_load_paired_controller(&bt_runtime_config);

    if (pair_button_held || !has_saved_controller) {
        bt_runtime_config.start_mode = BT_HOST_START_MODE_PAIRING;
        bt_runtime_config.clear_bonding_on_start = true;
        memset(bt_runtime_config.remote_addr, 0, sizeof(bt_runtime_config.remote_addr));

        if (pair_button_held) {
            bt_clear_paired_controller();
            printf("Pair button held on boot: entering pairing mode\n");
        } else {
            printf("No saved controller: entering pairing mode\n");
        }
    } else {
        bt_runtime_config.start_mode = BT_HOST_START_MODE_NORMAL;
        bt_runtime_config.clear_bonding_on_start = false;
        printf("Loaded saved controller, connecting without re-pair\n");
    }

    if (cyw43_arch_init() != 0) {
        printf("Bluetooth init failed: cyw43_arch_init()\n");
        return false;
    }

    btstack_host_start_non_blocking(&bt_runtime_config,
                                    bluetooth_report_callback,
                                    bluetooth_pairing_complete_callback,
                                    bluetooth_pairing_reset_callback);
    bt_controller_initialized = true;
    printf("Bluetooth host started (non-blocking)\n");
    return true;
}

static inline bool bt_hat_has_direction(uint8_t hat, controller_button_t button)
{
    switch (button) {
        case BUTTON_UP:
            return (hat == 0u) || (hat == 1u) || (hat == 7u);
        case BUTTON_RIGHT:
            return (hat == 1u) || (hat == 2u) || (hat == 3u);
        case BUTTON_DOWN:
            return (hat == 3u) || (hat == 4u) || (hat == 5u);
        case BUTTON_LEFT:
            return (hat == 5u) || (hat == 6u) || (hat == 7u);
        default:
            return false;
    }
}

static inline bool bt_axes_have_direction(const bt_host_gamepad_report_t *report, controller_button_t button)
{
    const bool left = report->lx < 64u;
    const bool right = report->lx > 192u;
    const bool up = report->ly < 64u;
    const bool down = report->ly > 192u;

    switch (button) {
        case BUTTON_UP:
            return up;
        case BUTTON_RIGHT:
            return right;
        case BUTTON_DOWN:
            return down;
        case BUTTON_LEFT:
            return left;
        default:
            return false;
    }
}

static inline bool bt_button_pressed(controller_button_t button)
{
    if (!bt_report_valid) {
        return false;
    }

    const bt_host_gamepad_report_t *report = &bt_latest_report;
    switch (button) {
        case BUTTON_UP:
        case BUTTON_DOWN:
        case BUTTON_LEFT:
        case BUTTON_RIGHT:
            if (report->dpad <= 7u) {
                return bt_hat_has_direction(report->dpad, button);
            }
            return bt_axes_have_direction(report, button);
        case BUTTON_A:
            if (report->controller_type == BT_HOST_CONTROLLER_PS4) {
                return (report->buttons_primary & (1u << 5)) != 0; // Cross
            }
            if (report->controller_type == BT_HOST_CONTROLLER_GENERIC) {
                // Generic parser packs dpad into low nibble; button 1 starts at bit 4.
                return (report->buttons_primary & (1u << 4)) != 0;
            }
            return (report->buttons_primary & (1u << 0)) != 0;
        case BUTTON_B:
            if (report->controller_type == BT_HOST_CONTROLLER_PS4) {
                return (report->buttons_primary & (1u << 6)) != 0; // Circle
            }
            if (report->controller_type == BT_HOST_CONTROLLER_GENERIC) {
                return (report->buttons_primary & (1u << 5)) != 0;
            }
            return (report->buttons_primary & (1u << 1)) != 0;
        case BUTTON_SELECT:
            if (report->controller_type == BT_HOST_CONTROLLER_PS4) {
                return (report->buttons_secondary & (1u << 4)) != 0; // Share
            }
            if (report->controller_type == BT_HOST_CONTROLLER_GENERIC) {
                // Common HID ordering: button 9 -> bit 4 in buttons_secondary.
                return (report->buttons_secondary & (1u << 4)) != 0;
            }
            return (report->buttons_primary & (1u << 2)) != 0;
        case BUTTON_START:
            if (report->controller_type == BT_HOST_CONTROLLER_PS4) {
                return (report->buttons_secondary & (1u << 5)) != 0; // Options
            }
            if (report->controller_type == BT_HOST_CONTROLLER_GENERIC) {
                // Common HID ordering: button 10 -> bit 5 in buttons_secondary.
                return (report->buttons_secondary & (1u << 5)) != 0;
            }
            return (report->buttons_primary & (1u << 3)) != 0;
        case BUTTON_HOME:
            if (report->controller_type == BT_HOST_CONTROLLER_PS4) {
                return (report->buttons_tertiary & (1u << 0)) != 0; // PS
            }
            if (report->controller_type == BT_HOST_CONTROLLER_GENERIC) {
                // Common HID ordering: button 11 -> bit 6 in buttons_secondary.
                return (report->buttons_secondary & (1u << 6)) != 0;
            }
            return (report->buttons_secondary & (1u << 0)) != 0;
        default:
            return false;
    }
}

static bool __no_inline_not_in_flash_func(game_controller)(void)
{
    static uint32_t last_micros = 0;

    if (bt_controller_initialized) {
        btstack_host_poll();
    }

    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 5000) {
        return false;
    }
    last_micros = current_micros;

    if (!bt_report_valid) {
        reset_button_states();
        return false;
    }

    for (int i = 0; i < BUTTON_COUNT; ++i) {
        bool pressed = bt_button_pressed((controller_button_t)i);
        button_states[i] = pressed ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
    }

    bool any_pressed = false;
    for (uint8_t i = 0; i < BUTTON_COUNT; i++)
    {
        if (button_states[i] == BUTTON_STATE_PRESSED)
        {
            any_pressed = true;
            break;
        }
    }
    gpio_put(PIN_LED, any_pressed);

    return true;
}
#elif USE_NES_CLASSIC_CONTROLLER
static bool __no_inline_not_in_flash_func(game_controller)(void)
{
    static uint32_t last_micros = 0;
    static bool initialized = false;
    static uint8_t i2c_buffer[8] = {0};
    static absolute_time_t next_init_time = {0};
    static bool waiting_for_init = false;
    static uint32_t pending_init_delay_ms = NES_CONTROLLER_INIT_DELAY_MS;
    static bool controller_armed = false; // don't report until we've seen a clean idle

    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 5000)   // NES Classic queries about every 5ms
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
        controller_armed = false;

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

        reset_button_states();

        return false;
    }

    last_micros = current_micros;

    i2c_buffer[0] = 0x00;
    (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 1, false);   // false - finished with bus
    sleep_us(300);  // NES Classic uses about 330uS

    // Clear buffer to avoid stale bits if the device returns fewer bytes.
    for (int j = 0; j < 8; ++j) {
        i2c_buffer[j] = 0xFF;
    }

    // Get button data - only read 6 bytes
    int ret = i2c_read_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 6, false);
    if (ret < 0)
    {
        last_micros = time_us_32();
        return false;
    }
        
    bool valid = false;
    uint8_t i;
    // Validate the buffer - Check the first 4 bytes
    for (i = 0; i < 4; i++)
    {
        if (i2c_buffer[i] != 0xFF)
            valid = true;
    }

    if (valid)
    {
        // Reject frames that are clearly bogus: all zeros on payload bytes.
        const uint8_t b4 = i2c_buffer[4];
        const uint8_t b5 = i2c_buffer[5];
        if (b4 == 0x00 && b5 == 0x00) {
            return false;
        }

        button_states[BUTTON_START] = BIT_IS_CLEAR(b4, 2) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_SELECT] = BIT_IS_CLEAR(b4, 4) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_DOWN] = BIT_IS_CLEAR(b4, 6) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_RIGHT] = BIT_IS_CLEAR(b4, 7) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_HOME] = BIT_IS_CLEAR(b4, 3) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;

        button_states[BUTTON_UP] = BIT_IS_CLEAR(b5, 0) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_LEFT] = BIT_IS_CLEAR(b5, 1) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_A] = BIT_IS_CLEAR(b5, 4) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;
        button_states[BUTTON_B] = BIT_IS_CLEAR(b5, 6) ? BUTTON_STATE_PRESSED : BUTTON_STATE_UNPRESSED;

#ifdef DEBUG_BUTTON_PRESS
        static uint8_t button_states_debug[BUTTON_COUNT] = {0};
        if (memcmp(button_states, button_states_debug, sizeof(button_states)) != 0)
        {
            memcpy(button_states_debug, button_states, sizeof(button_states));
            printf("Button states:  ");
            for (uint8_t i = 0; i < BUTTON_COUNT; i++)
                printf("%d ", button_states_debug[i]);
            printf("\n");
        }
#endif

        //TESTING!!!
        // // Prevent in-game reset lockup
        // // If A,B,Select and Start are all pressed, release them!
        // if ((button_states[BUTTON_A] | button_states[BUTTON_B] | button_states[BUTTON_SELECT]| button_states[BUTTON_START])==0)
        // {
        //     button_states[BUTTON_A] = BUTTON_STATE_UNPRESSED;
        //     button_states[BUTTON_B] = BUTTON_STATE_UNPRESSED;
        //     button_states[BUTTON_SELECT] = BUTTON_STATE_UNPRESSED;
        //     button_states[BUTTON_START] = BUTTON_STATE_UNPRESSED;
        // }
    }

    if (!valid )
    {
        initialized = false;
        waiting_for_init = false;
        pending_init_delay_ms = NES_CONTROLLER_REINIT_DELAY_MS;
        controller_armed = false;
        last_micros = time_us_32();
        return false;
    }

    // Debounce/arm: require several consecutive idle frames before honoring input.
    bool any_pressed = false;
    for (i = 0; i < BUTTON_COUNT; i++)
    {
        if (button_states[i] == BUTTON_STATE_PRESSED)
        {
            any_pressed = true;
            break;
        }
    }
    gpio_put(PIN_LED, any_pressed);


    if (!controller_armed)
    {
        if (any_pressed)
        {
            reset_button_states();
            printf("Waiting for clean button report (no presses)...\n");
            return false;
        }

        controller_armed = true;
        printf("Controller armed\n");
    }


    return true;
}
#else
// Using NES original controller!
static bool __no_inline_not_in_flash_func(game_controller)(void)
{
    static uint32_t last_micros = 0;
    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 20000)
        return false;

    last_micros = current_micros;

    gpio_put(PIN_NES_LATCH, 1);
    sleep_us(5);
    gpio_put(PIN_NES_LATCH, 0);
    sleep_us(1);
    button_states[0] = gpio_get(PIN_NES_DATA) ? BUTTON_STATE_UNPRESSED : BUTTON_STATE_PRESSED;
    sleep_us(4);

    for (uint i = 1; i < 8; i++) 
    {
        sleep_us(8);
        gpio_put(PIN_NES_PULSE, 0);
        sleep_us(1);
        gpio_put(PIN_NES_PULSE, 1);
        sleep_us(8);
        button_states[i] = gpio_get(PIN_NES_DATA) ? BUTTON_STATE_UNPRESSED : BUTTON_STATE_PRESSED;
    }

    // Debounce/arm: require several consecutive idle frames before honoring input.
    bool any_pressed = false;
    for (uint i = 0; i < BUTTON_COUNT; i++)
    {
        if (button_states[i] == BUTTON_STATE_PRESSED)
        {
            any_pressed = true;
            break;
        }
    }
    gpio_put(PIN_LED, any_pressed);

    return true;
}
#endif

// Palette support for both 640x480 and 800x600 modes
static void set_game_palette(int index)
{
    set_scheme_index(index);
    game_palette_rgb888 = (uint32_t*)get_scheme();

    // Set RGB888 palette pointer for 2bpp palette mode
    // Works for both 640x480 (no borders) and 800x600 (with borders)
    dvi_get_blank_settings(&dvi0)->palette_rgb888 = game_palette_rgb888;
}

static void sd_stream_chunk_yield(void)
{
    // Allow other subsystems (controller polling, audio) to make progress between SD chunks.
#if ENABLE_AUDIO
    pump_audio_samples();
#endif
    tight_loop_contents();
}

static bool button_is_pressed(controller_button_t button)
{
    return button_states[button] == BUTTON_STATE_PRESSED;
}

static bool button_was_released(controller_button_t button)
{
    return button_states[button] == BUTTON_STATE_UNPRESSED && button_states_previous[button] == BUTTON_STATE_PRESSED;
}

static bool command_check(void)
{
    // No change in states, just return false
    if (memcmp(button_states, button_states_previous, sizeof(button_states)) == 0)
        return false;

    // // Ignore any button presses for the first 5 seconds
    // if (time_us_32() < 5000000)
    //     return false;

    bool result = false;

    if (button_is_pressed(BUTTON_SELECT))
    {
        // SELECT + START - TODO: Change to OSD menu
        if (button_was_released(BUTTON_START))
        {
            result = true;
            printf("Hotkey: SELECT+START\n");
            OSD_toggle();
        }
    }
    else
    {
        // select not pressed

        if (button_was_released(BUTTON_HOME))
        {
            result = true;
#if HOME_RESETS_TO_BOOTLOADER
            // HOME - reset into USB mass storage mode for easier programming
            reset_pico(RESTART_MASS_STORAGE);
#else
            OSD_toggle();
#endif
        }
        else
        {
            if (OSD_is_enabled())
            {
                if (button_was_released(BUTTON_DOWN))
                {
                    result = true;
                    OSD_change_active_line(1);
                }
                else if (button_was_released(BUTTON_UP))
                {
                    result = true;
                    OSD_change_active_line(-1);
                }
                else if (button_was_released(BUTTON_RIGHT) 
                        || button_was_released(BUTTON_LEFT)
                        || button_was_released(BUTTON_A))
                {
                    result = true;
                    controller_button_t button = button_was_released(BUTTON_A) ? BUTTON_A : button_was_released(BUTTON_RIGHT) ? BUTTON_RIGHT : BUTTON_LEFT;
                    switch (OSD_get_active_line())
                    {
                        case OSD_LINE_COLOR_SCHEME:
                            set_game_palette(button == BUTTON_RIGHT ? get_scheme_index() + 1 : get_scheme_index() - 1);
                            update_osd();
                            break;
                        case OSD_LINE_FRAME_BLENDING:
                            frame_blending_enabled = !frame_blending_enabled;
                            printf("Frame blending: %s\n", frame_blending_enabled ? "ENABLED" : "DISABLED");
                            if (!frame_blending_enabled) {
                                // Clear previous frame buffer when disabling
                                memset(packed_buffer_previous, 0x00, PACKED_FRAME_SIZE);  // 0x00 = all white pixels
                            }

                            update_osd();
                            break;
                        case OSD_LINE_AUDIO_RESET:
                            if (button == BUTTON_A) {
                                restart_audio_pipeline();
                            }
                            break;
                        // case OSD_LINE_RESET_GAMEBOY:
                        //     gameboy_reset();
                        //     break;
                        case OSD_LINE_RESET_DEVICE:
                            if (button == BUTTON_A)
                            {
                                reset_pico(restart_option);
                            }
                            else
                            {
                                restart_option = restart_option == RESTART_NORMAL ? RESTART_MASS_STORAGE : RESTART_NORMAL;
                                update_osd();
                            }
                            break;
                        case OSD_LINE_SAVE_SETTINGS:
                            if (button == BUTTON_A)
                                save_settings();
                            break;
                        case OSD_LINE_EXIT:
                            if (button == BUTTON_A)    
                                OSD_toggle();
                            break;
                    }
                }
                else if (button_was_released(BUTTON_B))
                {
                    result = true;
                    OSD_toggle();
                }
            }
        }
    }

    return result;
}

static void button_state_save_previous(void)
{
    for (int i = 0; i < BUTTON_COUNT; i++) 
    {
        button_states_previous[i] = button_states[i];
    }
}

static void reset_button_states(void)
{
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        button_states[i] = BUTTON_STATE_UNPRESSED;
        button_states_previous[i] = BUTTON_STATE_UNPRESSED;
    }
}

static void save_settings(void)
{
    eeprom_result_t result;
    result = EEPROM_write(SAVE_INDEX_SCHEME, get_scheme_index());
    if (result == EEPROM_SUCCESS)
    {
        // We have to disable DVI to safely commit EEPROM changes
        dvi_serialiser_enable(&dvi0.ser_cfg, false);
        result = EEPROM_commit();
        dvi_serialiser_enable(&dvi0.ser_cfg, true);

        if (result == EEPROM_SUCCESS)
        {
            printf("Saved scheme index to EEPROM: %d\n", get_scheme_index());
        }
        else
        {
            printf("Failed to commit scheme index to EEPROM: %d\n", get_scheme_index());
        }
    }
    else
    {
        printf("Failed to save scheme index to EEPROM: %d\n", get_scheme_index());
    }
}

static void reset_pico(restart_option_t restart_option)
{
    if (restart_option == RESTART_NORMAL)
    {
        // Reset the Pico
        printf("Resetting Pico...\n");
        watchdog_reboot(0, 0, 0);
    }
    else
    {
        // Reset into USB boot mode
        printf("Resetting Pico into USB boot mode...\n");
        reset_usb_boot(0, DISABLE_MASK_NONE);
    }

    while (1)
    {
        tight_loop_contents();
    }
}

static void load_settings(void)
{
    boot_checkpoint("Loading settings...\n");
    uint8_t scheme = (uint8_t)SCHEME_SGB_4H;
    if (EEPROM_read(SAVE_INDEX_SCHEME, &scheme) == EEPROM_SUCCESS)
    {
        printf("Loaded palette from EEPROM: %d\n", scheme);
    }
    else
    {
        printf("Failed to load palette from EEPROM, using default: %d\n", scheme);
    }
    set_game_palette((int)scheme);

    boot_checkpoint("Settings loaded");

    // set_scheme_index((int)EEPROM_read(SAVE_INDEX_SCHEME));
    // frame_blending_enabled = EEPROM_read(SAVE_INDEX_FRAME_BLENDING) == 1;
}

static void update_osd(void)
{
    char buff[32];
    sprintf(buff, "COLOR SCHEME:%8d", get_scheme_index());
    OSD_set_line_text(OSD_LINE_COLOR_SCHEME, buff);

    // sprintf(buff, "BORDER COLOR:%8d", get_border_color_index());
    // OSD_set_line_text(OSD_LINE_BORDER_COLOR, buff);


    // OSD_set_line_text(OSD_LINE_RESET_GAMEBOY, "RESET GAMEBOY");

    sprintf(buff, "FRAME BLEND:%9s", frame_blending_enabled ? "ON" : "OFF");
    OSD_set_line_text(OSD_LINE_FRAME_BLENDING, buff);

    OSD_set_line_text(OSD_LINE_AUDIO_RESET, "AUDIO RESET");
    
    sprintf(buff, "RESET DEVICE:%8s", restart_option == RESTART_MASS_STORAGE ? "USB" : "NORM");
    OSD_set_line_text(OSD_LINE_RESET_DEVICE, buff);

    OSD_set_line_text(OSD_LINE_SAVE_SETTINGS, "SAVE SETTINGS");

    OSD_set_line_text(OSD_LINE_EXIT, "EXIT");
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

//********************************************************************************
// PUBLIC FUNCTIONS
//********************************************************************************
int main(void)
{
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
    log_free_heap("after clock init");
    reset_button_states();
    
    // Initialize stdio for serial debugging
    stdio_init_all();

    // Initialize frame blending lookup tables (one-time computation)
    // Both modes use packed buffers for capture, so both need LUTs
    init_frame_blending_luts();

    reset_active_rom_to_builtin();
    log_free_heap("after reset_active_rom_to_builtin");

    // Initialize controller I2C and LED before any menu interaction
    boot_checkpoint("Initializing GPIO");
    initialize_gpio();
    boot_checkpoint("GPIO initialized");

#if USE_BLUETOOTH_CONTROLLER
    if (!init_bluetooth_controller()) {
        printf("Bluetooth controller initialization failed; continuing without Bluetooth input.\n");
    }
#endif


    for (int i = 0; i < 5; i++) {
        printf("\n\n=== PicoDVI-DMG_EMU Starting (attempt %d) ===\n", i + 1);
        sleep_ms(100);
    }

    printf("Firmware build: %s %s\n", __DATE__, __TIME__);

    // boot_checkpoint("USB console ready");



    boot_checkpoint("Clocks configured");

    printf("[TRACE] entering display-prep stage\n");

    boot_checkpoint("Preparing display buffers (pre-copy)");
    memset(packed_buffer_0, 0xFF, PACKED_FRAME_SIZE);
    boot_checkpoint("Display buffer 0 clear complete");
    memset(packed_buffer_1, 0xFF, PACKED_FRAME_SIZE);
    boot_checkpoint("Display buffer 1 clear complete");
    packed_display_ptr = packed_buffer_0;
    packed_render_ptr = packed_buffer_1;
    boot_checkpoint("Display buffers primed");

    bool rom_list_ready = false;

#if ENABLE_SD_CARD
    boot_checkpoint("Calling mount_sd_card");
    bool sd_mount_ok = mount_sd_card();
    boot_checkpoint("mount_sd_card returned");
    log_free_heap("after mount_sd_card");

    if (!sd_mount_ok) {
        printf("Continuing with built-in ROM image (SD unavailable).\n");
    } else {
        boot_checkpoint("Scanning SD for ROM list");
        if (build_sd_rom_list()) {
            boot_checkpoint("SD ROM list built");
            printf("%lu ROM(s) discovered on SD card.\n", (unsigned long)sd_rom_list_count);
            log_free_heap("after ROM list build");
            rom_list_ready = true;
        } else {
            boot_checkpoint("No SD ROMs found");
            printf("No SD ROMs found - using built-in image.\n");
            reset_active_rom_to_builtin();
            log_free_heap("after no SD ROM found");
        }
    }
    boot_checkpoint("SD stage complete");
#else
    printf("SD card disabled at build time; using built-in ROM image.\n");
    reset_active_rom_to_builtin();
    boot_checkpoint("SD stage skipped (disabled)");
#endif

    // Initialize OSD overlays (disabled by default)
    OSD_init(DMG_PIXELS_X, DMG_PIXELS_Y);
    OSD_clear();
    OSD_set_enabled(false);

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    boot_checkpoint("DVI configured");
    log_free_heap("after DVI init");

    load_settings();

    uint32_t *bufptr = (uint32_t *)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    boot_checkpoint("Scanline buffers primed");

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
    int cts = dvi0.timing->bit_clk_khz * hdmi_n[offset] / (rate / 100) / 128; // ORIGINAL
    dvi_get_blank_settings(&dvi0)->top = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, rate, cts, hdmi_n[offset]);
    reset_audio_ring_prefill(AUDIO_BUFFER_SIZE - 8);
    for (int i = 0; i < 16; ++i) {
        pump_audio_samples();
    }
    sleep_ms(10); // allow PLL/DMA to settle after warmup
    size_t prefill = get_read_size(&dvi0.audio_ring, false);
    printf("Audio buffer pre-filled to %lu samples\n", (unsigned long)prefill);
    boot_checkpoint("Audio pipeline ready");
#endif

    boot_checkpoint("Starting Core 1 (DVI output)");
    multicore_launch_core1(core1_main);
    boot_checkpoint("Core 1 running - now consuming video");

    if (rom_list_ready) {
        boot_checkpoint("Displaying SD ROM menu");
        bool user_selected_rom = sd_rom_selection_menu(sd_rom_path, sizeof(sd_rom_path));
        if (user_selected_rom) {
            boot_checkpoint("User selected SD ROM");
            printf("About to load SD ROM: %s\n", sd_rom_path);
            if (load_sd_rom_file(sd_rom_path)) {
                boot_checkpoint("load_sd_rom_file returned true");
                printf("load_sd_rom_file success for %s\n", sd_rom_path);
                boot_checkpoint("SD ROM loaded into memory");
                log_free_heap("after SD ROM load");
            } else {
                printf("SD ROM load failed - reverting to built-in image.\n");
                reset_active_rom_to_builtin();
                log_free_heap("after SD ROM load failure fallback");
            }
        } else {
            boot_checkpoint("SD ROM selection skipped");
            printf("User declined SD ROM - using built-in image.\n");
            reset_active_rom_to_builtin();
            log_free_heap("after user skipped SD ROM");
        }
    }

    if (!init_peanut_emulator()) {
        while (1) {
            tight_loop_contents();
        }
    }
    log_free_heap("after gb init");
    boot_checkpoint("Peanut-GB initialized");
    printf("Peanut-GB initialized - entering main loop\n");

#if ENABLE_AUDIO
    // One more prefill after ROM/emulator init so the ring is full when gameplay starts.
    reset_audio_ring_prefill(AUDIO_BUFFER_SIZE - 8);
    for (int i = 0; i < 12; ++i) {
        pump_audio_samples();
    }
    sleep_ms(5);
#endif

    absolute_time_t next_frame_time = make_timeout_time_us(DMG_FRAME_DURATION_US);

    uint32_t frame_counter = 0;
    bool audio_guard_done = false;

    update_osd();

    while (true)
    {
        game_controller();
        update_emulator_inputs();
        run_emulator_frame();
        frame_counter++;
    #if ENABLE_SD_STATS_LOG
        if ((frame_counter % 120u) == 0) { // roughly every 2 seconds at 60fps
            sd_cache_log_frames++;
            printf("[SD] cache hits=%lu misses=%lu interval=%lu frames\n",
               (unsigned long)sd_cache_hits,
               (unsigned long)sd_cache_misses,
               (unsigned long)sd_cache_log_frames * 120u);
            sd_cache_hits = 0;
            sd_cache_misses = 0;
            sd_cache_log_frames = 0;
        }
    #endif
        if (gb_faulted) {
            printf("Emulator halted after Peanut-GB error %d (val=0x%04x)\n",
                   (int)gb_fault_info.code,
                   (unsigned int)gb_fault_info.val);
            while (true) {
                tight_loop_contents();
            }
        }
    #if ENABLE_AUDIO
        pump_audio_samples();
        if (!audio_guard_done) {
            audio_guard_done = ensure_audio_ready();
        }
    #endif

        // Apply frame blending if enabled (packed 2bpp data)
        if (frame_blending_enabled)
        {
            uint8_t *current_frame = packed_render_ptr;
            for (size_t i = 0; i < PACKED_FRAME_SIZE; i++)
            {
                uint8_t curr = current_frame[i];
                uint8_t prev = packed_buffer_previous[i];

                uint8_t blended = 0;
                for (int pixel = 0; pixel < 4; pixel++)
                {
                    int shift = (3 - pixel) * 2;
                    uint8_t p_curr = (curr >> shift) & 0x03;
                    uint8_t p_prev = (prev >> shift) & 0x03;
                    uint8_t p_blend = (p_curr == 0) ? (p_curr | p_prev) : p_curr;
                    blended |= (uint8_t)(p_blend << shift);
                }

                current_frame[i] = blended;
                packed_buffer_previous[i] = store_lut[curr];
            }
        }

        // Overlay OSD text, if enabled
        OSD_render((uint8_t*)packed_render_ptr);

        swap_display_buffers();
        (void)command_check();
        button_state_save_previous();

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
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

// Keep these defines at the top before including pico headers
#define PICO_DEFAULT_UART_BAUD_RATE 115200
#define PICO_DEFAULT_UART_TX_PIN    0
#define PICO_DEFAULT_UART_RX_PIN    1
#define PICO_DEFAULT_UART 0
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

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "audio_ring.h"  // For get_write_size and get_read_size

#include "mario.h"

#include "colors.h"
#include "hedley.h"
#include "board_pins.h"
// #include "hw_config.h"
#include "sdcard.h"
#include "ff.h"
#include "f_util.h"
#include "diskio.h"

#define SAMPLE_FREQ                 32768
#define AUDIO_BUFFER_SIZE           1024
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
#define ENABLE_OSD                  0  // Set to 1 to enable OSD code, 0 to disable (TODO)
#define ENABLE_SD_STATS_LOG         0  // Set to 1 to print periodic SD cache hit/miss stats
#define ENABLE_HEAP_LOG             0  // Set to 1 to print free-heap checkpoints

#define MAX_SD_ROM_FILE_BYTES       (ROM_BANK_SIZE * 512u)
#define MAX_SD_ROM_HEAP_BYTES       (320u * 1024u)  // allow 256 KB ROMs to heap-load
#define SD_HEAP_SAFETY_MARGIN_BYTES (8u * 1024u)
#define SD_ROM_CACHE_SLOTS          8u    // larger cache to cut bank thrash on streamed carts
#define SD_STREAM_CHUNK_BYTES       4096u // balanced chunk to reduce overhead without long stalls
#define ENABLE_SD_HEAP_LOAD         1   // 0 = always stream from SD to save RAM, 1 = allow heap copy when small enough
#define MAX_SD_ROM_LIST             16u  // limit menu entries to save RAM
#define MAX_SD_ROM_PATH_LEN         192u
#define MENU_MAX_LABEL_CHARS        24u
#define MENU_LINE_HEIGHT            8
#define MENU_COLOR_BG               0u
#define MENU_COLOR_FG               3u
#define MENU_COLOR_HL_BG            2u
#define MENU_COLOR_HL_FG            0u
#define MENU_COLOR_TITLE            3u


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

static sd_rom_cache_slot_t sd_rom_cache[SD_ROM_CACHE_SLOTS];
static char sd_rom_list[MAX_SD_ROM_LIST][MAX_SD_ROM_PATH_LEN];
static uint32_t sd_rom_list_count = 0;
static uint32_t sd_cache_hits = 0;
static uint32_t sd_cache_misses = 0;
static uint32_t sd_cache_log_frames = 0;

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

static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);
static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
static void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val);
static void update_emulator_inputs(void);
static void swap_display_buffers(void);
static void handle_palette_hotkeys(void);
static bool mount_sd_card(void);
// static bool discover_sd_rom(void);
static bool load_sd_rom_file(const char *path);
static void reset_active_rom_to_builtin(void);
static void boot_checkpoint(const char *label);
static void invalidate_sd_rom_cache(void);
static void close_sd_rom_stream(void);
static void free_sd_rom_heap(void);
static size_t estimate_free_heap_bytes(void);
static bool sd_stream_load_bank(uint32_t bank_index, sd_rom_cache_slot_t *slot);
static uint8_t sd_stream_read_byte(size_t addr);
static void sd_stream_chunk_yield(void);
static void log_free_heap(const char *tag);
static const char *path_basename(const char *path);
static void clear_sd_rom_list(void);
static uint32_t scan_directory_for_roms(const char *directory);
static bool build_sd_rom_list(void);
// static void print_sd_rom_list(void);
static bool sd_rom_selection_menu(char *selected_path, size_t selected_len);


static void initialize_gpio(void);

static bool __no_inline_not_in_flash_func(nes_classic_controller)(void);
static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline);


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

static void log_roms_in_directory(const char *directory)
{
    if ((directory == NULL) || !sd_filesystem_ready) {
        return;
    }

    DIR dir;
    FILINFO info;
    memset(&info, 0, sizeof(info));
    FRESULT fr = f_opendir(&dir, directory);
    if (fr != FR_OK) {
        printf("ROM scan: unable to open %s (%d: %s)\n", directory, fr, FRESULT_str(fr));
        return;
    }

    printf("ROM scan: %s\n", directory);
    bool any = false;

    while (true) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK) {
            printf("  readdir failed (%d: %s)\n", fr, FRESULT_str(fr));
            break;
        }
        if (info.fname[0] == '\0') {
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

        any = true;
        printf("  %s\n", name);
    }

    if (!any) {
        printf("  <no ROMs>\n");
    }

    FRESULT close_result = f_closedir(&dir);
    if (close_result != FR_OK) {
        printf("  close failed (%d: %s)\n", close_result, FRESULT_str(close_result));
    }
}

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

//     for (size_t i = 0; i < ARRAY_SIZE(search_paths); ++i) {
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

    for (size_t i = 0; i < ARRAY_SIZE(search_paths); ++i) {
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



typedef struct
{
    char ch;
    uint8_t rows[7];
} glyph_5x7_t;

#define GLYPH(_c, _r0, _r1, _r2, _r3, _r4, _r5, _r6) \
    { (_c), { (_r0), (_r1), (_r2), (_r3), (_r4), (_r5), (_r6) } }

static const glyph_5x7_t menu_font[] = {
    GLYPH(' ', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
    GLYPH('?', 0x0E, 0x11, 0x02, 0x04, 0x04, 0x00, 0x04),
    GLYPH('0', 0x1E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x1E),
    GLYPH('1', 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E),
    GLYPH('2', 0x1E, 0x01, 0x01, 0x0E, 0x10, 0x10, 0x1F),
    GLYPH('3', 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E),
    GLYPH('4', 0x12, 0x12, 0x12, 0x1F, 0x02, 0x02, 0x02),
    GLYPH('5', 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E),
    GLYPH('6', 0x0F, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E),
    GLYPH('7', 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    GLYPH('8', 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E),
    GLYPH('9', 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x1E),
    GLYPH('A', 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    GLYPH('B', 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E),
    GLYPH('C', 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E),
    GLYPH('D', 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C),
    GLYPH('E', 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F),
    GLYPH('F', 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10),
    GLYPH('G', 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E),
    GLYPH('H', 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    GLYPH('I', 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E),
    GLYPH('J', 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E),
    GLYPH('K', 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11),
    GLYPH('L', 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F),
    GLYPH('M', 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11),
    GLYPH('N', 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11),
    GLYPH('O', 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E),
    GLYPH('P', 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10),
    GLYPH('Q', 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D),
    GLYPH('R', 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11),
    GLYPH('S', 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E),
    GLYPH('T', 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    GLYPH('U', 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E),
    GLYPH('V', 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04),
    GLYPH('W', 0x11, 0x11, 0x11, 0x11, 0x15, 0x1B, 0x11),
    GLYPH('X', 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11),
    GLYPH('Y', 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04),
    GLYPH('Z', 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F),
    GLYPH('-', 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00),
    GLYPH('_', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F),
    GLYPH('.', 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C),
    GLYPH('\'', 0x06, 0x06, 0x02, 0x04, 0x00, 0x00, 0x00),
    GLYPH('/', 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00),
    GLYPH('(', 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02),
    GLYPH(')', 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08)
};

static const glyph_5x7_t *lookup_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }

    const size_t glyph_count = ARRAY_SIZE(menu_font);
    for (size_t i = 0; i < glyph_count; ++i) {
        if (menu_font[i].ch == c) {
            return &menu_font[i];
        }
    }

    return &menu_font[1]; // '?'
}

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
        const glyph_5x7_t *glyph = lookup_glyph(text[i]);
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
                c == '(' || c == ')' || c == '\'');
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

    const uint32_t visible_rows = (DMG_PIXELS_Y - 28) / MENU_LINE_HEIGHT;
    uint32_t page_start = 0;
    if (selected_index >= visible_rows) {
        page_start = selected_index - visible_rows + 1;
    }
    if (sd_rom_list_count > visible_rows && (page_start + visible_rows) > sd_rom_list_count) {
        page_start = sd_rom_list_count - visible_rows;
    }

    for (uint32_t row = 0; row < visible_rows && (page_start + row) < sd_rom_list_count; ++row) {
        uint32_t idx = page_start + row;
        char label[MENU_MAX_LABEL_CHARS];
        format_rom_label(sd_rom_list[idx], label, sizeof(label));
        const bool selected = (idx == selected_index);
        uint8_t fg = selected ? MENU_COLOR_HL_FG : MENU_COLOR_FG;
        uint8_t bg = selected ? MENU_COLOR_HL_BG : MENU_COLOR_BG;
        draw_text_line(buf, 8, 16 + (int)row * MENU_LINE_HEIGHT, label, fg, bg);
    }

    draw_text_line(buf, 4, DMG_PIXELS_Y - 12, "A/START=LOAD  B=CANCEL", MENU_COLOR_FG, MENU_COLOR_BG);
    swap_display_buffers();
}

static bool sd_rom_selection_menu(char *selected_path, size_t selected_len)
{
    if ((selected_path == NULL) || (selected_len == 0) || (sd_rom_list_count == 0)) {
        return false;
    }

    uint32_t index = 0;
    button_state_t last_up = BUTTON_STATE_UNPRESSED;
    button_state_t last_down = BUTTON_STATE_UNPRESSED;
    button_state_t last_a = BUTTON_STATE_UNPRESSED;
    button_state_t last_start = BUTTON_STATE_UNPRESSED;
    button_state_t last_b = BUTTON_STATE_UNPRESSED;
    button_state_t last_home = BUTTON_STATE_UNPRESSED;
    // absolute_time_t auto_select_deadline = make_timeout_time_ms(4000);
    render_rom_menu(index);

    while (true) {
        nes_classic_controller();
        handle_palette_hotkeys();

        const button_state_t up = button_states[BUTTON_UP];
        const button_state_t down = button_states[BUTTON_DOWN];
        const button_state_t a = button_states[BUTTON_A];
        const button_state_t b = button_states[BUTTON_B];
        const button_state_t start = button_states[BUTTON_START];
        const button_state_t home = button_states[BUTTON_HOME];

        bool moved = false;
        if ((up == BUTTON_STATE_PRESSED) && (last_up == BUTTON_STATE_UNPRESSED)) {
            index = (index == 0) ? (sd_rom_list_count - 1) : (index - 1);
            moved = true;
        }
        if ((down == BUTTON_STATE_PRESSED) && (last_down == BUTTON_STATE_UNPRESSED)) {
            index = (index + 1u) % sd_rom_list_count;
            moved = true;
        }
        if (moved) {
            render_rom_menu(index);
            // auto_select_deadline = make_timeout_time_ms(4000);
        }

        bool confirm = false;
        if ((a == BUTTON_STATE_PRESSED) && (last_a == BUTTON_STATE_UNPRESSED)) {
            confirm = true;
        }
        if ((start == BUTTON_STATE_PRESSED) && (last_start == BUTTON_STATE_UNPRESSED)) {
            confirm = true;
        }
        if (confirm) {
            strncpy(selected_path, sd_rom_list[index], selected_len - 1);
            selected_path[selected_len - 1] = '\0';
            render_rom_menu(index);
            return true;
        }

        bool cancel = false;
        if ((b == BUTTON_STATE_PRESSED) && (last_b == BUTTON_STATE_UNPRESSED)) {
            cancel = true;
        }
        if ((home == BUTTON_STATE_PRESSED) && (last_home == BUTTON_STATE_UNPRESSED)) {
            cancel = true;
        }
        if (cancel) {
            return false;
        }
        
        // if (time_reached(auto_select_deadline)) {
        //     strncpy(selected_path, sd_rom_list[index], selected_len - 1);
        //     selected_path[selected_len - 1] = '\0';
        //     render_rom_menu(index);
        //     return true;
        // }

        last_up = up;
        last_down = down;
        last_a = a;
        last_b = b;
        last_start = start;
        last_home = home;

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

//     for (size_t i = 0; i < ARRAY_SIZE(search_paths); ++i) {
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
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
    log_free_heap("after clock init");

    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        button_states[i] = BUTTON_STATE_UNPRESSED;
    }
 
    
    stdio_init_all();
    // uart_init(uart0, 115200);
    // gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    // gpio_set_function(PICO_DEFAULT_UART_RX_PIN, GPIO_FUNC_UART);
    
    // sleep_ms(3000);  // Give USB more time to enumerate



    reset_active_rom_to_builtin();
    log_free_heap("after reset_active_rom_to_builtin");

    // Initialize controller I2C and LED before any menu interaction
    boot_checkpoint("Initializing GPIO");
    initialize_gpio();
    boot_checkpoint("GPIO initialized");


    for (int i = 0; i < 5; i++) {
        printf("\n\n=== PicoDVI-DMG_EMU Starting (attempt %d) ===\n", i + 1);
        sleep_ms(100);
    }

    printf("Firmware build: %s %s\n", __DATE__, __TIME__);

    // boot_checkpoint("USB console ready");



    boot_checkpoint("Clocks configured");

    printf("[TRACE] entering display-prep stage\n");

    boot_checkpoint("Preparing display buffers (pre-copy)");
    // Temporary: clear buffers instead of copying mario_packed_160x144
    memset(packed_buffer_0, 0xFF, PACKED_FRAME_SIZE);
    boot_checkpoint("Display buffer 0 clear complete");
    memset(packed_buffer_1, 0xFF, PACKED_FRAME_SIZE);
    boot_checkpoint("Display buffer 1 clear complete");
    packed_display_ptr = packed_buffer_0;
    packed_render_ptr = packed_buffer_1;
    boot_checkpoint("Display buffers primed");

    boot_checkpoint("Calling mount_sd_card");
    bool sd_mount_ok = mount_sd_card();
    boot_checkpoint("mount_sd_card returned");
    log_free_heap("after mount_sd_card");

    bool rom_list_ready = false;
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

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    boot_checkpoint("DVI configured");
    log_free_heap("after DVI init");

    set_game_palette(SCHEME_SGB_4H);

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
    memset(audio_buffer, 0, sizeof(audio_buffer));
    int cts = dvi0.timing->bit_clk_khz * hdmi_n[offset] / (rate / 100) / 128;
    dvi_get_blank_settings(&dvi0)->top = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, rate, cts, hdmi_n[offset]);
    increase_write_pointer(&dvi0.audio_ring, AUDIO_BUFFER_SIZE / 2);
    printf("Audio buffer pre-filled to %d samples (50%%)\n", AUDIO_BUFFER_SIZE / 2);
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
    // Reset and prime audio ring with a few frames to avoid startup underflow on some boots.
    set_read_offset(&dvi0.audio_ring, 0);
    set_write_offset(&dvi0.audio_ring, 0);
    memset(audio_buffer, 0, sizeof(audio_buffer));
    increase_write_pointer(&dvi0.audio_ring, AUDIO_BUFFER_SIZE / 2);
    for (int i = 0; i < 4; ++i) {
        pump_audio_samples();
    }
#endif

    absolute_time_t next_frame_time = make_timeout_time_us(DMG_FRAME_DURATION_US);

    uint32_t frame_counter = 0;

    while (true)
    {
        nes_classic_controller();
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
    #endif
        swap_display_buffers();
        handle_palette_hotkeys();

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

static void initialize_gpio(void)
{    
    //Onboard LED
    gpio_init(ONBOARD_LED_PIN);
    gpio_set_dir(ONBOARD_LED_PIN, GPIO_OUT);
    gpio_put(ONBOARD_LED_PIN, 0);

    //Initialize I2C port at 400 kHz
    i2c_init(i2cHandle, 400 * 1000);

    // Initialize I2C pins
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SCL_PIN);
    gpio_pull_up(SDA_PIN);
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

    return true;
}

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

static void sd_stream_chunk_yield(void)
{
    // Allow other subsystems (controller polling, audio) to make progress between SD chunks.
#if ENABLE_AUDIO
    pump_audio_samples();
#endif
    tight_loop_contents();
}
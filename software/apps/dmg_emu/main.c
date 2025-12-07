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
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/platform.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "audio_ring.h"  // For get_write_size and get_read_size

#include "mario.h"

#include "colors.h"
#include "hedley.h"
#include "board_pins.h"
#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"
#include "diskio.h"

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
#define ENABLE_OSD                  0  // Set to 1 to enable OSD code, 0 to disable (TODO)

#define MAX_SD_ROM_FILE_BYTES       (ROM_BANK_SIZE * 512u)
#define MAX_SD_ROM_HEAP_BYTES       (192u * 1024u)
#define SD_HEAP_SAFETY_MARGIN_BYTES (32u * 1024u)
#define SD_ROM_CACHE_SLOTS          2u
#define SD_STREAM_CHUNK_BYTES       256u


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
static bool discover_sd_rom(void);
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

    if (sd_filesystem_ready && mounted_sd_card != NULL && mounted_sd_card->mounted) {
        printf("SD mount skipped: already mounted.\n");
        return true;
    }

    sd_card_t *card = sd_get_by_num(0);
    if (card == NULL) {
        printf("SD mount skipped: no slot configured\n");
        return false;
    }

    printf("Initializing SD card interface...\n");
    set_spi_dma_irq_channel(true, true);

    int status = sd_init(card);
    if (status & STA_NODISK) {
        printf("SD mount failed: no card detected (status=0x%02x)\n", status);
        return false;
    }

    if (status & STA_NOINIT) {
        printf("SD mount failed: card did not initialize (status=0x%02x)\n", status);
        return false;
    }

    printf("Mounting FatFs volume on %s\n", card->pcName != NULL ? card->pcName : "<null>");

    FRESULT mount_result = f_mount(&card->fatfs, card->pcName, 1);
    if (mount_result != FR_OK) {
        printf("f_mount failed (%d: %s)\n", mount_result, FRESULT_str(mount_result));
        card->mounted = false;
        mounted_sd_card = NULL;
        sd_filesystem_ready = false;
        return false;
    }

    if (card->pcName != NULL) {
        FRESULT chdrive_result = f_chdrive(card->pcName);
        if (chdrive_result != FR_OK) {
            printf("f_chdrive failed (%d: %s) for %s\n",
                   chdrive_result,
                   FRESULT_str(chdrive_result),
                   card->pcName);
            f_mount(NULL, card->pcName, 0);
            card->mounted = false;
            mounted_sd_card = NULL;
            sd_filesystem_ready = false;
            return false;
        }
    }

    FRESULT chdir_result = f_chdir("/");
    if (chdir_result != FR_OK) {
        printf("f_chdir('/') failed (%d: %s)\n", chdir_result, FRESULT_str(chdir_result));
        if (card->pcName != NULL) {
            f_mount(NULL, card->pcName, 0);
        }
        card->mounted = false;
        mounted_sd_card = NULL;
        sd_filesystem_ready = false;
        return false;
    }

    card->mounted = true;
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

static bool find_first_rom_in_directory(const char *directory, char *out_path, size_t out_len)
{
    if ((directory == NULL) || (out_path == NULL) || (out_len == 0)) {
        return false;
    }

    size_t dir_len = strlen(directory);
    bool directory_has_sep = (dir_len > 0) && (directory[dir_len - 1] == '/' || directory[dir_len - 1] == '\\');

    DIR dir;
    FILINFO info;
    memset(&info, 0, sizeof(info));
    FRESULT fr = f_opendir(&dir, directory);
    if (fr != FR_OK) {
        return false;
    }

    bool found = false;
    while (true) {
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

        const char *fmt = directory_has_sep ? "%s%s" : "%s/%s";
        int needed = snprintf(out_path, out_len, fmt, directory, name);
        if ((needed > 0) && ((size_t)needed < out_len)) {
            found = true;
        }
        break;
    }

    FRESULT close_result = f_closedir(&dir);
    if ((close_result != FR_OK) && !found) {
        printf("f_closedir failed (%d: %s) while scanning %s\n", close_result, FRESULT_str(close_result), directory);
    }
    return found;
}

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

static void dump_sd_rom_inventory(void)
{
    if (!sd_filesystem_ready) {
        printf("ROM scan skipped: filesystem not ready\n");
        return;
    }

    const char *search_paths[] = {
        "0:/ROMS",
        "0:/"
    };

    for (size_t i = 0; i < ARRAY_SIZE(search_paths); ++i) {
        log_roms_in_directory(search_paths[i]);
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
    while (remaining > 0) {
        size_t chunk = (remaining > SD_STREAM_CHUNK_BYTES) ? SD_STREAM_CHUNK_BYTES : remaining;
        UINT chunk_read = 0;
        fr = f_read(&sd_rom_stream.handle, dst, (UINT)chunk, &chunk_read);
        if ((fr != FR_OK) || (chunk_read != chunk)) {
            printf("SD ROM cache read failed (bank=%lu chunk=%u): %s (%d)\n",
                   (unsigned long)bank_index, (unsigned int)chunk, FRESULT_str(fr), fr);
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
            return slot->data[bank_offset];
        }
    }

    sd_rom_cache_slot_t *slot = &sd_rom_cache[sd_rom_stream.next_replace_slot];
    if (!sd_stream_load_bank(bank_index, slot)) {
        return 0xFF;
    }
    sd_rom_stream.next_replace_slot = (sd_rom_stream.next_replace_slot + 1u) % SD_ROM_CACHE_SLOTS;

    if (bank_offset >= slot->bytes_valid) {
        return 0xFF;
    }
    return slot->data[bank_offset];
}

static bool discover_sd_rom(void)
{
    if (!sd_filesystem_ready) {
        return false;
    }

    dump_sd_rom_inventory();

    const char *search_paths[] = {
        "0:/ROMS",
        "0:/"
    };

    for (size_t i = 0; i < ARRAY_SIZE(search_paths); ++i) {
        if (find_first_rom_in_directory(search_paths[i], sd_rom_path, sizeof(sd_rom_path))) {
            printf("Found SD ROM: %s\n", sd_rom_path);
            sd_rom_discovered = true;
            boot_checkpoint("discover_sd_rom about to return true");
            return true;
        }
    }

    printf("No .gb/.gbc files found on SD card (checked /ROMS and root).\n");
    sd_rom_discovered = false;
    sd_rom_path[0] = '\0';
    return false;
}

static void reset_active_rom_to_builtin(void)
{
    close_sd_rom_stream();
    active_rom_data = ACTIVE_ROM_DATA;
    active_rom_length = ACTIVE_ROM_LEN;
    active_rom_source = ROM_SOURCE_BUILTIN;
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
    size_t approx_free_heap = estimate_free_heap_bytes();
    bool rom_within_heap_limit = (rom_size_bytes <= MAX_SD_ROM_HEAP_BYTES);
    size_t required_with_margin = rom_size_bytes + SD_HEAP_SAFETY_MARGIN_BYTES;
    bool heap_headroom_ok = approx_free_heap > required_with_margin;

    if (!rom_within_heap_limit) {
        printf("SD ROM heap load skipped: %lu bytes exceeds limit (%u)\n",
               (unsigned long)rom_size_bytes, (unsigned int)MAX_SD_ROM_HEAP_BYTES);
    }

    if (rom_within_heap_limit && heap_headroom_ok) {
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
    } else if (rom_within_heap_limit) {
        printf("SD ROM heap load skipped: need ~%lu bytes (incl. margin), free ~%lu\n",
               (unsigned long)required_with_margin,
               (unsigned long)approx_free_heap);
    }

    sd_rom_stream.bank_count = (uint32_t)((sd_rom_stream.size_bytes + ROM_BANK_SIZE - 1u) / ROM_BANK_SIZE);
    if (sd_rom_stream.bank_count == 0) {
        printf("SD ROM load aborted: unable to determine bank count\n");
        close_sd_rom_stream();
        return false;
    }
    boot_checkpoint("SD ROM stream initialized");

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

    if (rom0_bytes < sizeof(rom_bank0)) {
        memset(rom_bank0 + rom0_bytes, 0xFF, sizeof(rom_bank0) - rom0_bytes);
    }

    active_rom_source = ROM_SOURCE_SD_STREAM;
    active_rom_data = NULL;
    active_rom_length = sd_rom_stream.size_bytes;

    // printf("Streaming SD ROM (%lu bytes) from %s\n", (unsigned long)sd_rom_stream.size_bytes, path);

    // uint8_t probe = sd_stream_read_byte(0x4000);
    // printf("[SD] Bank1 probe = 0x%02x\n", probe);
    /* temporarily disable USB flush here */

    boot_checkpoint("load_sd_rom_file about to return true");
    return true;
}


static void initialize_gpio(void);

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

    if (!sd_mount_ok) {
        printf("Continuing with built-in ROM image (SD unavailable).\n");
    } else {
        if (discover_sd_rom()) {
            boot_checkpoint("discover_sd_rom returned true");
            printf("About to load SD ROM via load_sd_rom_file(): %s\n", sd_rom_path);
            if (load_sd_rom_file(sd_rom_path)) {
                boot_checkpoint("load_sd_rom_file returned true");
                printf("load_sd_rom_file success for %s\n", sd_rom_path);
                boot_checkpoint("SD ROM loaded into memory");
            } else {
                printf("SD ROM load failed - reverting to built-in image.\n");
                reset_active_rom_to_builtin();
            }
        } else {
            boot_checkpoint("discover_sd_rom returned false");
            printf("No SD ROM discovered - using built-in image.\n");
        }
    }
    boot_checkpoint("SD stage complete");

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    boot_checkpoint("DVI configured");

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

    boot_checkpoint("Initializing GPIO");
    initialize_gpio();
    boot_checkpoint("GPIO initialized");

    if (!init_peanut_emulator()) {
        while (1) {
            tight_loop_contents();
        }
    }
    boot_checkpoint("Peanut-GB initialized");
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
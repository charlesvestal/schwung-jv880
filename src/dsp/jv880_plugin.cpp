/*
 * Mini-JV Plugin for Schwung
 * Based on mini-jv880 emulator by giulioz (based on Nuked-SC55 by nukeykt)
 * Multi-expansion support with unified patch list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <pwd.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>

#include "mcu.h"
extern "C" {
#include "resample/libresample.h"
}
/* Fixed-ratio 64000->44100 polyphase resampler (replaces libresample in the
 * v2 emu thread path; libresample left vendored/included for v1 or reference). */
#include "resampler_fixed.h"

extern "C" {
#include "plugin_api_v1.h"
}

/* Debug logging - disabled in release builds */
static void jv_debug(const char *fmt, ...) {
    (void)fmt;
}

/* ========================================================================
 * PERFORMANCE STATS INSTRUMENTATION
 * Set to 1 to enable instrumentation (per-iteration clock_gettime, a 15s
 * stderr/perf_stats.txt report, and the "perf_stats" get_param); 0 compiles
 * it all out for zero overhead.  Ship with this OFF.
 * ======================================================================== */
#define JV880_PERF_STATS 0

/* Patch data constants */
#define PATCH_SIZE 0x16a  /* 362 bytes per patch */
#define PATCH_NAME_LEN 12
#define PATCH_OFFSET_INTERNAL   0x008ce0  /* Internal bank (JV Strings, etc.) */
#define PATCH_OFFSET_PRESET_A   0x010ce0  /* Preset A (A.Piano 1, etc.) */
#define PATCH_OFFSET_PRESET_B   0x018ce0  /* Preset B (Pizzicato, etc.) */
#define NVRAM_PATCH_OFFSET      0x0d70    /* Working patch area (362 bytes) */
#define NVRAM_MODE_OFFSET       0x11
#define NVRAM_PATCH_INTERNAL    0x1000    /* User patch storage: 64 × 362 = 23168 bytes (up to 0x6A80) */
#define NUM_USER_PATCHES        64

/* Performance data constants
 * Performance structure is 204 bytes (0xCC)
 * 16 performances per bank, name is first 12 bytes
 * Preset A/B are in ROM2, Internal is in NVRAM
 * Internal performances (16 × 0xCC = 0xCC0) end at 0x0d70 (temp patch)
 */
#define PERF_SIZE 0xCC   /* 204 bytes per performance */
#define PERF_NAME_LEN 12
#define PERFS_PER_BANK 16
/* ROM2 offsets for preset performances */
#define PERF_OFFSET_PRESET_A    0x10020  /* Preset A: "Jazz Split", "Softly...", etc. */
#define PERF_OFFSET_PRESET_B    0x18020  /* Preset B: "GTR Players", etc. */
/* NVRAM offset for internal performances */
#define NVRAM_PERF_INTERNAL     0x00b0   /* Internal: "Syn Lead", "Encounter X", etc. */
/* SRAM offset for temp performance (discovered via scanning) */
#define SRAM_TEMP_PERF_OFFSET   0x206a   /* Temp performance buffer in SRAM */

/* Temp performance structure offsets (discovered via automated mapping):
 *   0-11:  Name (12 bytes)
 *   12:    Key mode (packed with other flags)
 *   14:    Reverb time
 *   15:    Reverb feedback
 *   16:    Chorus level
 *   17:    Chorus depth
 *   18:    Chorus rate
 *   19:    Chorus feedback
 *   20-24: Voice reserve 1-5
 *   28+:   Part data (8 parts × 22 bytes each)
 *
 * Part structure (22 bytes per part, offset from part base):
 *   +0:  Flags (transmit switch/channel/output packed)
 *   +4:  Transmit key range lower
 *   +6:  Transmit key transpose
 *   +7:  Transmit velocity sense
 *   +8:  Transmit velocity max
 *   +9:  Transmit velocity curve
 *   +10: Internal key range lower
 *   +12: Internal key transpose
 *   +13: Internal velocity sense
 *   +14: Internal velocity max
 *   +15: Internal velocity curve
 *   +17: Part level
 *   +18: Part pan
 *   +19: Part coarse tune
 *   +20: Part fine tune
 *   +21: Receive channel
 */
#define TEMP_PERF_COMMON_SIZE   28   /* Bytes before part data starts */
#define TEMP_PERF_PART_SIZE     22   /* Bytes per part (discovered) */

/* Expansion ROM support */
#define EXPANSION_SIZE_8MB 0x800000  /* 8MB standard */
#define EXPANSION_SIZE_2MB 0x200000  /* 2MB (Experience series) */
#define MAX_EXPANSIONS 32
#define MAX_PATCHES_PER_EXP 256

typedef struct {
    char filename[256];
    char name[64];          /* Short name like "01 Pop" */
    int patch_count;
    uint32_t patches_offset;
    int first_global_index; /* First patch index in unified list */
    uint32_t rom_size;      /* ROM size (8MB or 2MB) */
    uint8_t *unscrambled;   /* Unscrambled ROM data (loaded on demand) */
} ExpansionInfo;

/* Unified patch list.
 *
 * Size is determined at runtime from scanned content rather than capped at
 * a compile-time maximum. Total = 3 internal banks (Preset A/B + Internal,
 * 64 patches each = 192 fixed by the JV-880 ROM layout) plus the sum of
 * each expansion's declared patch_count (each card contributes
 * MAX_PATCHES_PER_EXP at most, MAX_EXPANSIONS cards at most).
 *
 * inst->patches is heap-allocated in v2_build_patch_list (after scanning
 * expansion headers so the exact total is known) or v2_load_cache (using
 * the total_patches stored in the cache header), and freed in
 * v2_destroy_instance. */
typedef struct {
    char name[PATCH_NAME_LEN + 1];
    int expansion_index;    /* -1 for internal, 0+ for expansion */
    int local_patch_index;  /* Index within bank/expansion */
    uint32_t rom_offset;    /* Offset in ROM2 or expansion ROM */
} PatchInfo;

/* Performance mode constants */
#define NUM_PERF_BANKS 3
#define NUM_PERFORMANCES (NUM_PERF_BANKS * PERFS_PER_BANK)  /* 48 total */

/* Bank navigation */
#define MAX_BANKS 64

/* Progressive loading state machine */
enum LoadingPhase {
    PHASE_INIT = 0,
    PHASE_CHECK_CACHE,
    PHASE_SCAN_EXPANSION,
    PHASE_BUILD_PATCHES,
    PHASE_WARMUP,
    PHASE_COMPLETE
};

/* Cache file structure */
#define CACHE_MAGIC 0x4A563838  /* "JV88" */
#define CACHE_VERSION 2  /* v2: added rom_size field */
#define CACHE_FILENAME "patch_cache.bin"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t rom1_size;
    uint32_t rom2_size;
    uint32_t waverom1_size;
    uint32_t waverom2_size;
    uint32_t expansion_count;
    uint32_t total_patches;
    uint32_t bank_count;
} CacheHeader;

/* Expansion file list for fingerprinting */
#define MAX_EXP_FILES 64

/* Parameter mapping constants */
#define MAP_SRAM_SCAN_SIZE 512  /* Bytes to scan around temp perf */

/* Audio ring buffer size */
#define AUDIO_RING_SIZE 512

/* MIDI queue sizes */
#define MIDI_QUEUE_SIZE 256
#define MIDI_MSG_MAX_LEN 256

/* Sample rates */
/* JV-880 PCM runs at 64 kHz with oversampling enabled. */
#define JV880_SAMPLE_RATE 64000
#define MOVE_SAMPLE_RATE 44100


/* Output gain (reduce to prevent clipping) */
#define OUTPUT_GAIN_SHIFT 1  /* -6dB headroom to prevent clipping on hot patches */

/* ========================================================================
 * TONE PARAMETER LOOKUP TABLE
 * Sorted alphabetically for binary search
 * ======================================================================== */

enum ToneParamType {
    TP_BYTE,        /* Simple byte read/write */
    TP_BITFIELD,    /* Bitfield within a byte */
    TP_BOOL,        /* Boolean (On/Off) as bit */
    TP_ENUM         /* Enum with string options */
};

typedef struct {
    const char *name;
    int nvram_offset;   /* Offset within tone (84 bytes) */
    int sysex_idx;      /* SysEx parameter index */
    enum ToneParamType type;
    int bit_shift;      /* For bitfields: bit position */
    int bit_mask;       /* For bitfields: mask after shift */
    int two_byte;       /* 1 if param needs 2-byte nibblized SysEx */
    int signed_param;   /* 1 if param is signed (needs +64 for SysEx, read as int8_t) */
} ToneParamEntry;

/* Sorted alphabetically for binary search
 * two_byte=1 for params needing 2-byte nibblized SysEx:
 *   wavenumber (0x01), lfo1delay (0x26), lfo2delay (0x31), pan (0x5e), tonedelaytime (0x62)
 * signed_param=1 for params needing +64 offset in SysEx and int8_t read:
 *   LFO depths, pitch coarse/fine, envelope depths/levels, velocity senses, pan
 */
static const ToneParamEntry TONE_PARAMS[] = {
    /*                  name,                     nvram, sysex, type,        shift, mask, 2byte, signed */
    {"chorussendlevel",                             83,   114, TP_BYTE,        0,    0,     0,     0},
    {"cutofffrequency",                             52,    74, TP_BYTE,        0,    0,     0,     0},
    {"cutoffkeyfollow",                             54,    77, TP_BYTE,        0,    0,     0,     0},
    {"drylevel",                                    81,   112, TP_BYTE,        0,    0,     0,     0},
    {"filtermode",                                  55,    73, TP_BITFIELD,    3, 0x03,     0,     0},
    {"fxmdepth",                                     2,     5, TP_BITFIELD,    0, 0x0F,     0,     0},  /* value-1 handled specially */
    {"fxmswitch",                                    2,     4, TP_BOOL,        7, 0x01,     0,     0},
    {"level",                                       67,    92, TP_BYTE,        0,    0,     0,     0},
    {"levelkeyfollow",                              70,    93, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"lfo1delay",                                   25,    38, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
    {"lfo1fadepolarity",                            23,    40, TP_BOOL,        7, 0x01,     0,     0},
    {"lfo1fadetime",                                26,    41, TP_BYTE,        0,    0,     0,     0},
    {"lfo1form",                                    23,    34, TP_BITFIELD,    0, 0x07,     0,     0},
    {"lfo1offset",                                  23,    35, TP_BITFIELD,    3, 0x07,     0,     0},
    {"lfo1pitchdepth",                              31,    42, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo1rate",                                    24,    37, TP_BYTE,        0,    0,     0,     0},
    {"lfo1synchro",                                 23,    36, TP_BOOL,        6, 0x01,     0,     0},
    {"lfo1tvadepth",                                33,    44, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo1tvfdepth",                                32,    43, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo2delay",                                   29,    49, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
    {"lfo2fadetime",                                30,    52, TP_BYTE,        0,    0,     0,     0},
    {"lfo2form",                                    27,    45, TP_BITFIELD,    0, 0x07,     0,     0},
    {"lfo2pitchdepth",                              34,    53, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo2rate",                                    28,    48, TP_BYTE,        0,    0,     0,     0},
    {"lfo2tvadepth",                                36,    55, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"lfo2tvfdepth",                                35,    54, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"pan",                                         68,    94, TP_BYTE,        0,    0,     1,     2},  /* 2-byte, special: stored with +64 */
    {"panningkeyfollow",                            39,    96, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"penvdepth",                                   43,    64, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel1",                                  45,    66, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel2",                                  47,    68, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel3",                                  49,    70, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvlevel4",                                  51,    72, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvtime1",                                   44,    65, TP_BYTE,        0,    0,     0,     0},
    {"penvtime2",                                   46,    67, TP_BYTE,        0,    0,     0,     0},
    {"penvtime3",                                   48,    69, TP_BYTE,        0,    0,     0,     0},
    {"penvtime4",                                   50,    71, TP_BYTE,        0,    0,     0,     0},
    {"penvtimekeyfollow",                           40,    63, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"penvvelocitylevelsense",                      41,    60, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"penvvelocityofftimesense",                    42,    62, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"penvvelocityontimesense",                     42,    61, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"pitchcoarse",                                 37,    56, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"pitchfine",                                   38,    57, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"pitchkeyfollow",                              40,    59, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"randompitchdepth",                            39,    58, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"resonance",                                   53,    75, TP_BYTE,        0,    0,     0,     0},
    {"resonancemode",                               53,    76, TP_BOOL,        7, 0x01,     0,     0},
    {"reverbsendlevel",                             82,   113, TP_BYTE,        0,    0,     0,     0},
    {"tonedelaymode",                               71,    97, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"tonedelaytime",                               69,    98, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
    {"toneswitch",                                   0,     3, TP_BOOL,        7, 0x01,     0,     0},
    {"tvaenvlevel1",                                75,   106, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvlevel2",                                77,   108, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvlevel3",                                79,   110, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime1",                                 74,   105, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime2",                                 76,   107, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime3",                                 78,   109, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtime4",                                 80,   111, TP_BYTE,        0,    0,     0,     0},
    {"tvaenvtimekeyfollow",                         70,   104, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvaenvvelocitycurve",                         71,   100, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvaenvvelocitylevelsense",                    72,   101, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"tvaenvvelocityofftimesense",                  73,   103, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvaenvvelocityontimesense",                   73,   102, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"tvfenvdepth",                                 58,    83, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"tvfenvlevel1",                                60,    85, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvlevel2",                                62,    87, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvlevel3",                                64,    89, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvlevel4",                                66,    91, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime1",                                 59,    84, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime2",                                 61,    86, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime3",                                 63,    88, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtime4",                                 65,    90, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvtimekeyfollow",                         54,    82, TP_BYTE,        0,    0,     0,     0},
    {"tvfenvvelocitycurve",                         55,    78, TP_BITFIELD,    0, 0x07,     0,     0},
    {"tvfenvvelocitylevelsense",                    56,    79, TP_BYTE,        0,    0,     0,     1},  /* signed */
    {"tvfenvvelocityofftimesense",                  57,    81, TP_BITFIELD,    4, 0x0F,     0,     0},
    {"tvfenvvelocityontimesense",                   57,    80, TP_BITFIELD,    0, 0x0F,     0,     0},
    {"velocityrangelower",                           3,     6, TP_BYTE,        0,    0,     0,     0},
    {"velocityrangeupper",                           4,     7, TP_BYTE,        0,    0,     0,     0},
    {"wavegroup",                                    0,     0, TP_BITFIELD,    0, 0x03,     0,     0},
    {"wavenumber",                                   1,     1, TP_BYTE,        0,    0,     1,     0},  /* 2-byte */
};
#define NUM_TONE_PARAMS (sizeof(TONE_PARAMS) / sizeof(TONE_PARAMS[0]))

/* Binary search for tone param */
static const ToneParamEntry* find_tone_param(const char *name) {
    int lo = 0, hi = NUM_TONE_PARAMS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(name, TONE_PARAMS[mid].name);
        if (cmp == 0) return &TONE_PARAMS[mid];
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* ========================================================================
 * MACRO ABSTRACTION
 *
 * All 6 cross-tone macros funnel through exactly one read/write pair
 * (macro_read / macro_write, defined later once the instance type and the
 * per-tone write helper are visible). The behavior is selected by a single
 * runtime mode:
 *
 *   MACRO_ABSOLUTE_ANCHORED  (default) - display = anchor tone's stored value
 *       (anchor = lowest-numbered enabled tone, else tone 0); writing applies
 *       delta = new - anchor_value to every enabled tone via the normal
 *       nvram_tone_* write path. No hidden state; survives patch reload only
 *       as ordinary tone edits; do_save_to_slot captures it.
 *
 *   MACRO_RELATIVE_RESET - the legacy behavior: an offset stored per macro in
 *       the instance, applied (non-destructively) over the tones' base values
 *       via SysEx, and cleared on program change.
 *
 * Acceptance: switching modes is a one-string change with no other code or
 * table edits. macro_def_t below is the single source of truth for the 6 macros.
 * ======================================================================== */

typedef enum {
    MACRO_ABSOLUTE_ANCHORED,
    MACRO_RELATIVE_RESET
} macro_mode_t;

#define MACRO_MODE_DEFAULT MACRO_ABSOLUTE_ANCHORED

typedef struct {
    const char *key;            /* macro key suffix, e.g. "cutoff" (full key: macro_cutoff) */
    const char *display_name;   /* short UI label */
    const char *tone_param;     /* backing nvram_tone_* parameter name */
    int min;                    /* display/clamp min */
    int max;                    /* display/clamp max */
} macro_def_t;

/* The 6 macros. Ranges match the backing tone params:
 *   cutofffrequency/resonance/tvaenvtime{1,2,4}: unsigned 0..127
 *   tvfenvdepth: signed -63..63
 */
static const macro_def_t MACRO_DEFS[] = {
    {"cutoff",        "Cutoff",      "cutofffrequency",   0, 127},
    {"resonance",     "Resonance",   "resonance",         0, 127},
    {"attack",        "Attack",      "tvaenvtime1",       0, 127},
    {"decay",         "Decay",       "tvaenvtime2",       0, 127},
    {"sustain",       "Sustain",     "tvaenvlevel3",      0, 127},
    {"release",       "Release",     "tvaenvtime4",       0, 127},
    {"tvf_env_depth", "Filter Env",  "tvfenvdepth",     -63,  63},
    {"lfo_depth",     "LFO Depth",   "lfo1tvfdepth",    -63,  63},
};
#define NUM_MACROS (sizeof(MACRO_DEFS) / sizeof(MACRO_DEFS[0]))

static int clamp_int(int value, int min_val, int max_val);  /* defined below */

static const macro_def_t* find_macro(const char *key_suffix) {
    for (size_t i = 0; i < NUM_MACROS; i++) {
        if (strcmp(key_suffix, MACRO_DEFS[i].key) == 0) return &MACRO_DEFS[i];
    }
    return NULL;
}

/* ---- Patch Common parameters (FX + control section) -------------------
 * One table drives get, set, and state serialization. Each entry maps a
 * nvram_patchCommon_<name> param to a byte offset within the 362-byte patch
 * + a bitfield (shift,width), plus the Roland Patch-Common SysEx parameter
 * index (DT1 to 00 08 20 <sysexIdx>). Several params share an NVRAM byte
 * (offset 12 packs reverb/chorus type + velocity switch; offset 24 packs
 * bend-up/portamento/key-assign/solo-legato; offset 25 packs porta time +
 * type) — set does read-modify-write to preserve siblings, while each
 * sub-param has its OWN SysEx address so the firmware unpacks it.
 * Offsets/indices verified against jv880_juce dataStructures.h and the
 * existing 8 params (whose SysEx indices match this table exactly). */
typedef struct {
    const char *name;
    int sysexIdx;     /* 4th DT1 address byte under 00 08 20 */
    int nvramOffset;  /* byte offset within the patch */
    int shift;        /* bit shift within that byte */
    int width;        /* value bit width */
    int minv, maxv;
} PatchCommonParam;

static const PatchCommonParam PATCH_COMMON_PARAMS[] = {
    /* name,                sysex, off, sh, w, min, max */
    {"reverbtype",          0x0D,  12,  0,  4,  0,   7},
    {"chorustype",          0x11,  12,  4,  2,  0,   2},
    {"velocityswitch",      0x0C,  12,  7,  1,  0,   1},
    {"reverblevel",         0x0E,  13,  0,  7,  0, 127},
    {"reverbtime",          0x0F,  14,  0,  7,  0, 127},
    {"reverbfeedback",      0x10,  15,  0,  7,  0, 127},
    {"choruslevel",         0x12,  16,  0,  7,  0, 127},
    {"chorusoutput",        0x16,  16,  7,  1,  0,   1},
    {"chorusdepth",         0x13,  17,  0,  7,  0, 127},
    {"chorusrate",          0x14,  18,  0,  7,  0, 127},
    {"chorusfeedback",      0x15,  19,  0,  7,  0, 127},
    {"analogfeel",          0x17,  20,  0,  7,  0, 127},
    {"patchlevel",          0x18,  21,  0,  7,  0, 127},
    {"patchpan",            0x19,  22,  0,  7,  0, 127},
    {"bendrangedown",       0x1A,  23,  0,  7,  0,  48},
    {"bendrangeup",         0x1B,  24,  0,  4,  0,  12},
    {"portamentomode",      0x1F,  24,  4,  1,  0,   1},
    {"sololegato",          0x1D,  24,  5,  1,  0,   1},
    {"portamentoswitch",    0x1E,  24,  6,  1,  0,   1},
    {"keyassign",           0x1C,  24,  7,  1,  0,   1},
    {"portamentotime",      0x21,  25,  0,  7,  0, 127},
    {"portamentotype",      0x20,  25,  7,  1,  0,   1},
};
#define NUM_PATCH_COMMON_PARAMS (sizeof(PATCH_COMMON_PARAMS)/sizeof(PATCH_COMMON_PARAMS[0]))

static const PatchCommonParam* find_patch_common_param(const char *name) {
    for (size_t i = 0; i < NUM_PATCH_COMMON_PARAMS; i++)
        if (strcmp(PATCH_COMMON_PARAMS[i].name, name) == 0) return &PATCH_COMMON_PARAMS[i];
    return NULL;
}

/* ---- Per-tone control matrix (Mod/Aftertouch/Expression -> 4 dests) ----
 * JV-880 has, per tone, three controller sources (Modulation/CC1, Aftertouch,
 * Expression/CC11), each routing to 4 destination slots (A..D) with a signed
 * sensitivity. In NVRAM the 4 destinations are packed two-per-byte (DestAB:
 * low nibble=A, high=B; DestCD: low=C, high=D); the 4 sensitivities are
 * separate signed int8 bytes (center 0). On the SysEx wire each slot has its
 * OWN dest + sens address (interleaved), and sensitivity is sent +64.
 *   dest value: 0=Off 1=Pitch 2=Cutoff 3=Resonance 4=Level 5/6=LFO1/2->Pitch
 *               7/8=LFO1/2->TVF 9/10=LFO1/2->TVA 11/12=LFO1/2 Rate
 * Kept OUT of the binary-searched TONE_PARAMS table (separate linear lookup)
 * and handled in the tone get/set paths. Tone-relative offsets 5..22, SysEx
 * indices from jv880_juce EditToneTab.cpp. */
typedef struct {
    const char *name;
    int off;       /* tone-relative NVRAM byte offset */
    int shift;     /* nibble shift for dest (0 or 4); ignored for sens */
    int width;     /* bit width for dest (4); ignored for sens */
    int sysexIdx;  /* tone-area SysEx param index */
    int isSens;    /* 1 = signed sensitivity byte; 0 = nibble-packed dest */
} MatrixParam;

static const MatrixParam MATRIX_PARAMS[] = {
    /* name,      off, sh, w, sysex, sens */
    {"moddesta",   5, 0, 4, 0x0A, 0}, {"moddestb",  5, 4, 4, 0x0C, 0},
    {"moddestc",   6, 0, 4, 0x0E, 0}, {"moddestd",  6, 4, 4, 0x10, 0},
    {"modsensa",   7, 0, 0, 0x0B, 1}, {"modsensb",  8, 0, 0, 0x0D, 1},
    {"modsensc",   9, 0, 0, 0x0F, 1}, {"modsensd", 10, 0, 0, 0x11, 1},
    {"aftdesta",  11, 0, 4, 0x12, 0}, {"aftdestb", 11, 4, 4, 0x14, 0},
    {"aftdestc",  12, 0, 4, 0x16, 0}, {"aftdestd", 12, 4, 4, 0x18, 0},
    {"aftsensa",  13, 0, 0, 0x13, 1}, {"aftsensb", 14, 0, 0, 0x15, 1},
    {"aftsensc",  15, 0, 0, 0x17, 1}, {"aftsensd", 16, 0, 0, 0x19, 1},
    {"expdesta",  17, 0, 4, 0x1A, 0}, {"expdestb", 17, 4, 4, 0x1C, 0},
    {"expdestc",  18, 0, 4, 0x1E, 0}, {"expdestd", 18, 4, 4, 0x20, 0},
    {"expsensa",  19, 0, 0, 0x1B, 1}, {"expsensb", 20, 0, 0, 0x1D, 1},
    {"expsensc",  21, 0, 0, 0x1F, 1}, {"expsensd", 22, 0, 0, 0x21, 1},
};
#define NUM_MATRIX_PARAMS (sizeof(MATRIX_PARAMS)/sizeof(MATRIX_PARAMS[0]))

static const MatrixParam* find_matrix_param(const char *name) {
    for (size_t i = 0; i < NUM_MATRIX_PARAMS; i++)
        if (strcmp(MATRIX_PARAMS[i].name, name) == 0) return &MATRIX_PARAMS[i];
    return NULL;
}

/* ========================================================================
 * SHARED HELPER FUNCTIONS
 * ======================================================================== */

/* Case-insensitive check for .bin extension */
static int has_bin_extension(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".bin") == 0);
}

/* Unscramble SR-JV expansion ROM (from jv880_juce) */
static void unscramble_rom(const uint8_t *src, uint8_t *dst, int len) {
    for (int i = 0; i < len; i++) {
        int address = i & ~0xfffff;
        static const int aa[] = {2, 0, 3, 4, 1, 9, 13, 10, 18, 17,
                                 6, 15, 11, 16, 8, 5, 12, 7, 14, 19};
        for (int j = 0; j < 20; j++) {
            if (i & (1 << j))
                address |= 1 << aa[j];
        }
        uint8_t srcdata = src[address];
        uint8_t data = 0;
        static const int dd[] = {2, 0, 4, 5, 7, 6, 3, 1};
        for (int j = 0; j < 8; j++) {
            if (srcdata & (1 << dd[j]))
                data |= 1 << j;
        }
        dst[i] = data;
    }
}

/* Extract short name from filename like "SR-JV80-01_Pop.bin" -> "01 Pop" */
static void extract_expansion_name(const char *filename, char *name, int max_len) {
    /* Look for pattern SR-JV80-XX_Name.bin */
    const char *p = strstr(filename, "SR-JV80-");
    if (p) {
        p += 8;  /* Skip "SR-JV80-" */
        /* Copy number and name */
        int i = 0;
        while (*p && *p != '.' && i < max_len - 1) {
            if (*p == '_') {
                name[i++] = ' ';
            } else {
                name[i++] = *p;
            }
            p++;
        }
        name[i] = '\0';
    } else {
        strncpy(name, filename, max_len - 1);
        name[max_len - 1] = '\0';
    }
}

/* ========================================================================
 * PLUGIN API V2 - INSTANCE-BASED (for multi-instance support)
 *
 * Note: V1 API has been removed. V2 is required.
 *
 * Note: JV880 is extremely resource-intensive (emulator, ROMs, threads).
 * Multiple simultaneous instances are technically possible but may cause
 * performance issues on limited hardware.
 * ======================================================================== */

/* Tone param cache size - 4 tones × 84 bytes each. Per-instance (see struct). */
#define TONE_CACHE_SIZE (4 * 84)

/* v2 instance structure containing ALL state (for true multi-instance) */
typedef struct {
    /* Module path */
    char module_dir[512];

    /* Emulator */
    MCU *mcu;
    int initialized;
    int rom_loaded;

    /* ROM data */
    uint8_t *rom2;

    /* Debug/SysEx */
    int debug_sysex;
    uint8_t sysex_buf[512];
    int sysex_len;
    int sysex_capture;

    /* Expansions */
    ExpansionInfo expansions[MAX_EXPANSIONS];
    int expansion_count;
    int current_expansion;
    int expansion_bank_offset;

    /* Expansion file tracking */
    char expansion_files[MAX_EXP_FILES][256];
    uint32_t expansion_sizes[MAX_EXP_FILES];
    int expansion_file_count;

    /* Patches (heap-allocated, sized to actual content - see PatchInfo comment) */
    PatchInfo *patches;
    int total_patches;
    int current_patch;

    /* Banks */
    int bank_starts[MAX_BANKS];
    char bank_names[MAX_BANKS][64];
    int bank_count;

    /* Performance mode */
    int performance_mode;
    int current_performance;
    int current_part;
    int perf_bank;
    int pending_perf_select;   /* Countdown to select performance after mode switch */
    int pending_patch_select;  /* Countdown to select patch after mode switch */
    volatile int warmup_remaining;  /* Warmup cycles remaining after reset */
    int deferred_patch_index;      /* Patch index waiting for debounce to complete */
    int deferred_patch_countdown;  /* Render blocks remaining before executing deferred patch */

    /* Part patch bank selection: 0=User, 1=Internal, 2=PresetA, 3=PresetB */
    int part_patchbank[8];

    /* Save/load slot browser indices */
    int save_slot_index;
    int load_slot_index;

    /* Resampling state (libresample) - retained for v1 / reference; v2 path
     * now uses the fixed-ratio polyphase resampler below. */
    void *resampleL;
    void *resampleR;
    float resample_in_l[4096];  /* Input buffer for resampler */
    float resample_in_r[4096];
    float resample_out_l[4096]; /* Output buffer for resampler */
    float resample_out_r[4096];

    /* Fixed-ratio 64000->44100 polyphase resampler (v2 path).  Carries L+R
     * history and persistent fractional phase so chunked input across
     * iterations resamples correctly. */
    ResamplerFixed rfix;

    /* Loading state */
    char loading_status[256];
    int loading_complete;
    int loading_phase;
    int loading_subindex;
    int warmup_count;

    /* Parameter mapping */
    int map_active;
    int map_phase;
    int map_mode;
    int map_part;
    int map_param_idx;
    int map_wait_cycles;
    int map_test_pass;
    uint8_t map_sram_snapshot[MAP_SRAM_SCAN_SIZE];
    uint8_t map_sysex_pending[16];
    int map_sysex_len;
    int map_last_offset;

    /* SRAM scanning */
    int sram_scan_countdown;
    int found_perf_sram_offset;

    /* Threading */
    pthread_t emu_thread;
    volatile int thread_running;
    pthread_t load_thread;
    volatile int load_thread_running;

    /* Audio ring buffer */
    int16_t audio_ring[AUDIO_RING_SIZE * 2];
    volatile int ring_write;
    volatile int ring_read;
    pthread_mutex_t ring_mutex;

    /* MIDI queue */
    uint8_t midi_queue[MIDI_QUEUE_SIZE][MIDI_MSG_MAX_LEN];
    int midi_queue_len[MIDI_QUEUE_SIZE];
    volatile int midi_write;
    volatile int midi_read;

    /* Other settings */
    int octave_transpose;

    /* Deferred state restoration (applied after loading completes) */
    char pending_state[2048];
    int pending_state_valid;

    /* Error state */
    char load_error[256];

    /* Audio diagnostics */
    int underrun_count;
    int render_count;
    int min_buffer_level;

    /* Macro behavior selector (absolute-anchored vs relative-reset).
     * Runtime-switchable via set_param("macro_mode", "absolute"|"relative"). */
    macro_mode_t macro_mode;

    /* Macro offsets (used only in MACRO_RELATIVE_RESET mode; applied across all 4 tones) */
    int macro_cutoff;        /* offset for cutofffrequency */
    int macro_resonance;     /* offset for resonance */
    int macro_attack;        /* offset for tvaenvtime1 */
    int macro_decay;         /* offset for tvaenvtime2 */
    int macro_release;       /* offset for tvaenvtime4 */
    int macro_tvf_env_depth; /* offset for tvfenvdepth */
    int macro_sustain;       /* offset for tvaenvlevel3 */
    int macro_lfo_depth;     /* offset for lfo1tvfdepth */

    /* Subtractive link mode: when set, Tone 1 drives every shared (non-oscillator)
     * param across all four tones. Transient editing mode — reset on patch change. */
    int link_tones;

    /* Link-mode backup: holds the original shared (non-oscillator) bytes of
     * tones 2,3,4 as they were before link snapped Tone 1 over them, so the
     * toggle is reversible. Stored as the full 84-byte tone slice for tones
     * 2,3,4 (only the shared bytes are touched on restore via v2_snap_link_tones'
     * param set). link_backup_valid==1 means a valid snapshot is held. */
    uint8_t link_backup[3 * 84];
    int link_backup_valid;

    /* Per-instance tone param cache - avoid hitting NVRAM on every poll.
     * tone_cache_valid is the wall-clock timestamp (ms) when the cache was
     * filled; 0 means invalid. MUST be per-instance: a process-global cache
     * lets one JV-880 slot serve another slot's stale NVRAM bytes, corrupting
     * serialized patches across slots. */
    uint8_t tone_cache[TONE_CACHE_SIZE];
    uint64_t tone_cache_valid;

#if JV880_PERF_STATS
    /* Per-window accumulators (reset every 15-second report window) */
    uint64_t perf_ns_emu;         /* Thread CPU ns spent in updateSC55 */
    uint64_t perf_ns_resamp;      /* Thread CPU ns spent in convert+resample+ringwrite */
    uint64_t perf_ns_total;       /* Total thread CPU ns for all working iterations */
    uint64_t perf_loop_iters;     /* Working iterations in this window */
    uint64_t perf_sleep_iters;    /* Ring-full (sleep) iterations in this window */
    uint64_t perf_emu_calls;      /* updateSC55 calls in this window */
    /* Wall-clock deadline for next report (CLOCK_MONOTONIC ns) */
    uint64_t perf_next_report_ns;
    /* Wall-clock ns at start of this window (for thread_cpu%wall) */
    uint64_t perf_wall_window_ns;
    /* Thread CPU ns at start of this window */
    uint64_t perf_cpu_window_ns;
    /* MCU sleep-fraction probe: window-start snapshots of mcu counters */
    uint64_t perf_probe_steps0;
    uint64_t perf_probe_sleep0;
    /* Last formatted report line (returned by get_param "perf_stats") */
    char perf_stats_buf[256];
#endif /* JV880_PERF_STATS */
} jv880_instance_t;

/* Forward declarations for v2 helper functions */
static int v2_load_rom(jv880_instance_t *inst, const char *filename, uint8_t *dest, size_t size);
static void* v2_load_thread_func(void *arg);
static void* v2_emu_thread_func(void *arg);
/* Forward declarations for v2 expansion functions */
static void v2_scan_expansion_files(jv880_instance_t *inst);
static int v2_load_cache(jv880_instance_t *inst);
static void v2_save_cache(jv880_instance_t *inst);
static int v2_scan_expansion_rom(jv880_instance_t *inst, const char *filename, ExpansionInfo *info);
static void v2_scan_expansions(jv880_instance_t *inst);
static void v2_build_patch_list(jv880_instance_t *inst);
static int v2_load_expansion_data(jv880_instance_t *inst, int exp_index);
static void v2_load_expansion_to_emulator(jv880_instance_t *inst, int exp_index);
static void v2_select_patch(jv880_instance_t *inst, int global_index);
static void v2_select_performance(jv880_instance_t *inst, int perf_index);
static void v2_set_mode(jv880_instance_t *inst, int performance_mode);
static void v2_send_all_notes_off(jv880_instance_t *inst);
static void v2_set_param(void *instance, const char *key, const char *val);
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len);
static void v2_snap_link_tones(jv880_instance_t *inst);
static void v2_restore_link_tones(jv880_instance_t *inst);

/* v2: Get file size helper */
static uint32_t v2_get_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    fclose(f);
    return size;
}

/* v2: Scan for expansion ROM files */
static void v2_scan_expansion_files(jv880_instance_t *inst) {
    char exp_dir[1024];
    snprintf(exp_dir, sizeof(exp_dir), "%s/roms/expansions", inst->module_dir);

    inst->expansion_file_count = 0;

    DIR *dir = opendir(exp_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->expansion_file_count < MAX_EXP_FILES) {
        if (strstr(entry->d_name, "SR-JV80") && has_bin_extension(entry->d_name)) {
            strncpy(inst->expansion_files[inst->expansion_file_count], entry->d_name,
                    sizeof(inst->expansion_files[0]) - 1);

            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", exp_dir, entry->d_name);
            inst->expansion_sizes[inst->expansion_file_count] = v2_get_file_size(path);

            inst->expansion_file_count++;
        }
    }
    closedir(dir);

    /* Sort alphabetically */
    if (inst->expansion_file_count > 1) {
        for (int i = 0; i < inst->expansion_file_count - 1; i++) {
            for (int j = i + 1; j < inst->expansion_file_count; j++) {
                if (strcmp(inst->expansion_files[i], inst->expansion_files[j]) > 0) {
                    char tmp_name[256];
                    strcpy(tmp_name, inst->expansion_files[i]);
                    strcpy(inst->expansion_files[i], inst->expansion_files[j]);
                    strcpy(inst->expansion_files[j], tmp_name);
                    uint32_t tmp_size = inst->expansion_sizes[i];
                    inst->expansion_sizes[i] = inst->expansion_sizes[j];
                    inst->expansion_sizes[j] = tmp_size;
                }
            }
        }
    }
}

/* v2: Scan a single expansion ROM */
static int v2_scan_expansion_rom(jv880_instance_t *inst, const char *filename, ExpansionInfo *info) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", inst->module_dir, filename);

    snprintf(inst->loading_status, sizeof(inst->loading_status), "Scanning: %.40s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t rom_size = 0;
    if (size == EXPANSION_SIZE_8MB) {
        rom_size = EXPANSION_SIZE_8MB;
    } else if (size == EXPANSION_SIZE_2MB) {
        rom_size = EXPANSION_SIZE_2MB;
    } else {
        fprintf(stderr, "JV880 v2: Skipping %s (wrong size)\n", filename);
        fclose(f);
        return 0;
    }

    uint8_t *scrambled = (uint8_t *)malloc(rom_size);
    uint8_t *unscrambled_data = (uint8_t *)malloc(rom_size);
    if (!scrambled || !unscrambled_data) {
        free(scrambled);
        free(unscrambled_data);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, rom_size, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled_data, rom_size);
    free(scrambled);

    int patch_count = unscrambled_data[0x67] | (unscrambled_data[0x66] << 8);
    uint32_t patches_offset = unscrambled_data[0x8f] |
                              (unscrambled_data[0x8e] << 8) |
                              (unscrambled_data[0x8d] << 16) |
                              (unscrambled_data[0x8c] << 24);

    if (patch_count <= 0 || patch_count > MAX_PATCHES_PER_EXP || patches_offset >= rom_size) {
        fprintf(stderr, "JV880 v2: Invalid expansion %s\n", filename);
        free(unscrambled_data);
        return 0;
    }

    strncpy(info->filename, filename, sizeof(info->filename) - 1);
    extract_expansion_name(filename, info->name, sizeof(info->name));
    info->patch_count = patch_count;
    info->patches_offset = patches_offset;
    info->rom_size = rom_size;
    info->unscrambled = unscrambled_data;

    /* Debug: show header bytes and first patch preview */
    fprintf(stderr, "JV880 v2: Scanned %s: %d patches at offset 0x%x\n",
            info->name, patch_count, patches_offset);
    fprintf(stderr, "JV880 v2: Header bytes 0x66-0x67: %02X %02X, 0x8c-0x8f: %02X %02X %02X %02X\n",
            unscrambled_data[0x66], unscrambled_data[0x67],
            unscrambled_data[0x8c], unscrambled_data[0x8d],
            unscrambled_data[0x8e], unscrambled_data[0x8f]);

    /* Check first patch at patches_offset */
    char first_patch_name[13];
    memcpy(first_patch_name, &unscrambled_data[patches_offset], 12);
    first_patch_name[12] = '\0';
    fprintf(stderr, "JV880 v2: First patch at 0x%x: name='%s', byte26=0x%02X\n",
            patches_offset, first_patch_name, unscrambled_data[patches_offset + 26]);

    return 1;
}

/* v2: Compare expansions for sorting */
static int v2_compare_expansions(const void *a, const void *b) {
    return strcmp(((ExpansionInfo*)a)->name, ((ExpansionInfo*)b)->name);
}

/* v2: Scan all expansions */
static void v2_scan_expansions(jv880_instance_t *inst) {
    char exp_dir[1024];
    snprintf(exp_dir, sizeof(exp_dir), "%s/roms/expansions", inst->module_dir);

    DIR *dir = opendir(exp_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->expansion_count < MAX_EXPANSIONS) {
        if (strstr(entry->d_name, "SR-JV80") && has_bin_extension(entry->d_name)) {
            if (v2_scan_expansion_rom(inst, entry->d_name, &inst->expansions[inst->expansion_count])) {
                inst->expansion_count++;
            }
        }
    }
    closedir(dir);

    if (inst->expansion_count > 1) {
        qsort(inst->expansions, inst->expansion_count, sizeof(ExpansionInfo), v2_compare_expansions);
    }

    fprintf(stderr, "JV880 v2: Found %d expansions\n", inst->expansion_count);
}

/* v2: Build complete patch list with expansions */
static void v2_build_patch_list(jv880_instance_t *inst) {
    /* Compute exact total: 3 fixed internal banks of 64 patches (Preset A/B
     * + Internal, hard-coded by JV-880 ROM layout) plus each expansion's
     * declared patch_count, capped per-expansion at MAX_PATCHES_PER_EXP and
     * by available bank slots. */
    const int internal_total = 3 * 64;
    int expansion_total = 0;
    int expansion_banks_usable = 0;
    int banks_used_by_internal = 3;
    int banks_remaining = (MAX_BANKS > banks_used_by_internal)
                          ? (MAX_BANKS - banks_used_by_internal) : 0;
    for (int e = 0; e < inst->expansion_count && expansion_banks_usable < banks_remaining; e++) {
        expansion_total += inst->expansions[e].patch_count;
        expansion_banks_usable++;
    }

    int new_total = internal_total + expansion_total;

    free(inst->patches);
    inst->patches = (PatchInfo *)calloc(new_total > 0 ? new_total : 1, sizeof(PatchInfo));
    if (!inst->patches) {
        fprintf(stderr, "JV880 v2: Failed to allocate patches array (%d entries)\n", new_total);
        inst->total_patches = 0;
        inst->bank_count = 0;
        return;
    }

    inst->total_patches = 0;
    inst->bank_count = 0;

    /* Preset Bank A (0-63) */
    inst->bank_starts[inst->bank_count] = inst->total_patches;
    strncpy(inst->bank_names[inst->bank_count], "Preset A", sizeof(inst->bank_names[0]) - 1);
    inst->bank_count++;

    for (int i = 0; i < 64; i++) {
        PatchInfo *p = &inst->patches[inst->total_patches];
        uint32_t offset = PATCH_OFFSET_PRESET_A + (i * PATCH_SIZE);
        memcpy(p->name, &inst->rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';
        p->expansion_index = -1;
        p->local_patch_index = i;
        p->rom_offset = offset;
        inst->total_patches++;
    }

    /* Preset Bank B (64-127) */
    inst->bank_starts[inst->bank_count] = inst->total_patches;
    strncpy(inst->bank_names[inst->bank_count], "Preset B", sizeof(inst->bank_names[0]) - 1);
    inst->bank_count++;

    for (int i = 0; i < 64; i++) {
        PatchInfo *p = &inst->patches[inst->total_patches];
        uint32_t offset = PATCH_OFFSET_PRESET_B + (i * PATCH_SIZE);
        memcpy(p->name, &inst->rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';
        p->expansion_index = -1;
        p->local_patch_index = 64 + i;
        p->rom_offset = offset;
        inst->total_patches++;
    }

    /* Internal Bank (128-191) */
    inst->bank_starts[inst->bank_count] = inst->total_patches;
    strncpy(inst->bank_names[inst->bank_count], "Internal", sizeof(inst->bank_names[0]) - 1);
    inst->bank_count++;

    for (int i = 0; i < 64; i++) {
        PatchInfo *p = &inst->patches[inst->total_patches];
        uint32_t offset = PATCH_OFFSET_INTERNAL + (i * PATCH_SIZE);
        memcpy(p->name, &inst->rom2[offset], PATCH_NAME_LEN);
        p->name[PATCH_NAME_LEN] = '\0';
        p->expansion_index = -1;
        p->local_patch_index = 128 + i;
        p->rom_offset = offset;
        inst->total_patches++;
    }

    /* Expansion patches. Bounded by expansion_banks_usable so total_patches
     * stays consistent with the up-front allocation when MAX_BANKS would
     * otherwise force us to drop trailing expansions. */
    for (int e = 0; e < expansion_banks_usable; e++) {
        ExpansionInfo *exp = &inst->expansions[e];
        exp->first_global_index = inst->total_patches;

        inst->bank_starts[inst->bank_count] = inst->total_patches;
        strncpy(inst->bank_names[inst->bank_count], exp->name, sizeof(inst->bank_names[0]) - 1);
        inst->bank_count++;

        for (int i = 0; i < exp->patch_count; i++) {
            PatchInfo *p = &inst->patches[inst->total_patches];
            uint32_t offset = exp->patches_offset + (i * PATCH_SIZE);

            if (exp->unscrambled) {
                memcpy(p->name, &exp->unscrambled[offset], PATCH_NAME_LEN);
                p->name[PATCH_NAME_LEN] = '\0';
            } else {
                snprintf(p->name, sizeof(p->name), "Patch %d", i);
            }

            p->expansion_index = e;
            p->local_patch_index = i;
            p->rom_offset = offset;
            inst->total_patches++;
        }
    }

    fprintf(stderr, "JV880 v2: Total patches: %d (%d internal + %d expansion) in %d banks\n",
            inst->total_patches, internal_total, inst->total_patches - internal_total,
            inst->bank_count);
}

/* v2: Load expansion data on demand */
static int v2_load_expansion_data(jv880_instance_t *inst, int exp_index) {
    if (exp_index < 0 || exp_index >= inst->expansion_count) return 0;

    ExpansionInfo *exp = &inst->expansions[exp_index];
    if (exp->unscrambled) return 1;

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/expansions/%s", inst->module_dir, exp->filename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint8_t *scrambled = (uint8_t *)malloc(exp->rom_size);
    uint8_t *unscrambled_data = (uint8_t *)malloc(exp->rom_size);
    if (!scrambled || !unscrambled_data) {
        free(scrambled);
        free(unscrambled_data);
        fclose(f);
        return 0;
    }

    fread(scrambled, 1, exp->rom_size, f);
    fclose(f);

    unscramble_rom(scrambled, unscrambled_data, exp->rom_size);
    free(scrambled);

    exp->unscrambled = unscrambled_data;
    fprintf(stderr, "JV880 v2: Loaded expansion %s on demand\n", exp->name);
    return 1;
}

/* v2: Send all notes off */
static void v2_send_all_notes_off(jv880_instance_t *inst) {
    pthread_mutex_lock(&inst->ring_mutex);
    for (int ch = 0; ch < 16; ch++) {
        uint8_t msg[3] = { (uint8_t)(0xB0 | ch), 123, 0 };
        int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[inst->midi_write], msg, 3);
            inst->midi_queue_len[inst->midi_write] = 3;
            inst->midi_write = next;
        }
    }
    pthread_mutex_unlock(&inst->ring_mutex);
}

/* v2: Load expansion to emulator */
static void v2_load_expansion_to_emulator(jv880_instance_t *inst, int exp_index) {
    if (exp_index < 0 || exp_index >= inst->expansion_count) return;

    ExpansionInfo *exp = &inst->expansions[exp_index];

    /* Load expansion data if not already in memory */
    if (!exp->unscrambled) {
        if (!v2_load_expansion_data(inst, exp_index)) return;
    }

    /* Skip if this expansion is already loaded in the emulator */
    if (exp_index == inst->current_expansion) return;

    v2_send_all_notes_off(inst);

    /* Load waveform data to expansion waverom */
    memset(inst->mcu->pcm.waverom_exp, 0, EXPANSION_SIZE_8MB);
    memcpy(inst->mcu->pcm.waverom_exp, exp->unscrambled, exp->rom_size);

    /* Load patch definitions to cardram for Card patches (64-127 in Performance mode)
     * The JV-880 looks for Card patch data in cardram when a part uses patchnumber 64-127.
     * Copy up to 64 patches from the expansion's patch area. */
    memset(inst->mcu->cardram, 0, CARDRAM_SIZE);
    int patches_to_copy = (exp->patch_count > 64) ? 64 : exp->patch_count;
    int bytes_to_copy = patches_to_copy * PATCH_SIZE;
    if (bytes_to_copy > CARDRAM_SIZE) bytes_to_copy = CARDRAM_SIZE;
    memcpy(inst->mcu->cardram, &exp->unscrambled[exp->patches_offset], bytes_to_copy);

    /* Debug: show what we copied */
    jv_debug("[JV880] Expansion load: patches_offset=0x%x, patch_count=%d\n",
             exp->patches_offset, exp->patch_count);

    /* Show first patch's structure */
    uint8_t *p0 = inst->mcu->cardram;
    char name[13];
    memcpy(name, p0, 12);
    name[12] = '\0';
    jv_debug("[JV880] cardram patch 0: name='%s'\n", name);
    jv_debug("[JV880] cardram patch 0: bytes 0-15: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
             p0[0], p0[1], p0[2], p0[3], p0[4], p0[5], p0[6], p0[7],
             p0[8], p0[9], p0[10], p0[11], p0[12], p0[13], p0[14], p0[15]);
    jv_debug("[JV880] cardram patch 0: bytes 16-31: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
             p0[16], p0[17], p0[18], p0[19], p0[20], p0[21], p0[22], p0[23],
             p0[24], p0[25], p0[26], p0[27], p0[28], p0[29], p0[30], p0[31]);
    /* Tone 0 starts at offset 26. Show wavegroup and wavenumber */
    jv_debug("[JV880] cardram patch 0 tone0: wavegroup=%d wavenumber=%d,%d (at offsets 26,27,28)\n",
             p0[26], p0[27], p0[28]);

    /* Also show what's at those same offsets in the expansion ROM directly */
    uint8_t *rom_p0 = &exp->unscrambled[exp->patches_offset];
    jv_debug("[JV880] ROM patch 0: bytes 0-15: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
             rom_p0[0], rom_p0[1], rom_p0[2], rom_p0[3], rom_p0[4], rom_p0[5], rom_p0[6], rom_p0[7],
             rom_p0[8], rom_p0[9], rom_p0[10], rom_p0[11], rom_p0[12], rom_p0[13], rom_p0[14], rom_p0[15]);

    fprintf(stderr, "JV880 v2: Copied %d patches (%d bytes) to cardram\n",
            patches_to_copy, bytes_to_copy);

    inst->current_expansion = exp_index;

    /* Reset emulator when switching expansions in patch mode.
     * The emulator can't handle waveform ROM swaps with active voices.
     * Performance mode skips reset since Card patches handle it differently.
     * Warmup kept short (5000 cycles) - debounce prevents repeated resets. */
    if (!inst->performance_mode) {
        inst->mcu->SC55_Reset();
        inst->warmup_remaining = 1000;
        fprintf(stderr, "JV880 v2: Loaded expansion %s to emulator (with reset, short warmup)\n", exp->name);
    } else {
        fprintf(stderr, "JV880 v2: Loaded expansion %s for Card patches (no reset)\n", exp->name);
    }
}

/* v2: Select a patch */
static void v2_select_patch(jv880_instance_t *inst, int global_index) {
    jv_debug("[v2_select_patch] Called: global_index=%d\n", global_index);

    if (!inst || !inst->mcu) {
        jv_debug("[v2_select_patch] ERROR: inst=%p mcu=%p\n", (void*)inst, inst ? (void*)inst->mcu : NULL);
        return;
    }
    if (global_index < 0 || global_index >= inst->total_patches) {
        jv_debug("[v2_select_patch] ERROR: invalid index %d (total=%d)\n", global_index, inst->total_patches);
        return;
    }

    /* If in performance mode, switch to patch mode first.
     * Update current_patch BEFORE switching so the mode switch loads the right patch. */
    if (inst->performance_mode) {
        jv_debug("[v2_select_patch] In performance mode, setting current_patch=%d then switching to patch mode\n", global_index);
        inst->current_patch = global_index;  /* Set target patch before mode switch */
        v2_set_mode(inst, 0);  /* This will load inst->current_patch */
        return;
    }

    PatchInfo *p = &inst->patches[global_index];
    inst->current_patch = global_index;

    jv_debug("[v2_select_patch] Loading patch %d: %s (exp=%d rom_off=0x%x)\n",
            global_index, p->name, p->expansion_index, p->rom_offset);

    /* Load patch data to NVRAM */
    if (p->expansion_index >= 0) {
        v2_load_expansion_to_emulator(inst, p->expansion_index);
        ExpansionInfo *exp = &inst->expansions[p->expansion_index];
        if (exp->unscrambled) {
            memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                   &exp->unscrambled[p->rom_offset], PATCH_SIZE);
            jv_debug("[v2_select_patch] Copied expansion patch to NVRAM\n");
        }
    } else {
        memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET], &inst->rom2[p->rom_offset], PATCH_SIZE);
        jv_debug("[v2_select_patch] Copied internal patch to NVRAM\n");
    }

    /* Ensure patch mode in NVRAM */
    inst->mcu->nvram[NVRAM_MODE_OFFSET] = 1;
    jv_debug("[v2_select_patch] Set NVRAM mode=1 (patch)\n");

    /* Send PC 0 to trigger emulator to reload from NVRAM */
    uint8_t pc_msg[2] = { 0xC0, 0x00 };
    pthread_mutex_lock(&inst->ring_mutex);
    int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
        inst->midi_queue_len[inst->midi_write] = 2;
        inst->midi_write = next;
        jv_debug("[v2_select_patch] Queued PC: [0x%02x 0x%02x]\n", pc_msg[0], pc_msg[1]);
    } else {
        jv_debug("[v2_select_patch] ERROR: MIDI queue full!\n");
    }
    pthread_mutex_unlock(&inst->ring_mutex);

    /* Reset macro offsets on patch change */
    inst->macro_cutoff = 0;
    inst->macro_resonance = 0;
    inst->macro_attack = 0;
    inst->macro_decay = 0;
    inst->macro_release = 0;
    inst->macro_tvf_env_depth = 0;
    inst->macro_sustain = 0;
    inst->macro_lfo_depth = 0;

    /* Subtractive link is an editing overlay on the live patch; a freshly loaded
     * patch must arrive intact, so always drop link on patch change (decision:
     * auto-disable, don't overwrite the new patch's per-tone data). Also drop any
     * stale link backup so a later toggle can't clobber the new patch's tones. */
    inst->link_tones = 0;
    inst->link_backup_valid = 0;

    jv_debug("[v2_select_patch] Complete\n");
}

static void chown_to_ableton(const char *path) {
    struct passwd *pw = getpwnam("ableton");
    if (pw) chown(path, pw->pw_uid, pw->pw_gid);
}

/* v2: Save cache */
static void v2_save_cache(jv880_instance_t *inst) {
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/roms/%s", inst->module_dir, CACHE_FILENAME);

    FILE *f = fopen(cache_path, "wb");
    if (!f) {
        fprintf(stderr, "JV880 v2: Failed to save cache to %s: %s\n", cache_path, strerror(errno));
        return;
    }

    CacheHeader hdr;
    hdr.magic = CACHE_MAGIC;
    hdr.version = CACHE_VERSION;

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/jv880_rom1.bin", inst->module_dir);
    hdr.rom1_size = v2_get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_rom2.bin", inst->module_dir);
    hdr.rom2_size = v2_get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom1.bin", inst->module_dir);
    hdr.waverom1_size = v2_get_file_size(path);
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom2.bin", inst->module_dir);
    hdr.waverom2_size = v2_get_file_size(path);

    hdr.expansion_count = inst->expansion_count;
    hdr.total_patches = inst->total_patches;
    hdr.bank_count = inst->bank_count;

    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(&inst->expansion_file_count, sizeof(inst->expansion_file_count), 1, f);
    for (int i = 0; i < inst->expansion_file_count; i++) {
        fwrite(inst->expansion_files[i], sizeof(inst->expansion_files[0]), 1, f);
        fwrite(&inst->expansion_sizes[i], sizeof(inst->expansion_sizes[0]), 1, f);
    }

    for (int i = 0; i < inst->expansion_count; i++) {
        fwrite(inst->expansions[i].filename, sizeof(inst->expansions[i].filename), 1, f);
        fwrite(inst->expansions[i].name, sizeof(inst->expansions[i].name), 1, f);
        fwrite(&inst->expansions[i].patch_count, sizeof(inst->expansions[i].patch_count), 1, f);
        fwrite(&inst->expansions[i].patches_offset, sizeof(inst->expansions[i].patches_offset), 1, f);
        fwrite(&inst->expansions[i].first_global_index, sizeof(inst->expansions[i].first_global_index), 1, f);
        fwrite(&inst->expansions[i].rom_size, sizeof(inst->expansions[i].rom_size), 1, f);
    }

    fwrite(inst->patches, sizeof(PatchInfo), inst->total_patches, f);
    fwrite(inst->bank_starts, sizeof(inst->bank_starts[0]), inst->bank_count, f);
    fwrite(inst->bank_names, sizeof(inst->bank_names[0]), inst->bank_count, f);

    fclose(f);
    chown_to_ableton(cache_path);
    fprintf(stderr, "JV880 v2: Saved cache\n");
}

/* v2: Load cache */
static int v2_load_cache(jv880_instance_t *inst) {
    char cache_path[1024];
    snprintf(cache_path, sizeof(cache_path), "%s/roms/%s", inst->module_dir, CACHE_FILENAME);

    FILE *f = fopen(cache_path, "rb");
    if (!f) return 0;

    CacheHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != CACHE_MAGIC ||
        hdr.version != CACHE_VERSION) {
        fclose(f);
        return 0;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/jv880_rom1.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.rom1_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_rom2.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.rom2_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom1.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.waverom1_size) { fclose(f); return 0; }
    snprintf(path, sizeof(path), "%s/roms/jv880_waverom2.bin", inst->module_dir);
    if (v2_get_file_size(path) != hdr.waverom2_size) { fclose(f); return 0; }

    int cached_exp_count;
    if (fread(&cached_exp_count, sizeof(cached_exp_count), 1, f) != 1 ||
        cached_exp_count != inst->expansion_file_count) {
        fclose(f);
        return 0;
    }

    for (int i = 0; i < cached_exp_count; i++) {
        char cached_name[256];
        uint32_t cached_size;
        fread(cached_name, sizeof(cached_name), 1, f);
        fread(&cached_size, sizeof(cached_size), 1, f);

        int found = 0;
        for (int j = 0; j < inst->expansion_file_count; j++) {
            if (strcmp(cached_name, inst->expansion_files[j]) == 0 &&
                cached_size == inst->expansion_sizes[j]) {
                found = 1;
                break;
            }
        }
        if (!found) { fclose(f); return 0; }
    }

    inst->expansion_count = hdr.expansion_count;
    inst->total_patches = hdr.total_patches;
    inst->bank_count = hdr.bank_count;

    for (int i = 0; i < inst->expansion_count; i++) {
        fread(inst->expansions[i].filename, sizeof(inst->expansions[i].filename), 1, f);
        fread(inst->expansions[i].name, sizeof(inst->expansions[i].name), 1, f);
        fread(&inst->expansions[i].patch_count, sizeof(inst->expansions[i].patch_count), 1, f);
        fread(&inst->expansions[i].patches_offset, sizeof(inst->expansions[i].patches_offset), 1, f);
        fread(&inst->expansions[i].first_global_index, sizeof(inst->expansions[i].first_global_index), 1, f);
        fread(&inst->expansions[i].rom_size, sizeof(inst->expansions[i].rom_size), 1, f);
        inst->expansions[i].unscrambled = nullptr;
    }

    free(inst->patches);
    inst->patches = (PatchInfo *)calloc(inst->total_patches > 0 ? inst->total_patches : 1,
                                        sizeof(PatchInfo));
    if (!inst->patches) {
        fprintf(stderr, "JV880 v2: Failed to allocate patches array from cache (%d entries)\n",
                inst->total_patches);
        inst->total_patches = 0;
        inst->bank_count = 0;
        fclose(f);
        return 0;
    }

    fread(inst->patches, sizeof(PatchInfo), inst->total_patches, f);
    fread(inst->bank_starts, sizeof(inst->bank_starts[0]), inst->bank_count, f);
    fread(inst->bank_names, sizeof(inst->bank_names[0]), inst->bank_count, f);

    fclose(f);
    fprintf(stderr, "JV880 v2: Loaded cache (%d patches, %d banks, %d expansions)\n",
            inst->total_patches, inst->bank_count, inst->expansion_count);
    return 1;
}

/* v2: Load thread function */
static void* v2_load_thread_func(void *arg) {
    jv880_instance_t *inst = (jv880_instance_t*)arg;

    fprintf(stderr, "JV880 v2: Load thread started\n");

    /* Scan for expansion files */
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Checking expansions...");
    v2_scan_expansion_files(inst);
    fprintf(stderr, "JV880 v2: Found %d expansion files\n", inst->expansion_file_count);

    /* Try cache first */
    int cache_valid = v2_load_cache(inst);

    if (!cache_valid) {
        fprintf(stderr, "JV880 v2: Cache miss, scanning expansions...\n");
        snprintf(inst->loading_status, sizeof(inst->loading_status), "Scanning expansions...");
        v2_scan_expansions(inst);
        v2_build_patch_list(inst);
        v2_save_cache(inst);
    }

    /* Select initial patch */
    if (inst->total_patches > 0) {
        v2_select_patch(inst, 0);
    }

    /* Warmup */
    fprintf(stderr, "JV880 v2: Running warmup...\n");
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Warming up...");
    for (int i = 0; i < 100000; i++) {
        inst->mcu->updateSC55(1);
    }
    fprintf(stderr, "JV880 v2: Warmup done\n");

    /* Pre-fill audio buffer */
    inst->ring_write = 0;
    inst->ring_read = 0;

    /* Initialize fixed-ratio polyphase resampler (64000 Hz -> 44100 Hz,
     * L=441 M=640).  Builds its coefficient bank once here. */
    double ratio = (double)MOVE_SAMPLE_RATE / (double)JV880_SAMPLE_RATE;
    inst->resampleL = nullptr;
    inst->resampleR = nullptr;
    ResamplerFixed_init(&inst->rfix);
    fprintf(stderr, "JV880 v2: Fixed-ratio resampler initialized (ratio %.4f, "
                    "L=%d M=%d, %d taps/phase)\n",
            ratio, RF_L, RF_M, RF_TAPS_PER_PHASE);

    fprintf(stderr, "JV880 v2: Pre-filling buffer...\n");
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Preparing audio...");
    for (int i = 0; i < 256 && inst->ring_write < AUDIO_RING_SIZE / 2; i++) {
        inst->mcu->updateSC55(8);
        int avail = inst->mcu->sample_write_ptr;
        int in_samples = avail / 2;

        if (in_samples > 0 && in_samples < 4096) {
            /* Convert int16 to float for resampler */
            for (int j = 0; j < in_samples; j++) {
                inst->resample_in_l[j] = (float)inst->mcu->sample_buffer[j * 2] / 32768.0f;
                inst->resample_in_r[j] = (float)inst->mcu->sample_buffer[j * 2 + 1] / 32768.0f;
            }

            /* Resample (fixed-ratio polyphase, L+R together) */
            int out_samples = ResamplerFixed_process(&inst->rfix,
                                  inst->resample_in_l, inst->resample_in_r, in_samples,
                                  inst->resample_out_l, inst->resample_out_r);

            /* Copy to ring buffer */
            for (int j = 0; j < out_samples && inst->ring_write < AUDIO_RING_SIZE / 2; j++) {
                int32_t l = (int32_t)(inst->resample_out_l[j] * 32768.0f);
                int32_t r = (int32_t)(inst->resample_out_r[j] * 32768.0f);
                if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                inst->audio_ring[inst->ring_write * 2 + 0] = (int16_t)l;
                inst->audio_ring[inst->ring_write * 2 + 1] = (int16_t)r;
                inst->ring_write = (inst->ring_write + 1) % AUDIO_RING_SIZE;
            }
        }
    }
    fprintf(stderr, "JV880 v2: Buffer pre-filled: %d samples\n", inst->ring_write);

    /* Start emulation thread - set initialized BEFORE pthread_create so
     * render_block and on_midi can start working with the pre-filled buffer
     * while the emu_thread starts up */
    inst->thread_running = 1;
    inst->initialized = 1;
    pthread_create(&inst->emu_thread, NULL, v2_emu_thread_func, inst);

    inst->loading_complete = 1;
    snprintf(inst->loading_status, sizeof(inst->loading_status),
             "Ready: %d patches in %d banks", inst->total_patches, inst->bank_count);

    /* Apply any pending state that was queued during loading */
    if (inst->pending_state_valid) {
        fprintf(stderr, "JV880 v2: Applying deferred state restoration\n");
        inst->pending_state_valid = 0;
        /* Re-call set_param now that loading is complete */
        v2_set_param(inst, "state", inst->pending_state);
    }

    fprintf(stderr, "JV880 v2: Ready!\n");
    inst->load_thread_running = 0;
    return NULL;
}

/* v2: Create instance */
static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    jv880_instance_t *inst = (jv880_instance_t*)calloc(1, sizeof(jv880_instance_t));
    if (!inst) {
        fprintf(stderr, "JV880 v2: Failed to allocate instance\n");
        return NULL;
    }

    /* Initialize part_patchbank to -1 (uninitialized) so we derive from SRAM on first access */
    for (int i = 0; i < 8; i++) {
        inst->part_patchbank[i] = -1;
    }

    /* Macro semantics default (calloc already zeroes this; set explicitly for clarity) */
    inst->macro_mode = MACRO_MODE_DEFAULT;

    /* Per-instance tone cache + link backup start invalid (calloc zeroes these;
     * set explicitly for clarity / correctness against future struct moves). */
    inst->tone_cache_valid = 0;
    inst->link_backup_valid = 0;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    fprintf(stderr, "JV880 v2: Loading from %s\n", module_dir);

    /* Check for debug mode */
    char debug_path[1024];
    snprintf(debug_path, sizeof(debug_path), "%s/debug_sysex_test", module_dir);
    if (access(debug_path, F_OK) == 0) {
        inst->debug_sysex = 1;
        fprintf(stderr, "JV880 v2: SysEx debug enabled\n");
    }

    /* Initialize mutex */
    pthread_mutex_init(&inst->ring_mutex, NULL);

    /* Initialize loading status */
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Initializing...");
    inst->current_expansion = -1;
    inst->found_perf_sram_offset = -1;
    inst->map_last_offset = -1;

    /* Create emulator instance */
    inst->mcu = new MCU();

    /* Load ROMs */
    uint8_t *rom1 = (uint8_t *)malloc(ROM1_SIZE);
    uint8_t *rom2 = (uint8_t *)malloc(ROM2_SIZE);
    uint8_t *waverom1 = (uint8_t *)malloc(0x200000);
    uint8_t *waverom2 = (uint8_t *)malloc(0x200000);
    uint8_t *nvram = (uint8_t *)malloc(NVRAM_SIZE);

    if (!rom1 || !rom2 || !waverom1 || !waverom2 || !nvram) {
        fprintf(stderr, "JV880 v2: Memory allocation failed\n");
        free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);
        delete inst->mcu;
        pthread_mutex_destroy(&inst->ring_mutex);
        free(inst);
        return NULL;
    }

    memset(nvram, 0xFF, NVRAM_SIZE);

    int ok = 1;
    ok = ok && v2_load_rom(inst, "jv880_rom1.bin", rom1, ROM1_SIZE);
    ok = ok && v2_load_rom(inst, "jv880_rom2.bin", rom2, ROM2_SIZE);
    ok = ok && v2_load_rom(inst, "jv880_waverom1.bin", waverom1, 0x200000);
    ok = ok && v2_load_rom(inst, "jv880_waverom2.bin", waverom2, 0x200000);

    /* NVRAM is optional */
    char nvram_path[1024];
    snprintf(nvram_path, sizeof(nvram_path), "%s/roms/jv880_nvram.bin", module_dir);
    FILE *nf = fopen(nvram_path, "rb");
    if (nf) {
        fread(nvram, 1, NVRAM_SIZE, nf);
        fclose(nf);
        fprintf(stderr, "JV880 v2: Loaded NVRAM\n");
    }

    if (!ok) {
        fprintf(stderr, "JV880 v2: ROM loading failed\n");
        snprintf(inst->load_error, sizeof(inst->load_error),
                 "Mini-JV: ROM files not found. Place ROM files in roms/ folder.");
        free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);
        delete inst->mcu;
        inst->mcu = nullptr;
        inst->rom_loaded = 0;
        inst->initialized = 1;  /* Mark as initialized so get_error works */
        return inst;  /* Return instance so error can be retrieved */
    }

    /* Initialize emulator */
    inst->mcu->startSC55(rom1, rom2, waverom1, waverom2, nvram);

    /* Keep ROM2 for internal patch access */
    inst->rom2 = rom2;

    free(rom1); free(waverom1); free(waverom2); free(nvram);

    inst->rom_loaded = 1;

    /* Set patch mode */
    inst->mcu->nvram[NVRAM_MODE_OFFSET] = 1;

    /* Load patches/expansions and warmup in background */
    inst->load_thread_running = 1;
    pthread_create(&inst->load_thread, NULL, v2_load_thread_func, inst);

    fprintf(stderr, "JV880 v2: Instance created\n");
    return inst;
}

/* v2: Destroy instance */
static void v2_destroy_instance(void *instance) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return;

    fprintf(stderr, "JV880 v2: Destroying instance\n");

    /* Stop load thread */
    if (inst->load_thread_running) {
        inst->load_thread_running = 0;
        pthread_join(inst->load_thread, NULL);
    }

    /* Stop emulator thread */
    if (inst->thread_running) {
        inst->thread_running = 0;
        pthread_join(inst->emu_thread, NULL);
    }

    /* Cleanup resampler */
    if (inst->resampleL) {
        resample_close(inst->resampleL);
        inst->resampleL = nullptr;
    }
    if (inst->resampleR) {
        resample_close(inst->resampleR);
        inst->resampleR = nullptr;
    }

    /* Cleanup emulator */
    if (inst->mcu) {
        delete inst->mcu;
        inst->mcu = nullptr;
    }

    /* Free ROM2 */
    if (inst->rom2) {
        free(inst->rom2);
        inst->rom2 = nullptr;
    }

    /* Free patch list (heap-allocated in v2_build_patch_list / v2_load_cache) */
    if (inst->patches) {
        free(inst->patches);
        inst->patches = nullptr;
    }

    /* Free expansion data */
    for (int i = 0; i < inst->expansion_count; i++) {
        if (inst->expansions[i].unscrambled) {
            free(inst->expansions[i].unscrambled);
            inst->expansions[i].unscrambled = nullptr;
        }
    }

    pthread_mutex_destroy(&inst->ring_mutex);
    free(inst);
    fprintf(stderr, "JV880 v2: Instance destroyed\n");
}

/* v2: Load ROM helper */
static int v2_load_rom(jv880_instance_t *inst, const char *filename, uint8_t *dest, size_t size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/%s", inst->module_dir, filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "JV880 v2: Cannot open: %s\n", path);
        return 0;
    }

    size_t got = fread(dest, 1, size, f);
    fclose(f);

    if (got != size) {
        fprintf(stderr, "JV880 v2: Size mismatch: %s (%zu vs %zu)\n", filename, got, size);
        return 0;
    }

    fprintf(stderr, "JV880 v2: Loaded %s\n", filename);
    return 1;
}

/* v2: Ring buffer helpers (instance-based) */
static int v2_ring_available(jv880_instance_t *inst) {
    int avail = inst->ring_write - inst->ring_read;
    if (avail < 0) avail += AUDIO_RING_SIZE;
    return avail;
}

static int v2_ring_free(jv880_instance_t *inst) {
    return AUDIO_RING_SIZE - 1 - v2_ring_available(inst);
}

/* v2: Per-thread performance setup for the emulator thread.
 * - Pins to cores 0-2 (core 3 is reserved for the SPI audio callback).
 * - Requests SCHED_FIFO 45 (below MoveOriginal audio threads at FIFO 70,
 *   well below the SPI thread at FIFO 90). EPERM is non-fatal.
 * - Flushes aarch64 denormals to zero via FPCR bit 24.
 */
static void v2_emu_thread_setup(void) {
#ifdef __linux__
    /* CPU affinity: cores 0, 1, 2 */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    CPU_SET(1, &cpuset);
    CPU_SET(2, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        fprintf(stderr, "JV880 v2: Failed to set CPU affinity: %s\n", strerror(errno));
    }

    /* Real-time scheduling: SCHED_FIFO priority 45 */
    struct sched_param sp;
    sp.sched_priority = 45;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        fprintf(stderr, "JV880 v2: Could not set SCHED_FIFO 45 (%s) — continuing with default scheduling\n",
                strerror(rc));
    }
#endif /* __linux__ */

#ifdef __aarch64__
    /* Flush-to-zero for float32/float64 denormals (FPCR bit 24) */
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);
    __asm__ __volatile__("msr fpcr, %0" :: "r"(fpcr));
#endif /* __aarch64__ */
}

/* v2: Emulator thread */
static void* v2_emu_thread_func(void *arg) {
    jv880_instance_t *inst = (jv880_instance_t*)arg;
    v2_emu_thread_setup();
    fprintf(stderr, "JV880 v2: Emulation thread started\n");

#if JV880_PERF_STATS
    /* Helper lambda-like macro: read ns from a timespec */
#define TS_NS(ts) ((uint64_t)(ts).tv_sec * 1000000000ULL + (uint64_t)(ts).tv_nsec)

    /* Seed window start values */
    {
        struct timespec ts_wall, ts_cpu;
        clock_gettime(CLOCK_MONOTONIC, &ts_wall);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_cpu);
        inst->perf_wall_window_ns = TS_NS(ts_wall);
        inst->perf_cpu_window_ns  = TS_NS(ts_cpu);
        /* First report after 15 s of wall time */
        inst->perf_next_report_ns = inst->perf_wall_window_ns + 15000000000ULL;
    }
#endif /* JV880_PERF_STATS */

    while (inst->thread_running) {
        /* Handle warmup after SC55_Reset */
        if (inst->warmup_remaining > 0) {
            int batch = (inst->warmup_remaining > 1000) ? 1000 : inst->warmup_remaining;
            for (int i = 0; i < batch; i++) {
                inst->mcu->updateSC55(1);
            }
            inst->warmup_remaining -= batch;
            if (inst->warmup_remaining <= 0) {
                snprintf(inst->loading_status, sizeof(inst->loading_status),
                         "Ready: %d patches", inst->total_patches);
                jv_debug("[v2_emu_thread] Warmup complete\n");
            }
            continue;  /* Skip audio output during warmup */
        }

        /* Process MIDI queue */
        while (inst->midi_read != inst->midi_write) {
            int idx = inst->midi_read;
            inst->mcu->postMidiSC55(inst->midi_queue[idx], inst->midi_queue_len[idx]);
            inst->midi_read = (inst->midi_read + 1) % MIDI_QUEUE_SIZE;
        }

        /* Check for pending parameter mapping SysEx */
        if (inst->map_sysex_len > 0) {
            inst->mcu->postMidiSC55(inst->map_sysex_pending, inst->map_sysex_len);
            inst->map_sysex_len = 0;
        }

        /* Check if we need more audio.
         * Sleep ~1ms when the ring is full: the consumer drains 128 frames
         * per ~2.9ms, so 64 frames free takes ~1.45ms.  A 50us poll here
         * meant ~20k wakeups/s of syscall+scheduler overhead on the A72,
         * which dominated the thread's CPU once emulation got cheaper.
         * Latency is unaffected: the ring is already at max fill when we
         * sleep, and 1ms of drain (~44 frames) is refilled in well under
         * a millisecond of emulation. */
        int free_space = v2_ring_free(inst);
        if (free_space < 64) {
#if JV880_PERF_STATS
            inst->perf_sleep_iters++;
#endif
            usleep(1000);
            continue;
        }

#if JV880_PERF_STATS
        /* --- T0: start of working iteration --- */
        struct timespec ts_t0, ts_t1, ts_t2;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_t0);
#endif

        inst->mcu->updateSC55(64);

#if JV880_PERF_STATS
        /* --- T1: after updateSC55 --- */
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_t1);
        inst->perf_emu_calls++;
#endif

        int avail = inst->mcu->sample_write_ptr;
        int in_samples = avail / 2;  /* Stereo pairs */

        if (in_samples > 0 && in_samples < 4096) {
            /* Convert int16 to float for resampler (separate L/R channels) */
            for (int i = 0; i < in_samples; i++) {
                inst->resample_in_l[i] = (float)inst->mcu->sample_buffer[i * 2] / 32768.0f;
                inst->resample_in_r[i] = (float)inst->mcu->sample_buffer[i * 2 + 1] / 32768.0f;
            }

            /* Resample using fixed-ratio polyphase filter (L+R together,
             * persistent phase state across chunk boundaries). */
            int out_samples = ResamplerFixed_process(&inst->rfix,
                                  inst->resample_in_l, inst->resample_in_r, in_samples,
                                  inst->resample_out_l, inst->resample_out_r);

            /* Batch copy to ring buffer with single lock */
            if (out_samples > 0) {
                pthread_mutex_lock(&inst->ring_mutex);
                int free_now = v2_ring_free(inst);
                int to_write = (out_samples < free_now) ? out_samples : free_now;
                for (int i = 0; i < to_write; i++) {
                    int wr = inst->ring_write;
                    /* Convert float back to int16 */
                    int32_t l = (int32_t)(inst->resample_out_l[i] * 32768.0f);
                    int32_t r = (int32_t)(inst->resample_out_r[i] * 32768.0f);
                    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                    inst->audio_ring[wr * 2 + 0] = (int16_t)l;
                    inst->audio_ring[wr * 2 + 1] = (int16_t)r;
                    inst->ring_write = (wr + 1) % AUDIO_RING_SIZE;
                }
                pthread_mutex_unlock(&inst->ring_mutex);
            }
        }

#if JV880_PERF_STATS
        /* --- T2: end of convert+resample+ringwrite block --- */
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_t2);

        {
            uint64_t ns0 = TS_NS(ts_t0);
            uint64_t ns1 = TS_NS(ts_t1);
            uint64_t ns2 = TS_NS(ts_t2);
            inst->perf_ns_emu    += ns1 - ns0;
            inst->perf_ns_resamp += ns2 - ns1;
            inst->perf_ns_total  += ns2 - ns0;
        }
        inst->perf_loop_iters++;

        /* Periodic report: check wall clock once per working iteration */
        {
            struct timespec ts_wall_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_wall_now);
            uint64_t wall_now_ns = TS_NS(ts_wall_now);
            if (wall_now_ns >= inst->perf_next_report_ns) {
                /* Compute wall and cpu deltas for the window */
                struct timespec ts_cpu_now;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_cpu_now);
                uint64_t cpu_now_ns  = TS_NS(ts_cpu_now);

                uint64_t wall_delta = wall_now_ns  - inst->perf_wall_window_ns;
                uint64_t cpu_delta  = cpu_now_ns   - inst->perf_cpu_window_ns;

                /* Compute percentages (avoid divide-by-zero) */
                double pct_emu    = (inst->perf_ns_total > 0)
                    ? 100.0 * (double)inst->perf_ns_emu    / (double)inst->perf_ns_total : 0.0;
                double pct_resamp = (inst->perf_ns_total > 0)
                    ? 100.0 * (double)inst->perf_ns_resamp / (double)inst->perf_ns_total : 0.0;
                double pct_other  = 100.0 - pct_emu - pct_resamp;
                double pct_cpu_wall = (wall_delta > 0)
                    ? 100.0 * (double)cpu_delta / (double)wall_delta : 0.0;

                double wall_s = (double)wall_delta / 1e9;
                double loops_s  = (wall_s > 0.0) ? (double)inst->perf_loop_iters  / wall_s : 0.0;
                double sleeps_s = (wall_s > 0.0) ? (double)inst->perf_sleep_iters / wall_s : 0.0;

                uint64_t probe_steps_d = inst->mcu->probe_steps - inst->perf_probe_steps0;
                uint64_t probe_sleep_d = inst->mcu->probe_sleep_steps - inst->perf_probe_sleep0;
                double mcu_sleep_frac = (probe_steps_d > 0)
                    ? 100.0 * (double)probe_sleep_d / (double)probe_steps_d : 0.0;

                snprintf(inst->perf_stats_buf, sizeof(inst->perf_stats_buf),
                    "JV880 perf: emu=%.1f%% resamp=%.1f%% other=%.1f%% of thread cpu"
                    " | thread_cpu=%.1f%% wall | sleeps/s=%.0f loops/s=%.0f"
                    " | mcu_sleep=%.1f%%",
                    pct_emu, pct_resamp, pct_other,
                    pct_cpu_wall, sleeps_s, loops_s, mcu_sleep_frac);

                fprintf(stderr, "%s\n", inst->perf_stats_buf);

                /* MoveOriginal's stderr is discarded on-device; also drop the
                 * line into the module dir so it can be read over ssh.  One
                 * tiny write per 15s from this (non-SPI) thread is harmless. */
                {
                    char perf_path[600];
                    snprintf(perf_path, sizeof(perf_path), "%s/perf_stats.txt",
                             inst->module_dir);
                    FILE *pf = fopen(perf_path, "w");
                    if (pf) {
                        fprintf(pf, "%s\n", inst->perf_stats_buf);
                        fclose(pf);
                    }
                }

                /* Reset window accumulators */
                inst->perf_ns_emu        = 0;
                inst->perf_ns_resamp     = 0;
                inst->perf_ns_total      = 0;
                inst->perf_loop_iters    = 0;
                inst->perf_sleep_iters   = 0;
                inst->perf_emu_calls     = 0;
                inst->perf_wall_window_ns = wall_now_ns;
                inst->perf_cpu_window_ns  = cpu_now_ns;
                inst->perf_probe_steps0   = inst->mcu->probe_steps;
                inst->perf_probe_sleep0   = inst->mcu->probe_sleep_steps;
                inst->perf_next_report_ns = wall_now_ns + 15000000000ULL;
            }
        }
#endif /* JV880_PERF_STATS */
    }

#if JV880_PERF_STATS
#undef TS_NS
#endif

    fprintf(stderr, "JV880 v2: Emulation thread stopped\n");
    return NULL;
}

/* v2: Helper to queue SysEx for tone parameter changes
 * two_byte: if true, sends nibblized 2-byte data (high nibble, low nibble)
 *           used for wavenumber, lfo delay, pan, tone delay time
 */
static void v2_queue_tone_sysex(jv880_instance_t *inst, int toneIdx, int paramIdx, int value, int two_byte) {
    if (!inst || toneIdx < 0 || toneIdx > 3) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data... checksum F7 */
    uint8_t addr[4] = { 0x00, 0x08, (uint8_t)(0x28 + toneIdx), (uint8_t)paramIdx };

    if (two_byte) {
        /* 2-byte nibblized format: high nibble then low nibble */
        uint8_t data_hi = (uint8_t)((value >> 4) & 0x0F);
        uint8_t data_lo = (uint8_t)(value & 0x0F);

        /* Calculate checksum */
        int sum = addr[0] + addr[1] + addr[2] + addr[3] + data_hi + data_lo;
        uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

        uint8_t sysex[13] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                              addr[0], addr[1], addr[2], addr[3],
                              data_hi, data_lo, chk, 0xF7 };

        /* Queue the SysEx. Lock the ring producer so control-thread fan-outs
         * (macros, link snap/restore) don't race the audio-thread producer
         * (v2_on_midi / render-block PC) on midi_write. */
        pthread_mutex_lock(&inst->ring_mutex);
        int write_idx = inst->midi_write;
        int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) { /* skip if queue full */
            memcpy(inst->midi_queue[write_idx], sysex, 13);
            inst->midi_queue_len[write_idx] = 13;
            inst->midi_write = next;
        }
        pthread_mutex_unlock(&inst->ring_mutex);
    } else {
        /* Standard 1-byte format */
        uint8_t data = (uint8_t)(value & 0x7F);

        /* Calculate checksum: 128 - (sum of addr + data) mod 128 */
        int sum = addr[0] + addr[1] + addr[2] + addr[3] + data;
        uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

        uint8_t sysex[12] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                              addr[0], addr[1], addr[2], addr[3],
                              data, chk, 0xF7 };

        /* Queue the SysEx (locked; see above). */
        pthread_mutex_lock(&inst->ring_mutex);
        int write_idx = inst->midi_write;
        int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[write_idx], sysex, 12);
            inst->midi_queue_len[write_idx] = 12;
            inst->midi_write = next;
        }
        pthread_mutex_unlock(&inst->ring_mutex);
    }
}

/* v2: Helper to queue SysEx for patch common parameter changes */
static void v2_queue_patch_common_sysex(jv880_instance_t *inst, int paramIdx, int value) {
    if (!inst) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data checksum F7 */
    uint8_t addr[4] = { 0x00, 0x08, 0x20, (uint8_t)paramIdx };
    uint8_t data = (uint8_t)(value & 0x7F);

    /* Calculate checksum */
    int sum = addr[0] + addr[1] + addr[2] + addr[3] + data;
    uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

    uint8_t sysex[12] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                          addr[0], addr[1], addr[2], addr[3],
                          data, chk, 0xF7 };

    /* Queue the SysEx (locked ring producer; see v2_queue_tone_sysex). */
    pthread_mutex_lock(&inst->ring_mutex);
    int write_idx = inst->midi_write;
    int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[write_idx], sysex, 12);
        inst->midi_queue_len[write_idx] = 12;
        inst->midi_write = next;
    }
    pthread_mutex_unlock(&inst->ring_mutex);
}

/* v2: Apply a macro offset across all 4 tones via SysEx
 * Reads base value from NVRAM, applies offset, sends SysEx for each tone.
 * Does NOT modify NVRAM — preserves per-tone base values.
 */
static void v2_apply_macro(jv880_instance_t *inst, const char *tone_param_name, int offset) {
    const ToneParamEntry *p = find_tone_param(tone_param_name);
    if (!p || !inst->mcu) return;

    for (int t = 0; t < 4; t++) {
        int toneBase = NVRAM_PATCH_OFFSET + 26 + (t * 84);
        uint8_t raw = inst->mcu->nvram[toneBase + p->nvram_offset];
        int sysex_val;

        if (p->signed_param == 1) {
            /* Signed param: stored as int8_t, SysEx uses +64 offset */
            int base = (int)(int8_t)raw;
            int result = base + offset;
            if (result < -63) result = -63;
            if (result > 63) result = 63;
            sysex_val = result + 64;
        } else {
            /* Unsigned param: 0-127 */
            int base = (int)raw;
            int result = base + offset;
            if (result < 0) result = 0;
            if (result > 127) result = 127;
            sysex_val = result;
        }
        v2_queue_tone_sysex(inst, t, p->sysex_idx, sysex_val, p->two_byte);
    }
}

/* v2: Read a TP_BYTE tone param's stored display value from NVRAM.
 * Returns the signed (signed_param==1) or unsigned (else) value as displayed.
 * Used by macro_read and by absolute macro_write to compute per-tone deltas. */
static int v2_read_tone_byte_param(jv880_instance_t *inst, int toneIdx,
                                   const ToneParamEntry *p) {
    int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
    uint8_t raw = inst->mcu->nvram[toneBase + p->nvram_offset];
    if (p->signed_param == 1) return (int)(int8_t)raw;
    return (int)raw;
}

/* v2: Write a single TP_BYTE tone param (NVRAM + DT1 SysEx) given a display
 * value, mirroring the TP_BYTE branch in v2_set_param. Reuses v2_queue_tone_sysex
 * so no SysEx building is duplicated. Handles unsigned (0..127) and
 * signed_param==1 (-63..63, +64 SysEx offset). Macros only touch such params. */
static void v2_write_tone_byte_param(jv880_instance_t *inst, int toneIdx,
                                     const ToneParamEntry *p, int value) {
    int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
    uint8_t *b = &inst->mcu->nvram[toneBase + p->nvram_offset];
    if (p->signed_param == 1) {
        int v = clamp_int(value, -63, 63);
        *b = (uint8_t)(int8_t)v;
        v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v + 64, p->two_byte);
    } else {
        int v = clamp_int(value, 0, 127);
        *b = (uint8_t)v;
        v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
    }
}

/* v2: True if a tone's toneswitch bit is set (tone enabled).
 * toneswitch lives at nvram_offset 0, bit 7 of the tone block. */
static int v2_tone_enabled(jv880_instance_t *inst, int toneIdx) {
    const ToneParamEntry *sw = find_tone_param("toneswitch");
    if (!sw) return 1;
    int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);
    uint8_t raw = inst->mcu->nvram[toneBase + sw->nvram_offset];
    return (raw & (1 << sw->bit_shift)) ? 1 : 0;
}

/* v2: Anchor tone = lowest-numbered enabled tone, else tone 0. */
static int v2_macro_anchor_tone(jv880_instance_t *inst) {
    for (int t = 0; t < 4; t++) {
        if (v2_tone_enabled(inst, t)) return t;
    }
    return 0;
}

/* Accessor for the per-macro relative offset store (RELATIVE_RESET mode). */
static int* v2_macro_offset_slot(jv880_instance_t *inst, const macro_def_t *m) {
    if (strcmp(m->key, "cutoff") == 0)        return &inst->macro_cutoff;
    if (strcmp(m->key, "resonance") == 0)     return &inst->macro_resonance;
    if (strcmp(m->key, "attack") == 0)        return &inst->macro_attack;
    if (strcmp(m->key, "decay") == 0)         return &inst->macro_decay;
    if (strcmp(m->key, "release") == 0)       return &inst->macro_release;
    if (strcmp(m->key, "tvf_env_depth") == 0) return &inst->macro_tvf_env_depth;
    if (strcmp(m->key, "sustain") == 0)       return &inst->macro_sustain;
    if (strcmp(m->key, "lfo_depth") == 0)     return &inst->macro_lfo_depth;
    return NULL;
}

/* ========================================================================
 * THE MACRO FUNNEL: macro_read / macro_write
 * Every macro get_param routes through macro_read; every macro set_param
 * routes through macro_write. Behavior branches solely on inst->macro_mode.
 * ======================================================================== */

/* macro_read: returns the value to display on the knob.
 *   ABSOLUTE_ANCHORED -> anchor tone's stored value.
 *   RELATIVE_RESET    -> the current stored offset for this macro. */
static int macro_read(jv880_instance_t *inst, const macro_def_t *m) {
    if (!inst || !inst->mcu || !m) return 0;
    if (inst->macro_mode == MACRO_RELATIVE_RESET) {
        int *slot = v2_macro_offset_slot(inst, m);
        return slot ? *slot : 0;
    }
    /* ABSOLUTE_ANCHORED */
    const ToneParamEntry *p = find_tone_param(m->tone_param);
    if (!p) return 0;
    return v2_read_tone_byte_param(inst, v2_macro_anchor_tone(inst), p);
}

/* macro_write: applies the macro semantics for the given display value.
 *   ABSOLUTE_ANCHORED -> delta = value - anchor_value; for every enabled tone
 *       write clamp(tone_value + delta, min, max) via the normal nvram+SysEx path.
 *   RELATIVE_RESET    -> store offset and apply it non-destructively (legacy). */
static void macro_write(jv880_instance_t *inst, const macro_def_t *m, int value) {
    if (!inst || !inst->mcu || !m) return;

    if (inst->macro_mode == MACRO_RELATIVE_RESET) {
        int *slot = v2_macro_offset_slot(inst, m);
        int offset = clamp_int(value, -64, 63);
        if (slot) *slot = offset;
        v2_apply_macro(inst, m->tone_param, offset);
        return;
    }

    /* ABSOLUTE_ANCHORED */
    const ToneParamEntry *p = find_tone_param(m->tone_param);
    if (!p) return;
    int target = clamp_int(value, m->min, m->max);
    int anchor = v2_macro_anchor_tone(inst);
    int anchor_value = v2_read_tone_byte_param(inst, anchor, p);
    int delta = target - anchor_value;
    for (int t = 0; t < 4; t++) {
        if (!v2_tone_enabled(inst, t)) continue;
        int cur = v2_read_tone_byte_param(inst, t, p);
        int nv = clamp_int(cur + delta, m->min, m->max);
        v2_write_tone_byte_param(inst, t, p, nv);
    }
}

/* v2: Helper to queue SysEx for part parameter changes
 * two_byte: if true, sends MSB/LSB format (for 0-255 range params like patchnumber)
 *           MSB = (value >> 7) & 0x7F, LSB = value & 0x7F
 */
static void v2_queue_part_sysex(jv880_instance_t *inst, int partIdx, int paramIdx, int value, int two_byte) {
    if (!inst || partIdx < 0 || partIdx > 7) return;

    /* Build Roland DT1 SysEx: F0 41 10 46 12 addr[4] data... checksum F7 */
    /* Part address: [0x00, 0x00, 0x18 + partIdx, paramIdx] */
    uint8_t addr[4] = { 0x00, 0x00, (uint8_t)(0x18 + partIdx), (uint8_t)paramIdx };

    if (two_byte) {
        /* 2-byte nibblized format for 0-255 range (e.g., patchnumber)
         * Per Roland SysEx spec: high nibble at addr, low nibble at addr+1 */
        uint8_t data_high = (uint8_t)((value >> 4) & 0x0F);
        uint8_t data_low = (uint8_t)(value & 0x0F);

        /* Calculate checksum */
        int sum = addr[0] + addr[1] + addr[2] + addr[3] + data_high + data_low;
        uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

        uint8_t sysex[13] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                              addr[0], addr[1], addr[2], addr[3],
                              data_high, data_low, chk, 0xF7 };

        /* Queue the SysEx (locked ring producer; see v2_queue_tone_sysex). */
        pthread_mutex_lock(&inst->ring_mutex);
        int write_idx = inst->midi_write;
        int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[write_idx], sysex, 13);
            inst->midi_queue_len[write_idx] = 13;
            inst->midi_write = next;
        }
        pthread_mutex_unlock(&inst->ring_mutex);
        jv_debug("[JV880] Part%d SysEx: addr=%02X.%02X.%02X.%02X data=%02X.%02X (value=%d)\n",
                partIdx, addr[0], addr[1], addr[2], addr[3], data_high, data_low, value);
    } else {
        /* Standard 1-byte format */
        uint8_t data = (uint8_t)(value & 0x7F);

        /* Calculate checksum */
        int sum = addr[0] + addr[1] + addr[2] + addr[3] + data;
        uint8_t chk = (128 - (sum & 0x7F)) & 0x7F;

        uint8_t sysex[12] = { 0xF0, 0x41, 0x10, 0x46, 0x12,
                              addr[0], addr[1], addr[2], addr[3],
                              data, chk, 0xF7 };

        /* Queue the SysEx (locked; see above). */
        pthread_mutex_lock(&inst->ring_mutex);
        int write_idx = inst->midi_write;
        int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[write_idx], sysex, 12);
            inst->midi_queue_len[write_idx] = 12;
            inst->midi_write = next;
        }
        pthread_mutex_unlock(&inst->ring_mutex);
        jv_debug("[JV880] Part%d SysEx: addr=%02X.%02X.%02X.%02X data=%02X (value=%d)\n",
                partIdx, addr[0], addr[1], addr[2], addr[3], data, value);
    }
}

/* v2: MIDI handler */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst || !inst->initialized) return;
    (void)source;

    /* Copy to MIDI queue with octave transpose. Lock the ring producer so this
     * audio-thread enqueue doesn't race control-thread fan-outs on midi_write. */
    pthread_mutex_lock(&inst->ring_mutex);
    int write_idx = inst->midi_write;
    int next = (write_idx + 1) % MIDI_QUEUE_SIZE;
    if (next == inst->midi_read) { pthread_mutex_unlock(&inst->ring_mutex); return; } /* Queue full */

    memcpy(inst->midi_queue[write_idx], msg, len);
    inst->midi_queue_len[write_idx] = len;

    uint8_t status = inst->midi_queue[write_idx][0] & 0xF0;

    /* Poly -> channel aftertouch translation.
     * The Move's pads emit polyphonic (per-note) aftertouch (0xA0 note pressure),
     * but the JV-880 only responds to channel aftertouch (0xD0 pressure) for its
     * AFT controller-matrix source — so pad pressure was being ignored. Rewrite
     * each poly-AT message into a channel-AT message carrying the same pressure.
     * Per-note pressure collapses to the latest note's value, which is correct
     * for the JV's single channel-wide aftertouch source. */
    if (status == 0xA0 && len >= 3) {
        uint8_t ch = inst->midi_queue[write_idx][0] & 0x0F;
        inst->midi_queue[write_idx][0] = 0xD0 | ch;
        inst->midi_queue[write_idx][1] = inst->midi_queue[write_idx][2]; /* pressure */
        inst->midi_queue_len[write_idx] = 2;
        status = 0xD0;
    }

    /* Apply octave transpose to note on/off */
    if ((status == 0x90 || status == 0x80) && len >= 2) {
        int note = inst->midi_queue[write_idx][1] + (inst->octave_transpose * 12);
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        inst->midi_queue[write_idx][1] = note;
    }

    inst->midi_write = next;
    pthread_mutex_unlock(&inst->ring_mutex);
}

/* v2: Helper to find which bank a patch belongs to */
static int v2_get_bank_for_patch(jv880_instance_t *inst, int patch_index) {
    for (int i = inst->bank_count - 1; i >= 0; i--) {
        if (patch_index >= inst->bank_starts[i]) {
            return i;
        }
    }
    return 0;
}

/* v2: Jump to next/previous bank */
static void v2_jump_to_bank(jv880_instance_t *inst, int direction) {
    int current_bank = v2_get_bank_for_patch(inst, inst->current_patch);
    int new_bank = current_bank + direction;

    /* Wrap around */
    if (new_bank < 0) new_bank = inst->bank_count - 1;
    if (new_bank >= inst->bank_count) new_bank = 0;

    v2_select_patch(inst, inst->bank_starts[new_bank]);
    fprintf(stderr, "JV880 v2: Jumped to bank %d: %s\n", new_bank, inst->bank_names[new_bank]);
}

/* v2: Switch between patch and performance mode */
static void v2_set_mode(jv880_instance_t *inst, int performance_mode) {
    if (!inst || !inst->mcu) {
        jv_debug("[v2_set_mode] ERROR: inst=%p mcu=%p\n", (void*)inst, inst ? (void*)inst->mcu : NULL);
        return;
    }

    int new_mode = performance_mode ? 1 : 0;

    jv_debug("[v2_set_mode] Called: current=%s requested=%s patch=%d perf=%d\n",
            inst->performance_mode ? "Performance" : "Patch",
            new_mode ? "Performance" : "Patch",
            inst->current_patch, inst->current_performance);

    /* Only switch if mode is actually changing */
    if (inst->performance_mode == new_mode) {
        jv_debug("[v2_set_mode] Mode unchanged, returning\n");
        return;
    }

    jv_debug("[v2_set_mode] Switching from %s to %s mode\n",
            inst->performance_mode ? "Performance" : "Patch",
            new_mode ? "Performance" : "Patch");

    /* Send All Notes Off on all channels before mode switch */
    pthread_mutex_lock(&inst->ring_mutex);
    for (int ch = 0; ch < 16; ch++) {
        uint8_t notes_off[3] = { (uint8_t)(0xB0 | ch), 123, 0 };
        int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
        if (next != inst->midi_read) {
            memcpy(inst->midi_queue[inst->midi_write], notes_off, 3);
            inst->midi_queue_len[inst->midi_write] = 3;
            inst->midi_write = next;
        }
    }
    pthread_mutex_unlock(&inst->ring_mutex);
    jv_debug("[v2_set_mode] Sent All Notes Off on all 16 channels\n");

    /* Update mode state */
    inst->performance_mode = new_mode;

    /* Set NVRAM mode directly: 0 = performance, 1 = patch */
    uint8_t desired_nvram_mode = inst->performance_mode ? 0 : 1;
    inst->mcu->nvram[NVRAM_MODE_OFFSET] = desired_nvram_mode;
    jv_debug("[v2_set_mode] Set NVRAM[0x%x] = %d\n", NVRAM_MODE_OFFSET, desired_nvram_mode);

    /* Reset emulator for clean state - don't use button press which can cause conflicts */
    jv_debug("[v2_set_mode] Resetting emulator for clean mode switch\n");
    inst->mcu->SC55_Reset();
    snprintf(inst->loading_status, sizeof(inst->loading_status), "Warming up...");
    inst->warmup_remaining = 100000;  /* Same as initial warmup */

    if (!inst->performance_mode) {
        /* Entering patch mode */
        jv_debug("[v2_set_mode] Entering patch mode, setting pending_patch_select\n");
        inst->pending_patch_select = 50;  /* Delay to allow warmup to complete */
    } else {
        /* Entering performance mode */
        jv_debug("[v2_set_mode] Entering performance mode, setting pending_perf_select\n");
        inst->pending_perf_select = 50;  /* Same delay as patch mode */
    }

    jv_debug("[v2_set_mode] Complete\n");
}

/* v2: Select a performance (0-47 across 3 banks) */
static void v2_select_performance(jv880_instance_t *inst, int perf_index) {
    jv_debug("[v2_select_performance] Called: perf_index=%d\n", perf_index);

    if (!inst || !inst->mcu || perf_index < 0 || perf_index >= NUM_PERFORMANCES) {
        jv_debug("[v2_select_performance] ERROR: invalid args inst=%p mcu=%p perf=%d\n",
                (void*)inst, inst ? (void*)inst->mcu : NULL, perf_index);
        return;
    }

    inst->current_performance = perf_index;
    inst->perf_bank = perf_index / PERFS_PER_BANK;
    int perf_in_bank = perf_index % PERFS_PER_BANK;

    jv_debug("[v2_select_performance] perf=%d bank=%d in_bank=%d current_mode=%s\n",
            perf_index, inst->perf_bank, perf_in_bank,
            inst->performance_mode ? "Performance" : "Patch");

    /* Ensure we're in performance mode */
    if (!inst->performance_mode) {
        jv_debug("[v2_select_performance] Not in performance mode, calling v2_set_mode(1)\n");
        inst->current_performance = perf_index;  /* Store desired perf for deferred selection */
        v2_set_mode(inst, 1);
        /* v2_set_mode sets pending_perf_select, which will trigger this function again
         * from render_block after mode switch has been processed. Return now. */
        return;
    }

    /* Calculate bank select and program change values per JV-880 MIDI spec */
    uint8_t bank_msb;
    uint8_t pc_value;

    switch (inst->perf_bank) {
        case 0:  /* Preset A */
            bank_msb = 81;
            pc_value = perf_in_bank;  /* 0-15 */
            break;
        case 1:  /* Preset B */
            bank_msb = 81;
            pc_value = 64 + perf_in_bank;  /* 64-79 */
            break;
        case 2:  /* Internal */
        default:
            bank_msb = 80;
            pc_value = perf_in_bank;  /* 0-15 */
            break;
    }

    jv_debug("[v2_select_performance] bank_msb=%d pc_value=%d\n", bank_msb, pc_value);

    /* Send on Control Channel (channel 16 = 0x0F) */
    uint8_t ctrl_ch = 0x0F;
    uint8_t bank_msg[3] = { (uint8_t)(0xB0 | ctrl_ch), 0x00, bank_msb };
    uint8_t pc_msg[2] = { (uint8_t)(0xC0 | ctrl_ch), pc_value };

    /* Queue Bank Select (CC#0) */
    pthread_mutex_lock(&inst->ring_mutex);
    int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[inst->midi_write], bank_msg, 3);
        inst->midi_queue_len[inst->midi_write] = 3;
        inst->midi_write = next;
        jv_debug("[v2_select_performance] Queued Bank: [0x%02x 0x%02x 0x%02x]\n",
                bank_msg[0], bank_msg[1], bank_msg[2]);
    } else {
        jv_debug("[v2_select_performance] ERROR: MIDI queue full for bank!\n");
    }

    /* Queue Program Change */
    next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != inst->midi_read) {
        memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
        inst->midi_queue_len[inst->midi_write] = 2;
        inst->midi_write = next;
        jv_debug("[v2_select_performance] Queued PC: [0x%02x 0x%02x]\n",
                pc_msg[0], pc_msg[1]);
    } else {
        jv_debug("[v2_select_performance] ERROR: MIDI queue full for PC!\n");
    }
    pthread_mutex_unlock(&inst->ring_mutex);

    jv_debug("[v2_select_performance] Complete\n");

    /* Schedule SRAM scan for performance data discovery */
    inst->sram_scan_countdown = 100;
}

/* v2: Select a part within the current performance (0-7) */
static void v2_select_part(jv880_instance_t *inst, int part_index) {
    if (!inst || part_index < 0 || part_index > 7) return;
    inst->current_part = part_index;
    fprintf(stderr, "JV880 v2: Selected part %d\n", part_index + 1);
}

static int clamp_int(int value, int min_val, int max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

/* Helper to extract a JSON number value by key */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

/* v2: Set parameter - full expansion support */
/* Subtractive link mode: which per-tone params stay independent (the
 * "oscillator" — wave, tuning, output level/pan) vs. shared (filter, amp env,
 * LFOs, pitch env, control matrix, sends) and driven by Tone 1. Anything not in
 * the per-tone list is treated as shared, so new params default to shared. */
static int v2_is_shared_tone_param(const char *name) {
    static const char *PER_TONE[] = {
        "wavegroup", "wavenumber", "fxmswitch", "fxmdepth", "toneswitch",
        "pitchcoarse", "pitchfine", "randompitchdepth", "pitchkeyfollow",
        "level", "pan", "levelkeyfollow", "panningkeyfollow"
    };
    for (size_t i = 0; i < sizeof(PER_TONE)/sizeof(PER_TONE[0]); i++)
        if (strcmp(name, PER_TONE[i]) == 0) return 0;
    return 1;
}

/* Write one tone param (NVRAM + DT1 SysEx) for a single tone. Extracted from the
 * nvram_tone_* set branch so link mode can fan the same write across all 4 tones.
 * Handles the per-tone control matrix and every ToneParamEntry type. */
static void v2_write_one_tone_param(jv880_instance_t *inst, int toneIdx,
                                    const char *paramName, const char *val) {
    if (!inst || !inst->mcu || toneIdx < 0 || toneIdx > 3) return;
    const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

    const MatrixParam *mx = find_matrix_param(paramName);
    if (mx) {
        uint8_t *mb = &inst->mcu->nvram[toneBase + mx->off];
        if (mx->isSens) {
            int sv = clamp_int(atoi(val), -63, 63);
            *mb = (uint8_t)(int8_t)sv;
            v2_queue_tone_sysex(inst, toneIdx, mx->sysexIdx, sv + 64, 0);
        } else {
            int dv = clamp_int(atoi(val), 0, 12);
            int dmask = ((1 << mx->width) - 1) << mx->shift;
            *mb = (uint8_t)((*mb & ~dmask) | ((dv << mx->shift) & dmask));
            v2_queue_tone_sysex(inst, toneIdx, mx->sysexIdx, dv, 0);
        }
        return;
    }

    const ToneParamEntry *p = find_tone_param(paramName);
    if (!p) return;
    uint8_t *b = &inst->mcu->nvram[toneBase + p->nvram_offset];
    int v, sysex_val;
    switch (p->type) {
        case TP_BYTE:
            if (p->signed_param == 1) {
                v = clamp_int(atoi(val), -63, 63);
                *b = (uint8_t)(int8_t)v; sysex_val = v + 64;
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, sysex_val, p->two_byte);
            } else if (p->signed_param == 2) {
                v = clamp_int(atoi(val), -64, 63); sysex_val = v + 64;
                *b = (uint8_t)sysex_val;
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, sysex_val, p->two_byte);
            } else {
                v = clamp_int(atoi(val), 0, 127); *b = (uint8_t)v;
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
            }
            return;
        case TP_BITFIELD:
            v = clamp_int(atoi(val), 0, p->bit_mask);
            *b = (*b & ~(p->bit_mask << p->bit_shift)) | ((v & p->bit_mask) << p->bit_shift);
            if (strcmp(paramName, "fxmdepth") == 0 && v > 0)
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v - 1, p->two_byte);
            else
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
            return;
        case TP_BOOL:
            v = (strcmp(val, "On") == 0 || atoi(val)) ? 1 : 0;
            if (v) *b |= (1 << p->bit_shift); else *b &= ~(1 << p->bit_shift);
            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
            return;
        case TP_ENUM:
            if (strcmp(paramName, "filtermode") == 0) {
                if (strcmp(val, "Off") == 0) v = 0;
                else if (strcmp(val, "LPF") == 0) v = 1;
                else if (strcmp(val, "HPF") == 0) v = 2;
                else v = clamp_int(atoi(val), 0, 2);
                *b = (*b & ~(p->bit_mask << p->bit_shift)) | ((v & p->bit_mask) << p->bit_shift);
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                return;
            }
            if (strcmp(paramName, "resonancemode") == 0) {
                v = (strcmp(val, "Hard") == 0 || atoi(val)) ? 1 : 0;
                if (v) *b |= 0x80; else *b &= ~0x80;
                v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
                return;
            }
            v = clamp_int(atoi(val), 0, p->bit_mask);
            *b = (*b & ~(p->bit_mask << p->bit_shift)) | ((v & p->bit_mask) << p->bit_shift);
            v2_queue_tone_sysex(inst, toneIdx, p->sysex_idx, v, p->two_byte);
            return;
    }
}

/* Snap: copy every shared tone param from Tone 1 (idx 0) to tones 2-4 so the
 * four tones immediately share one filter/amp/LFO/matrix shape. Round-trips each
 * value as a string through get/write (all shared params read back as plain
 * numerics that the writer accepts), so it stays correct across param types. */
static void v2_snap_link_tones(jv880_instance_t *inst) {
    if (!inst || !inst->mcu) return;

    /* Back up tones 2,3,4's current 84-byte slices BEFORE we overwrite their
     * shared bytes, so disabling link can restore the originals. The slice
     * fully contains every shared byte the snap below touches (matrix bytes +
     * tone-param bytes all live within each tone's 84-byte region). */
    const int tone2Base = NVRAM_PATCH_OFFSET + 26 + (1 * 84);
    memcpy(inst->link_backup, &inst->mcu->nvram[tone2Base], 3 * 84);
    inst->link_backup_valid = 1;

    char k[48], tmp[48];
    for (size_t i = 0; i < NUM_TONE_PARAMS; i++) {
        const char *nm = TONE_PARAMS[i].name;
        if (!v2_is_shared_tone_param(nm)) continue;
        snprintf(k, sizeof(k), "nvram_tone_0_%s", nm);
        if (v2_get_param(inst, k, tmp, sizeof(tmp)) <= 0) continue;
        for (int t = 1; t < 4; t++) v2_write_one_tone_param(inst, t, nm, tmp);
    }
    for (size_t i = 0; i < NUM_MATRIX_PARAMS; i++) {
        const char *nm = MATRIX_PARAMS[i].name;  /* all matrix params are shared */
        snprintf(k, sizeof(k), "nvram_tone_0_%s", nm);
        if (v2_get_param(inst, k, tmp, sizeof(tmp)) <= 0) continue;
        for (int t = 1; t < 4; t++) v2_write_one_tone_param(inst, t, nm, tmp);
    }
}

/* Restore tones 2,3,4's pre-link shared bytes from the link backup, making the
 * "Link all to 1" toggle reversible. Restores the NVRAM bytes, then re-emits
 * SysEx for every shared param (read back from the restored NVRAM) so the
 * engine state matches the working patch. No-op if no valid backup is held. */
static void v2_restore_link_tones(jv880_instance_t *inst) {
    if (!inst || !inst->mcu || !inst->link_backup_valid) return;

    /* 1) Put the original bytes back into NVRAM. */
    const int tone2Base = NVRAM_PATCH_OFFSET + 26 + (1 * 84);
    memcpy(&inst->mcu->nvram[tone2Base], inst->link_backup, 3 * 84);
    /* NVRAM changed underneath the read cache; force a refresh on next poll. */
    inst->tone_cache_valid = 0;

    /* 2) Re-emit SysEx for every shared param from the now-restored NVRAM so the
     * running engine matches. Reuse v2_write_one_tone_param with the value read
     * back from NVRAM (string round-trip), mirroring v2_snap_link_tones. */
    char k[48], tmp[48];
    for (size_t i = 0; i < NUM_TONE_PARAMS; i++) {
        const char *nm = TONE_PARAMS[i].name;
        if (!v2_is_shared_tone_param(nm)) continue;
        for (int t = 1; t < 4; t++) {
            snprintf(k, sizeof(k), "nvram_tone_%d_%s", t, nm);
            if (v2_get_param(inst, k, tmp, sizeof(tmp)) <= 0) continue;
            v2_write_one_tone_param(inst, t, nm, tmp);
        }
    }
    for (size_t i = 0; i < NUM_MATRIX_PARAMS; i++) {
        const char *nm = MATRIX_PARAMS[i].name;  /* all matrix params are shared */
        for (int t = 1; t < 4; t++) {
            snprintf(k, sizeof(k), "nvram_tone_%d_%s", t, nm);
            if (v2_get_param(inst, k, tmp, sizeof(tmp)) <= 0) continue;
            v2_write_one_tone_param(inst, t, nm, tmp);
        }
    }

    inst->link_backup_valid = 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return;

    /* State restore from patch save */
    if (strcmp(key, "state") == 0) {
        /* If loading isn't complete, queue the state for later application */
        if (!inst->loading_complete) {
            strncpy(inst->pending_state, val, sizeof(inst->pending_state) - 1);
            inst->pending_state[sizeof(inst->pending_state) - 1] = '\0';
            inst->pending_state_valid = 1;
            fprintf(stderr, "JV880 v2: Queued state for deferred restoration\n");
            return;
        }

        float f;
        /* Restore mode first */
        if (json_get_number(val, "mode", &f) == 0) {
            v2_set_mode(inst, (int)f);
        }
        /* Note: expansion_index is saved for state but we don't restore it directly.
         * v2_select_patch will load the correct expansion when it selects the patch.
         * Setting current_expansion here would cause v2_load_expansion_to_emulator
         * to skip loading the actual ROM data. */
        if (json_get_number(val, "expansion_bank_offset", &f) == 0) {
            inst->expansion_bank_offset = (int)f;
        }
        /* Restore macro semantics mode (absent in v1 state -> keep default). */
        if (json_get_number(val, "macro_mode", &f) == 0) {
            inst->macro_mode = ((int)f == MACRO_RELATIVE_RESET)
                ? MACRO_RELATIVE_RESET : MACRO_ABSOLUTE_ANCHORED;
        }
        /* Restore preset or performance based on mode */
        if (inst->performance_mode) {
            if (json_get_number(val, "performance", &f) == 0) {
                int perf = (int)f;
                if (perf >= 0 && perf < NUM_PERFORMANCES) {
                    v2_select_performance(inst, perf);
                }
            }
            if (json_get_number(val, "part", &f) == 0) {
                int part = (int)f;
                if (part >= 0 && part <= 7) {
                    v2_select_part(inst, part);
                }
            }
        } else {
            if (json_get_number(val, "preset", &f) == 0) {
                int preset = (int)f;
                if (preset >= 0 && preset < inst->total_patches) {
                    v2_select_patch(inst, preset);
                }
            }
        }
        /* Restore octave transpose */
        if (json_get_number(val, "octave_transpose", &f) == 0) {
            int oct = (int)f;
            if (oct < -4) oct = -4;
            if (oct > 4) oct = 4;
            inst->octave_transpose = oct;
        }

        /* Restore working patch data from hex string */
        if (inst->mcu) {
            const char *patch_start = strstr(val, "\"patch\":\"");
            if (patch_start) {
                patch_start += 9;  /* Skip past "patch":" */
                const char *patch_end = strchr(patch_start, '"');
                if (patch_end && (patch_end - patch_start) == PATCH_SIZE * 2) {
                    /* Decode hex to NVRAM */
                    for (int i = 0; i < PATCH_SIZE; i++) {
                        unsigned int byte;
                        if (sscanf(patch_start + i * 2, "%2X", &byte) == 1) {
                            inst->mcu->nvram[NVRAM_PATCH_OFFSET + i] = (uint8_t)byte;
                        }
                    }
                    /* Send PC 0 to trigger emulator to reload from NVRAM */
                    uint8_t pc_msg[2] = { 0xC0, 0x00 };
                    pthread_mutex_lock(&inst->ring_mutex);
                    int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
                    if (next != inst->midi_read) {
                        memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
                        inst->midi_queue_len[inst->midi_write] = 2;
                        inst->midi_write = next;
                    }
                    pthread_mutex_unlock(&inst->ring_mutex);
                    fprintf(stderr, "JV880 v2: Restored working patch from state\n");
                }
            }
        }
        return;
    }

    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->total_patches && idx != inst->current_patch) {
            /* Check if this crosses an expansion boundary */
            int new_exp = inst->patches[idx].expansion_index;
            int cur_exp = (inst->current_patch >= 0 && inst->current_patch < inst->total_patches)
                        ? inst->patches[inst->current_patch].expansion_index : -1;
            if (new_exp >= 0 && new_exp != cur_exp) {
                /* Cross-expansion: update UI immediately but defer ROM swap */
                inst->current_patch = idx;
                inst->deferred_patch_index = idx;
                inst->deferred_patch_countdown = 3;  /* ~9ms debounce */
            } else {
                /* Same expansion or internal: load immediately */
                inst->deferred_patch_countdown = 0;  /* Cancel any pending */
                v2_select_patch(inst, idx);
            }
        }
    } else if (strcmp(key, "octave_transpose") == 0) {
        int v = atoi(val);
        if (v < -3) v = -3;
        if (v > 3) v = 3;
        if (v != inst->octave_transpose) {
            v2_send_all_notes_off(inst);
        }
        inst->octave_transpose = v;
    } else if (strcmp(key, "program_change") == 0) {
        int program = atoi(val);
        if (program >= 0 && program < inst->total_patches && program != inst->current_patch) {
            /* Check if this crosses an expansion boundary */
            int new_exp = inst->patches[program].expansion_index;
            int cur_exp = (inst->current_patch >= 0 && inst->current_patch < inst->total_patches)
                        ? inst->patches[inst->current_patch].expansion_index : -1;
            if (new_exp >= 0 && new_exp != cur_exp) {
                /* Cross-expansion: update UI immediately but defer ROM swap */
                inst->current_patch = program;
                inst->deferred_patch_index = program;
                inst->deferred_patch_countdown = 3;
            } else {
                inst->deferred_patch_countdown = 0;
                v2_select_patch(inst, program);
            }
        }
    } else if (strcmp(key, "next_bank") == 0) {
        v2_jump_to_bank(inst, 1);
    } else if (strcmp(key, "prev_bank") == 0) {
        v2_jump_to_bank(inst, -1);
    } else if (strcmp(key, "mode") == 0) {
        /* Switch between patch (0) and performance (1) mode
         * Accept both string names and numeric indices for enum compatibility */
        int mode;
        if (strcmp(val, "Patch") == 0 || strcmp(val, "patch") == 0) {
            mode = 0;
        } else if (strcmp(val, "Performance") == 0 || strcmp(val, "performance") == 0) {
            mode = 1;
        } else {
            mode = atoi(val);
        }
        jv_debug("[set_param] mode='%s' -> %d (current=%s)\n",
                val, mode, inst->performance_mode ? "Performance" : "Patch");
        v2_set_mode(inst, mode);
    } else if (strcmp(key, "performance") == 0) {
        /* Select performance 0-47 */
        int perf = atoi(val);
        if (perf < 0) perf = 0;
        if (perf >= NUM_PERFORMANCES) perf = NUM_PERFORMANCES - 1;
        v2_select_performance(inst, perf);
    } else if (strcmp(key, "part") == 0) {
        /* Select part 0-7 within performance */
        int part = atoi(val);
        if (part < 0) part = 0;
        if (part > 7) part = 7;
        v2_select_part(inst, part);
    } else if (strcmp(key, "load_expansion") == 0) {
        /* Load a specific expansion card by index (for performance mode Card patches) */
        int exp_idx = atoi(val);
        int bank_offset = 0;
        const char *comma = strchr(val, ',');
        if (comma) {
            bank_offset = atoi(comma + 1);
        }
        if (exp_idx >= 0 && exp_idx < inst->expansion_count) {
            int max_offset = (inst->expansions[exp_idx].patch_count > 64) ?
                             ((inst->expansions[exp_idx].patch_count - 1) / 64) * 64 : 0;
            if (bank_offset < 0) bank_offset = 0;
            if (bank_offset > max_offset) bank_offset = max_offset;
            inst->expansion_bank_offset = bank_offset;
            /* Actually load the expansion ROM into the emulator */
            v2_load_expansion_to_emulator(inst, exp_idx);
            fprintf(stderr, "JV880 v2: Loaded expansion %d (%s) at bank offset %d\n",
                    exp_idx, inst->expansions[exp_idx].name, bank_offset);
        }
    } else if (strcmp(key, "jump_to_expansion") == 0) {
        /* Jump to first patch of expansion (for patch browsing) */
        /* -1 = factory patches, 0+ = expansion index */
        int exp_idx = atoi(val);
        if (exp_idx == -1) {
            /* Jump to factory patches (Preset A) */
            v2_select_patch(inst, 0);
            fprintf(stderr, "JV880 v2: Jumped to factory patches\n");
        } else if (exp_idx >= 0 && exp_idx < inst->expansion_count) {
            int first_patch = inst->expansions[exp_idx].first_global_index;
            if (first_patch >= 0 && first_patch < inst->total_patches) {
                v2_select_patch(inst, first_patch);
                fprintf(stderr, "JV880 v2: Jumped to expansion %d (%s) at patch %d\n",
                        exp_idx, inst->expansions[exp_idx].name, first_patch);
            }
        }
    } else if (strcmp(key, "jump_to_internal") == 0) {
        /* Jump to first internal patch (Preset A) */
        v2_select_patch(inst, 0);
        fprintf(stderr, "JV880 v2: Jumped to internal patches\n");
    } else if (strncmp(key, "nvram_patchCommon_", 18) == 0 && inst->mcu) {
        const PatchCommonParam *pc = find_patch_common_param(key + 18);
        if (pc) {
            int v = clamp_int(atoi(val), pc->minv, pc->maxv);
            int mask = ((1 << pc->width) - 1) << pc->shift;
            /* Read-modify-write the NVRAM byte (preserves sibling bitfields)
             * for immediate readback; send the sub-value to its own SysEx
             * address so the emulator applies the sound change. */
            uint8_t *b = &inst->mcu->nvram[NVRAM_PATCH_OFFSET + pc->nvramOffset];
            *b = (uint8_t)((*b & ~mask) | ((v << pc->shift) & mask));
            v2_queue_patch_common_sysex(inst, pc->sysexIdx, v);
        }
    } else if (strncmp(key, "nvram_tone_", 11) == 0 && inst->mcu) {
        int toneIdx = atoi(key + 11);
        const char *underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char *paramName = underscore + 1;
            if (inst->link_tones && v2_is_shared_tone_param(paramName)) {
                /* Subtractive link: a shared param drives all four tones at once
                 * (Tone 1 is the master, but editing any tone's shared control
                 * fans out identically since the tones are kept equal). */
                for (int t = 0; t < 4; t++) v2_write_one_tone_param(inst, t, paramName, val);
            } else {
                v2_write_one_tone_param(inst, toneIdx, paramName, val);
            }
        }
    } else if (strcmp(key, "link_tones") == 0) {
        /* Subtractive mode toggle. On enable, back up + snap Tone 1's shared
         * params onto tones 2-4 so they immediately behave as one voice. On
         * disable, restore tones 2-4's pre-link shared bytes so the toggle is
         * fully reversible. */
        int on = (strcmp(val, "On") == 0 || atoi(val)) ? 1 : 0;
        int was = inst->link_tones;
        inst->link_tones = on;
        if (on && !was) v2_snap_link_tones(inst);
        else if (!on && was) v2_restore_link_tones(inst);
    } else if (strcmp(key, "macro_mode") == 0) {
        /* Runtime A/B switch for macro semantics (not surfaced in UI). */
        if (strcmp(val, "relative") == 0 || strcmp(val, "MACRO_RELATIVE_RESET") == 0) {
            inst->macro_mode = MACRO_RELATIVE_RESET;
        } else {
            inst->macro_mode = MACRO_ABSOLUTE_ANCHORED;
        }
    } else if (strncmp(key, "macro_", 6) == 0 && inst->mcu) {
        /* All macro behavior funnels through macro_write; semantics branch on mode. */
        const macro_def_t *m = find_macro(key + 6);
        if (m) macro_write(inst, m, atoi(val));
    } else if (strncmp(key, "sram_part_", 10) == 0 && inst->mcu) {
        int partIdx = key[10] - '0';
        if (partIdx >= 0 && partIdx < 8 && key[11] == '_') {
            const char *paramName = key + 12;
            const int partBase = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (partIdx * TEMP_PERF_PART_SIZE);
            int sramOffset = -1;
            int sysexIdx = -1;

            /* Map parameter names to SRAM offset and SysEx index */
            if (strcmp(paramName, "partlevel") == 0) { sramOffset = 17; sysexIdx = 25; }
            else if (strcmp(paramName, "partpan") == 0) { sramOffset = 18; sysexIdx = 26; }
            else if (strcmp(paramName, "internalkeyrangelower") == 0) { sramOffset = 10; sysexIdx = 15; }
            else if (strcmp(paramName, "internalkeyrangeupper") == 0) { sramOffset = 11; sysexIdx = 16; }
            else if (strcmp(paramName, "internalvelocitysense") == 0) { sramOffset = 13; sysexIdx = 18; }
            else if (strcmp(paramName, "internalvelocitymax") == 0) { sramOffset = 14; sysexIdx = 19; }

            /* Handle patchbank - selects which bank patchnumber refers to
             * Accepts: "User", "Internal", "Preset A", "Preset B" or numeric 0-3
             * Also triggers patch load at current index from new bank
             * Note: "Card" bank removed - expansion waveforms accessed via User patches */
            if (strcmp(paramName, "patchbank") == 0) {
                int bank;
                if (strcmp(val, "User") == 0) bank = 0;
                else if (strcmp(val, "Internal") == 0) bank = 1;
                else if (strcmp(val, "Preset A") == 0) bank = 2;
                else if (strcmp(val, "Preset B") == 0) bank = 3;
                else bank = clamp_int(atoi(val), 0, 3);
                inst->part_patchbank[partIdx] = bank;
                jv_debug("[JV880] Part %d patchbank set to %s (%d)\n", partIdx + 1, val, bank);

                /* Get current patch index (0-63) and load from new bank */
                int currentPatch = inst->mcu->sram[partBase + 16];
                int patchInBank = currentPatch % 64;
                int actualPatchNum;

                switch (bank) {
                    case 0:  /* User - copy user patch to temp area for playback */
                        if (inst->mcu && patchInBank < NUM_USER_PATCHES) {
                            uint32_t src = NVRAM_PATCH_INTERNAL + (patchInBank * PATCH_SIZE);
                            if (inst->mcu->nvram[src] != 0xFF) {
                                uint32_t dest = patchInBank * PATCH_SIZE;
                                if (dest + PATCH_SIZE <= CARDRAM_SIZE) {
                                    memcpy(&inst->mcu->cardram[dest],
                                           &inst->mcu->nvram[src], PATCH_SIZE);
                                }
                            }
                        }
                        actualPatchNum = 64 + patchInBank;  /* Use cardram range */
                        break;
                    case 1: actualPatchNum = patchInBank; break;         /* Internal: 0-63 */
                    case 2: actualPatchNum = 128 + patchInBank; break;   /* Preset A: 128-191 */
                    case 3: actualPatchNum = 192 + patchInBank; break;   /* Preset B: 192-255 */
                    default: actualPatchNum = patchInBank; break;
                }

                jv_debug("[JV880] Part %d loading patch %d from bank %d (actual: %d)\n",
                        partIdx + 1, patchInBank, bank, actualPatchNum);

                inst->mcu->sram[partBase + 16] = (uint8_t)actualPatchNum;
                v2_queue_part_sysex(inst, partIdx, 0x17, actualPatchNum, 1);
                return;
            }

            /* Handle patchnumber - 0-63 within selected bank
             * Combined with patchbank to determine actual patch */
            if (strcmp(paramName, "patchnumber") == 0) {
                int patchInBank = clamp_int(atoi(val), 0, 63);
                int bank = inst->part_patchbank[partIdx];
                int actualPatchNum;

                switch (bank) {
                    case 0:  /* User - copy user patch to cardram for playback */
                        if (inst->mcu && patchInBank < NUM_USER_PATCHES) {
                            uint32_t src = NVRAM_PATCH_INTERNAL + (patchInBank * PATCH_SIZE);
                            if (inst->mcu->nvram[src] != 0xFF) {
                                uint32_t dest = patchInBank * PATCH_SIZE;
                                if (dest + PATCH_SIZE <= CARDRAM_SIZE) {
                                    memcpy(&inst->mcu->cardram[dest],
                                           &inst->mcu->nvram[src], PATCH_SIZE);
                                }
                            }
                        }
                        actualPatchNum = 64 + patchInBank;  /* Use cardram range */
                        break;
                    case 1:  /* Internal: 0-63 */
                        actualPatchNum = patchInBank;
                        break;
                    case 2:  /* Preset A: 128-191 */
                        actualPatchNum = 128 + patchInBank;
                        break;
                    case 3:  /* Preset B: 192-255 */
                        actualPatchNum = 192 + patchInBank;
                        break;
                    default:
                        actualPatchNum = patchInBank;
                        break;
                }

                inst->mcu->sram[partBase + 16] = (uint8_t)actualPatchNum;
                v2_queue_part_sysex(inst, partIdx, 0x17, actualPatchNum, 1);
                return;
            }

            /* Handle reverbswitch and chorusswitch (bit fields)
             * Accepts: "Off", "On" or numeric 0, 1 */
            if (strcmp(paramName, "reverbswitch") == 0) {
                int v;
                if (strcmp(val, "On") == 0) v = 1;
                else if (strcmp(val, "Off") == 0) v = 0;
                else v = atoi(val) ? 1 : 0;
                uint8_t *b = &inst->mcu->sram[partBase + 21];
                if (v) *b |= 0x40;
                else *b &= ~0x40;
                v2_queue_part_sysex(inst, partIdx, 29, v, 0);
                return;
            }
            if (strcmp(paramName, "chorusswitch") == 0) {
                int v;
                if (strcmp(val, "On") == 0) v = 1;
                else if (strcmp(val, "Off") == 0) v = 0;
                else v = atoi(val) ? 1 : 0;
                uint8_t *b = &inst->mcu->sram[partBase + 21];
                if (v) *b |= 0x20;
                else *b &= ~0x20;
                v2_queue_part_sysex(inst, partIdx, 30, v, 0);
                return;
            }

            /* Handle signed parameters (coarse/fine tune, transpose) */
            if (strcmp(paramName, "partcoarsetune") == 0) { sramOffset = 19; sysexIdx = 27; }
            else if (strcmp(paramName, "partfinetune") == 0) { sramOffset = 20; sysexIdx = 28; }
            else if (strcmp(paramName, "internalkeytranspose") == 0) { sramOffset = 12; sysexIdx = 17; }

            if (sramOffset >= 0 && sysexIdx >= 0) {
                int v = clamp_int(atoi(val), 0, 127);
                /* Check if this is a signed parameter (tune/transpose) */
                if (sramOffset == 19 || sramOffset == 20 || sramOffset == 12) {
                    int stored = v - 64;
                    if (stored < -64) stored = -64;
                    if (stored > 63) stored = 63;
                    inst->mcu->sram[partBase + sramOffset] = (uint8_t)(int8_t)stored;
                } else {
                    inst->mcu->sram[partBase + sramOffset] = (uint8_t)v;
                }
                v2_queue_part_sysex(inst, partIdx, sysexIdx, v, 0);
            }
        }
    } else if (strncmp(key, "write_patch_", 12) == 0 && inst->mcu) {
        /* Write current working patch to user patch slot (0-63)
         * Copies working patch from NVRAM_PATCH_OFFSET to NVRAM_PATCH_INTERNAL
         * Usage: write_patch_<slot> where slot is 0-63 */
        int slot = atoi(key + 12);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Wrote patch '%s' to User slot %d (NVRAM 0x%04x)\n",
                    name, slot + 1, dest_offset);
        } else {
            fprintf(stderr, "JV880 v2: Invalid patch slot %d (must be 0-63)\n", slot);
        }
    } else if (strncmp(key, "write_performance_", 18) == 0 && inst->mcu) {
        /* Write temp performance to Internal slot (0-15)
         * Copies temp performance from SRAM to NVRAM Internal slot */
        int slot = atoi(key + 18);
        if (slot >= 0 && slot < 16) {
            uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (slot * PERF_SIZE);
            memcpy(&inst->mcu->nvram[nvram_offset],
                   &inst->mcu->sram[SRAM_TEMP_PERF_OFFSET], PERF_SIZE);
            char name[13];
            memcpy(name, &inst->mcu->sram[SRAM_TEMP_PERF_OFFSET], 12);
            name[12] = '\0';
            fprintf(stderr, "JV880 v2: Wrote performance '%s' to Internal slot %d (NVRAM 0x%04x)\n",
                    name, slot + 1, nvram_offset);
        } else {
            fprintf(stderr, "JV880 v2: Invalid performance slot %d (must be 0-15)\n", slot);
        }
    } else if (strcmp(key, "save_nvram") == 0 && inst->mcu) {
        /* Save NVRAM to disk (persists patches and performances) */
        char path[1024];
        snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
            fclose(f);
            chown_to_ableton(path);
            fprintf(stderr, "JV880 v2: Saved NVRAM to %s\n", path);
        } else {
            fprintf(stderr, "JV880 v2: Failed to save NVRAM to %s\n", path);
        }
    } else if (strncmp(key, "save_to_slot_", 13) == 0 && inst->mcu) {
        /* Save current working patch to user slot 1-64 (menu items) */
        int slot = atoi(key + 13) - 1;  /* Convert 1-64 to 0-63 */
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
                chown_to_ableton(path);
            }
        }
    } else if (strncmp(key, "load_from_slot_", 15) == 0 && inst->mcu) {
        /* Load user patch from slot 1-64 (menu items) */
        int slot = atoi(key + 15) - 1;  /* Convert 1-64 to 0-63 */
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            /* Check if slot has valid data (not 0xFF filled) */
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded patch '%s' from User slot %d\n", name, slot + 1);
            } else {
                fprintf(stderr, "JV880 v2: User slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "save_to_user_slot") == 0 && inst->mcu) {
        /* Save current working patch to user slot (0-63) - for UI menu */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
                chown_to_ableton(path);
            }
        }
    } else if (strcmp(key, "do_save_to_slot") == 0 && inst->mcu) {
        /* Save to slot - triggered by items_param/select_param UI pattern */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
                chown_to_ableton(path);
            }
        }
    } else if (strcmp(key, "do_load_from_slot") == 0 && inst->mcu) {
        /* Load from slot - triggered by items_param/select_param UI pattern */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded patch '%s' from User slot %d\n", name, slot + 1);
            } else {
                fprintf(stderr, "JV880 v2: User slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "save_slot") == 0 && inst->mcu) {
        /* Save slot browser - selecting a slot saves current patch there */
        int slot = atoi(val);
        inst->save_slot_index = slot;
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t dest_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            memcpy(&inst->mcu->nvram[dest_offset],
                   &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            memcpy(name, &inst->mcu->nvram[NVRAM_PATCH_OFFSET], PATCH_NAME_LEN);
            name[PATCH_NAME_LEN] = '\0';
            fprintf(stderr, "JV880 v2: Saved patch '%s' to User slot %d\n", name, slot + 1);
            /* Auto-save NVRAM to persist */
            char path[1024];
            snprintf(path, sizeof(path), "%s/roms/jv880_nvram.bin", inst->module_dir);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(inst->mcu->nvram, 1, NVRAM_SIZE, f);
                fclose(f);
                chown_to_ableton(path);
            }
        }
    } else if (strcmp(key, "load_slot") == 0 && inst->mcu) {
        /* Load slot browser - selecting a slot loads that patch */
        int slot = atoi(val);
        inst->load_slot_index = slot;
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                /* Copy user patch to working patch area */
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded user patch '%s' from slot %d\n", name, slot + 1);

                /* Ensure patch mode in NVRAM (like v2_select_patch does) */
                inst->mcu->nvram[NVRAM_MODE_OFFSET] = 1;

                /* Trigger emulator to reload patch via PC 0 */
                uint8_t pc_msg[2] = { 0xC0, 0x00 };
                pthread_mutex_lock(&inst->ring_mutex);
                int next = (inst->midi_write + 1) % MIDI_QUEUE_SIZE;
                if (next != inst->midi_read) {
                    memcpy(inst->midi_queue[inst->midi_write], pc_msg, 2);
                    inst->midi_queue_len[inst->midi_write] = 2;
                    inst->midi_write = next;
                }
                pthread_mutex_unlock(&inst->ring_mutex);
            } else {
                fprintf(stderr, "JV880 v2: User patch slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "load_user_patch") == 0 && inst->mcu) {
        /* Load a user patch from NVRAM into working area (legacy) */
        int slot = atoi(val);
        if (slot >= 0 && slot < NUM_USER_PATCHES) {
            uint32_t src_offset = NVRAM_PATCH_INTERNAL + (slot * PATCH_SIZE);
            /* Check if slot has valid data (not 0xFF filled) */
            if (inst->mcu->nvram[src_offset] != 0xFF) {
                memcpy(&inst->mcu->nvram[NVRAM_PATCH_OFFSET],
                       &inst->mcu->nvram[src_offset], PATCH_SIZE);
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[src_offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                fprintf(stderr, "JV880 v2: Loaded user patch '%s' from slot %d\n", name, slot + 1);
            } else {
                fprintf(stderr, "JV880 v2: User patch slot %d is empty\n", slot + 1);
            }
        }
    } else if (strcmp(key, "run_param_test") == 0 && inst->mcu) {
        /* Automated parameter offset verification test */
        fprintf(stderr, "\n");
        fprintf(stderr, "============================================\n");
        fprintf(stderr, "=== AUTOMATED PARAMETER OFFSET TEST (v2) ===\n");
        fprintf(stderr, "============================================\n\n");

        int pass = 0, fail = 0;
        const int toneIdx = 0;
        const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

        struct { const char* name; int offset; uint8_t testVal; } tests[] = {
            {"level", 67, 0x63},
            {"pan", 68, 0x40},
            {"tvaenvtime1", 74, 0x4A},
            {"tvaenvtime2", 76, 0x4C},
            {"tvaenvtime3", 78, 0x4E},
            {"tvaenvtime4", 80, 0x50},
            {"drylevel", 81, 0x51},
            {"reverbsendlevel", 82, 0x52},
            {"chorussendlevel", 83, 0x53},
            {"cutofffrequency", 52, 0x7F},
            {"resonance", 53, 0x32},
            {"pitchcoarse", 37, 0x40},
            {"pitchfine", 38, 0x41},
        };
        const int numTests = sizeof(tests) / sizeof(tests[0]);

        uint8_t origValues[20];
        for (int i = 0; i < numTests; i++) {
            origValues[i] = inst->mcu->nvram[toneBase + tests[i].offset];
        }

        fprintf(stderr, "Testing tone %d parameters (base=0x%04x):\n\n", toneIdx, toneBase);

        for (int i = 0; i < numTests; i++) {
            inst->mcu->nvram[toneBase + tests[i].offset] = tests[i].testVal;
            uint8_t readVal = inst->mcu->nvram[toneBase + tests[i].offset];

            if (readVal == tests[i].testVal) {
                fprintf(stderr, "  ✓ PASS: %-20s offset=%2d wrote=0x%02x read=0x%02x\n",
                        tests[i].name, tests[i].offset, tests[i].testVal, readVal);
                pass++;
            } else {
                fprintf(stderr, "  ✗ FAIL: %-20s offset=%2d wrote=0x%02x read=0x%02x\n",
                        tests[i].name, tests[i].offset, tests[i].testVal, readVal);
                fail++;
            }
        }

        for (int i = 0; i < numTests; i++) {
            inst->mcu->nvram[toneBase + tests[i].offset] = origValues[i];
        }

        fprintf(stderr, "\n--------------------------------------------\n");
        fprintf(stderr, "Results: %d passed, %d failed\n", pass, fail);
        fprintf(stderr, "============================================\n\n");
    } else if (strcmp(key, "dump_tone_layout") == 0 && inst->mcu) {
        /* Dump current tone structure for verification */
        const int toneIdx = 0;
        const int toneBase = NVRAM_PATCH_OFFSET + 26 + (toneIdx * 84);

        fprintf(stderr, "\n=== Tone %d Structure (v2, base=0x%04x) ===\n\n", toneIdx, toneBase);
        fprintf(stderr, "--- TVA Section (67-83) ---\n");
        fprintf(stderr, "  67 tvaLevel:      %3d\n", inst->mcu->nvram[toneBase+67]);
        fprintf(stderr, "  68 tvaPan:        %3d\n", inst->mcu->nvram[toneBase+68]);
        fprintf(stderr, "  74 tvaEnvTime1:   %3d\n", inst->mcu->nvram[toneBase+74]);
        fprintf(stderr, "  76 tvaEnvTime2:   %3d\n", inst->mcu->nvram[toneBase+76]);
        fprintf(stderr, "  78 tvaEnvTime3:   %3d\n", inst->mcu->nvram[toneBase+78]);
        fprintf(stderr, "  80 tvaEnvTime4:   %3d\n", inst->mcu->nvram[toneBase+80]);
        fprintf(stderr, "  81 drySend:       %3d\n", inst->mcu->nvram[toneBase+81]);
        fprintf(stderr, "  82 reverbSend:    %3d\n", inst->mcu->nvram[toneBase+82]);
        fprintf(stderr, "  83 chorusSend:    %3d\n", inst->mcu->nvram[toneBase+83]);
        fprintf(stderr, "\n--- TVF Section (52-53) ---\n");
        fprintf(stderr, "  52 tvfCutoff:     %3d\n", inst->mcu->nvram[toneBase+52]);
        fprintf(stderr, "  53 tvfResonance:  %3d\n", inst->mcu->nvram[toneBase+53]);
        fprintf(stderr, "\n=== End Tone Layout ===\n");
    }
}

/* Tone param cache TTL. Cache storage is per-instance (jv880_instance_t). */
#define TONE_CACHE_TTL_MS 50  /* Refresh cache every 50ms */

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* v2: Get parameter */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst) return -1;

    /* Fast path for frequently-accessed tone params - uses cache to avoid NVRAM reads */
    if (strncmp(key, "nvram_tone_", 11) == 0 && inst->mcu) {
        int toneIdx = atoi(key + 11);
        const char* underscore = strchr(key + 11, '_');
        if (underscore && toneIdx >= 0 && toneIdx < 4) {
            const char* paramName = underscore + 1;

            /* Refresh cache if stale */
            uint64_t now = get_time_ms();
            if (inst->tone_cache_valid == 0 ||
                now - inst->tone_cache_valid > TONE_CACHE_TTL_MS) {
                int nvramBase = NVRAM_PATCH_OFFSET + 26;
                memcpy(inst->tone_cache, &inst->mcu->nvram[nvramBase], TONE_CACHE_SIZE);
                inst->tone_cache_valid = now;
            }

            /* Per-tone control matrix (dest nibble / signed sensitivity). */
            const MatrixParam *mx = find_matrix_param(paramName);
            if (mx) {
                uint8_t mbyte = inst->tone_cache[(toneIdx * 84) + mx->off];
                if (mx->isSens) return snprintf(buf, buf_len, "%d", (int)(int8_t)mbyte);
                return snprintf(buf, buf_len, "%d", (mbyte >> mx->shift) & ((1 << mx->width) - 1));
            }

            const ToneParamEntry *p = find_tone_param(paramName);
            if (p) {
                int cacheOffset = (toneIdx * 84) + p->nvram_offset;
                uint8_t byte = inst->tone_cache[cacheOffset];
                switch (p->type) {
                    case TP_BYTE:
                        if (p->signed_param == 1) {
                            /* Signed param: read as int8_t for proper sign extension */
                            return snprintf(buf, buf_len, "%d", (int)(int8_t)byte);
                        } else if (p->signed_param == 2) {
                            /* Pan special case: stored with +64 offset, display as -64 to +63 */
                            return snprintf(buf, buf_len, "%d", (int)byte - 64);
                        } else {
                            return snprintf(buf, buf_len, "%d", byte);
                        }
                    case TP_BITFIELD:
                        /* FXM depth special case: stored 0-15, display 1-16 */
                        if (strcmp(paramName, "fxmdepth") == 0) {
                            return snprintf(buf, buf_len, "%d", ((byte >> p->bit_shift) & p->bit_mask) + 1);
                        }
                        return snprintf(buf, buf_len, "%d", (byte >> p->bit_shift) & p->bit_mask);
                    case TP_BOOL:
                        /* Return numeric index (0/1); the UI maps it to an enum
                         * label via the chain_params "options" array. SET still
                         * accepts both "On"/"Off" and numeric, so this is safe. */
                        return snprintf(buf, buf_len, "%d", (byte & (1 << p->bit_shift)) ? 1 : 0);
                    case TP_ENUM:
                        /* Return numeric index; UI formats via chain_params options.
                         * resonancemode is a single 0x80 bit, others use mask. */
                        if (strcmp(paramName, "resonancemode") == 0) {
                            return snprintf(buf, buf_len, "%d", (byte & 0x80) ? 1 : 0);
                        }
                        return snprintf(buf, buf_len, "%d", (byte >> p->bit_shift) & p->bit_mask);
                }
            }
        }
    }

    /* Knob overlay params: knob_N_name and knob_N_value for shim overlay display */
    if (strncmp(key, "knob_", 5) == 0) {
        int knob_num;
        char action[32];
        if (sscanf(key + 5, "%d_%31s", &knob_num, action) == 2 && knob_num >= 1 && knob_num <= 8) {
            /* Knob-to-param mapping based on current mode */
            static const char *patch_knob_keys[8] = {
                "macro_cutoff", "macro_resonance", "macro_attack", "macro_decay",
                "macro_release", "macro_tvf_env_depth",
                "nvram_patchCommon_reverblevel", "nvram_patchCommon_choruslevel"
            };
            static const char *patch_knob_labels[8] = {
                "Cutoff", "Resonance", "Attack", "Decay",
                "Release", "Filter Env", "Reverb", "Chorus"
            };
            static const char *perf_knob_keys[8] = {
                "partlevel", "partpan", "reverbswitch", "chorusswitch",
                "partcoarsetune", "partfinetune",
                "internalkeyrangelower", "internalkeyrangeupper"
            };
            static const char *perf_knob_labels[8] = {
                "Level", "Pan", "Reverb", "Chorus",
                "Coarse Tune", "Fine Tune", "Key Lo", "Key Hi"
            };

            int idx = knob_num - 1;  /* 0-based */
            int is_perf = inst->performance_mode;
            const char **labels = is_perf ? perf_knob_labels : patch_knob_labels;
            const char **keys = is_perf ? perf_knob_keys : patch_knob_keys;

            if (strcmp(action, "name") == 0) {
                return snprintf(buf, buf_len, "%s", labels[idx]);
            }
            else if (strcmp(action, "value") == 0) {
                /* Get current value of mapped parameter */
                const char *pkey = keys[idx];
                if (strncmp(pkey, "macro_", 6) == 0) {
                    /* Macro params: current display value (mode-dependent). */
                    const macro_def_t *m = find_macro(pkey + 6);
                    return snprintf(buf, buf_len, "%d", m ? macro_read(inst, m) : 0);
                } else {
                    /* Forward to normal get_param for nvram/sram params */
                    return v2_get_param(instance, pkey, buf, buf_len);
                }
            }
        }
        return -1;
    }

    /* Macro mode (A/B selector, readable). */
    if (strcmp(key, "macro_mode") == 0) {
        return snprintf(buf, buf_len, "%s",
            inst->macro_mode == MACRO_RELATIVE_RESET ? "relative" : "absolute");
    }

    /* Macro params: current display value (mode-dependent, via the funnel). */
    if (strncmp(key, "macro_", 6) == 0) {
        const macro_def_t *m = find_macro(key + 6);
        if (!m) return -1;
        return snprintf(buf, buf_len, "%d", macro_read(inst, m));
    }

    /* Fast path for sram_part_ params (performance mode editing) */
    if (strncmp(key, "sram_part_", 10) == 0 && inst->mcu) {
        int partIdx = key[10] - '0';
        if (partIdx >= 0 && partIdx < 8 && key[11] == '_') {
            const char* paramName = key + 12;
            int partBase = SRAM_TEMP_PERF_OFFSET + TEMP_PERF_COMMON_SIZE + (partIdx * TEMP_PERF_PART_SIZE);
            int offset = -1;

            /* Handle patchbank - return stored value, derive from SRAM only if uninitialized (-1) */
            if (strcmp(paramName, "patchbank") == 0) {
                static const char *bank_names[] = {"User", "Internal", "Preset A", "Preset B"};
                int bank = inst->part_patchbank[partIdx];

                /* If not yet initialized, derive from actual patch number */
                if (bank < 0 || bank > 3) {
                    int actualPatch = inst->mcu->sram[partBase + 16];
                    if (actualPatch < 64) bank = 1;        /* Internal */
                    else if (actualPatch < 128) bank = 0;  /* User (via cardram) */
                    else if (actualPatch < 192) bank = 2;  /* Preset A */
                    else bank = 3;                         /* Preset B */
                    inst->part_patchbank[partIdx] = bank;
                }
                return snprintf(buf, buf_len, "%s", bank_names[bank]);
            }

            /* Handle patchnumber - return 0-63 within selected bank */
            if (strcmp(paramName, "patchnumber") == 0) {
                int actualPatch = inst->mcu->sram[partBase + 16];
                int patchInBank = actualPatch % 64;  /* Extract 0-63 within bank */
                return snprintf(buf, buf_len, "%d", patchInBank);
            }

            /* Direct storage parameters */
            if (strcmp(paramName, "partlevel") == 0) offset = 17;
            else if (strcmp(paramName, "partpan") == 0) offset = 18;
            else if (strcmp(paramName, "internalkeyrangelower") == 0) offset = 10;
            else if (strcmp(paramName, "internalkeyrangeupper") == 0) offset = 11;

            /* Handle reverbswitch and chorusswitch as bit fields */
            if (strcmp(paramName, "reverbswitch") == 0) {
                uint8_t b = inst->mcu->sram[partBase + 21];
                return snprintf(buf, buf_len, "%s", ((b >> 6) & 1) ? "On" : "Off");
            }
            if (strcmp(paramName, "chorusswitch") == 0) {
                uint8_t b = inst->mcu->sram[partBase + 21];
                return snprintf(buf, buf_len, "%s", ((b >> 5) & 1) ? "On" : "Off");
            }

            /* Signed offset parameters */
            if (strcmp(paramName, "partcoarsetune") == 0 ||
                strcmp(paramName, "partfinetune") == 0 ||
                strcmp(paramName, "internalkeytranspose") == 0) {
                if (strcmp(paramName, "partcoarsetune") == 0) offset = 19;
                else if (strcmp(paramName, "partfinetune") == 0) offset = 20;
                else if (strcmp(paramName, "internalkeytranspose") == 0) offset = 12;
                if (offset >= 0) {
                    int8_t stored = (int8_t)inst->mcu->sram[partBase + offset];
                    int val = stored + 64;
                    return snprintf(buf, buf_len, "%d", val);
                }
            }

            if (offset >= 0) {
                return snprintf(buf, buf_len, "%d", inst->mcu->sram[partBase + offset]);
            }
        }
    }

    /* Fast path for nvram_patchCommon_ params (FX + control section) */
    if (strncmp(key, "nvram_patchCommon_", 18) == 0 && inst->mcu) {
        const PatchCommonParam *pc = find_patch_common_param(key + 18);
        if (pc) {
            uint8_t b = inst->mcu->nvram[NVRAM_PATCH_OFFSET + pc->nvramOffset];
            int v = (b >> pc->shift) & ((1 << pc->width) - 1);
            return snprintf(buf, buf_len, "%d", v);
        }
    }

    if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_name") == 0 || strcmp(key, "name") == 0) {
        if (!inst->loading_complete) {
            return snprintf(buf, buf_len, "%s", inst->loading_status);
        }
        if (inst->performance_mode) {
            /* In performance mode, read name from ROM2/NVRAM */
            int idx = inst->current_performance;
            if (idx >= 0 && idx < NUM_PERFORMANCES) {
                int bank = idx / PERFS_PER_BANK;
                int perf_in_bank = idx % PERFS_PER_BANK;
                uint8_t name_buf[PERF_NAME_LEN + 1];
                memset(name_buf, 0, sizeof(name_buf));
                int got_name = 0;

                if (bank == 2 && inst->mcu) {
                    /* Internal - read from NVRAM */
                    uint32_t nvram_offset = NVRAM_PERF_INTERNAL + (perf_in_bank * PERF_SIZE);
                    if (nvram_offset + PERF_NAME_LEN <= 0x8000) {
                        memcpy(name_buf, &inst->mcu->nvram[nvram_offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                } else if (inst->rom2) {
                    /* Preset A/B - read from ROM2 */
                    uint32_t offset = (bank == 0) ? PERF_OFFSET_PRESET_A : PERF_OFFSET_PRESET_B;
                    offset += perf_in_bank * PERF_SIZE;
                    if (offset + PERF_NAME_LEN <= 0x40000) {
                        memcpy(name_buf, &inst->rom2[offset], PERF_NAME_LEN);
                        got_name = 1;
                    }
                }

                if (got_name) {
                    /* Trim trailing spaces */
                    int len = PERF_NAME_LEN;
                    while (len > 0 && (name_buf[len - 1] == ' ' || name_buf[len - 1] == 0)) len--;
                    name_buf[len] = '\0';
                    return snprintf(buf, buf_len, "%s", name_buf);
                }
            }
            return snprintf(buf, buf_len, "---");
        }
        if (inst->current_patch >= 0 && inst->current_patch < inst->total_patches) {
            return snprintf(buf, buf_len, "%s", inst->patches[inst->current_patch].name);
        }
        return snprintf(buf, buf_len, "Mini-JV");
    }
    if (strcmp(key, "preset_count") == 0 || strcmp(key, "total_patches") == 0) {
        return snprintf(buf, buf_len, "%d", inst->total_patches);
    }
    if (strcmp(key, "current_patch") == 0 || strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_patch);
    }
    if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    }
    if (strcmp(key, "link_tones") == 0) {
        return snprintf(buf, buf_len, "%d", inst->link_tones);
    }
    /* State serialization for patch save/load */
    if (strcmp(key, "state") == 0) {
        /* NOTE: link_tones is a transient editing overlay (its reversal backup
         * is not persisted), so it is intentionally NOT serialized here — the
         * state SET parser doesn't restore it, and emitting it would imply a
         * round-trip that doesn't exist. Patch save captures the live (snapped)
         * tone bytes, which is the correct persisted result. */
        int written = snprintf(buf, buf_len,
            "{\"version\":2,\"mode\":%d,\"preset\":%d,\"performance\":%d,\"part\":%d,\"octave_transpose\":%d,"
            "\"expansion_index\":%d,\"expansion_bank_offset\":%d,\"macro_mode\":%d",
            inst->performance_mode ? 1 : 0,
            inst->current_patch,
            inst->current_performance,
            inst->current_part,
            inst->octave_transpose,
            inst->current_expansion,
            inst->expansion_bank_offset,
            (int)inst->macro_mode);

        /* Include working patch data as hex if MCU is ready */
        if (inst->mcu && written < buf_len - 800) {
            written += snprintf(buf + written, buf_len - written, ",\"patch\":\"");
            for (int i = 0; i < PATCH_SIZE && written < buf_len - 10; i++) {
                written += snprintf(buf + written, buf_len - written, "%02X",
                    inst->mcu->nvram[NVRAM_PATCH_OFFSET + i]);
            }
            written += snprintf(buf + written, buf_len - written, "\"");
        }

        /* Flat native-int param values so schwung-manager's bulk init path
         * (sendAllParamsAtOnce, which reads this "state" key) can seed the
         * graphical remote UI — without these it forwards only the fields
         * above and every macro/tone/patch-common control inits blank.
         * Emitted AFTER the authoritative patch hex and behind a hard size
         * guard (also bounded to the 16KB chain-autosave buffer), so "state"
         * stays valid JSON and save/restore can never be truncated. The
         * "state" set parser looks up only named keys, so these are ignored
         * on restore; values come from this same get_param, so they always
         * match what the UI controls expect. */
        if (inst->mcu && inst->loading_complete) {
            const int SAFE = (buf_len < 15800 ? buf_len : 15800) - 64;
            char tmp[48], k[48];
            for (size_t mi = 0; mi < NUM_MACROS && written < SAFE; mi++) {
                snprintf(k, sizeof(k), "macro_%s", MACRO_DEFS[mi].key);
                int n = v2_get_param(instance, k, tmp, sizeof(tmp));
                if (n > 0) written += snprintf(buf + written, buf_len - written, ",\"%s\":%s", k, tmp);
            }
            for (size_t pi = 0; pi < NUM_PATCH_COMMON_PARAMS && written < SAFE; pi++) {
                snprintf(k, sizeof(k), "nvram_patchCommon_%s", PATCH_COMMON_PARAMS[pi].name);
                int n = v2_get_param(instance, k, tmp, sizeof(tmp));
                if (n > 0) written += snprintf(buf + written, buf_len - written, ",\"%s\":%s", k, tmp);
            }
            for (int t = 0; t < 4 && written < SAFE; t++) {
                for (size_t ti = 0; ti < sizeof(TONE_PARAMS)/sizeof(TONE_PARAMS[0]) && written < SAFE; ti++) {
                    snprintf(k, sizeof(k), "nvram_tone_%d_%s", t, TONE_PARAMS[ti].name);
                    int n = v2_get_param(instance, k, tmp, sizeof(tmp));
                    if (n > 0) written += snprintf(buf + written, buf_len - written, ",\"%s\":%s", k, tmp);
                }
            }
        }
        written += snprintf(buf + written, buf_len - written, "}");
        return written;
    }
    if (strcmp(key, "loading_complete") == 0) {
        return snprintf(buf, buf_len, "%d", inst->loading_complete);
    }
    if (strcmp(key, "loading_status") == 0) {
        return snprintf(buf, buf_len, "%s", inst->loading_status);
    }
    if (strcmp(key, "audio_diag") == 0) {
        int avail = v2_ring_available(inst);
        return snprintf(buf, buf_len, "underruns=%d renders=%d ring=%d/%d min=%d",
                inst->underrun_count, inst->render_count, avail, AUDIO_RING_SIZE,
                inst->min_buffer_level);
    }
    if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "28");
    }
    /* Bank information */
    if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->bank_count);
    }
    if (strcmp(key, "bank_name") == 0) {
        /* During loading, show loading status */
        if (!inst->loading_complete) {
            return snprintf(buf, buf_len, "Loading...");
        }
        if (inst->performance_mode) {
            /* Performance mode - return performance bank name */
            static const char* perf_bank_names[] = {"Preset A", "Preset B", "Internal"};
            int bank = inst->current_performance / PERFS_PER_BANK;
            if (bank >= 0 && bank < NUM_PERF_BANKS) {
                return snprintf(buf, buf_len, "%s", perf_bank_names[bank]);
            }
            return snprintf(buf, buf_len, "Performances");
        }
        /* Patch mode - return current expansion/bank name */
        if (inst->current_patch >= 0 && inst->current_patch < inst->total_patches) {
            int bank = v2_get_bank_for_patch(inst, inst->current_patch);
            if (bank >= 0 && bank < inst->bank_count) {
                return snprintf(buf, buf_len, "%s", inst->bank_names[bank]);
            }
        }
        return snprintf(buf, buf_len, "Patches");
    }
    if (strcmp(key, "patch_in_bank") == 0) {
        if (inst->performance_mode) {
            /* Return 1-indexed position within performance bank */
            int pos = (inst->current_performance % PERFS_PER_BANK) + 1;
            return snprintf(buf, buf_len, "%d", pos);
        } else {
            /* Return 1-indexed position within current patch bank */
            int bank = v2_get_bank_for_patch(inst, inst->current_patch);
            int pos = inst->current_patch - inst->bank_starts[bank] + 1;
            return snprintf(buf, buf_len, "%d", pos);
        }
    }
    /* Mode information - return string for enum compatibility */
    if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", inst->performance_mode ? "Performance" : "Patch");
    }
    if (strcmp(key, "performance_mode") == 0) {
        return snprintf(buf, buf_len, "%d", inst->performance_mode);
    }
    if (strcmp(key, "current_performance") == 0 || strcmp(key, "performance") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_performance);
    }
    if (strcmp(key, "current_part") == 0 || strcmp(key, "part") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_part);
    }
    if (strcmp(key, "num_performances") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_PERFORMANCES);
    }
    if (strcmp(key, "num_parts") == 0) {
        return snprintf(buf, buf_len, "8");
    }
    /* Expansion information */
    if (strcmp(key, "expansion_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->expansion_count);
    }
    if (strcmp(key, "current_expansion") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_expansion);
    }
    if (strcmp(key, "expansion_bank_offset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->expansion_bank_offset);
    }
    /* Expansion list for "Choose Expansion" menu - returns JSON array */
    if (strcmp(key, "expansion_list") == 0) {
        /* Include factory patches as first entry with index -1 */
        int written = snprintf(buf, buf_len, "[{\"index\":-1,\"name\":\"Factory (Preset A)\",\"first_patch\":0,\"patch_count\":128}");
        for (int i = 0; i < inst->expansion_count && written < buf_len - 100; i++) {
            written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"index\":%d,\"name\":\"%s\",\"first_patch\":%d,\"patch_count\":%d}",
                i, inst->expansions[i].name, inst->expansions[i].first_global_index,
                inst->expansions[i].patch_count);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Patchbank list for part bank selection (no Card - expansion waveforms via User patches) */
    if (strcmp(key, "patchbank_list") == 0) {
        return snprintf(buf, buf_len,
            "[{\"index\":1,\"name\":\"Internal\"},"
            "{\"index\":2,\"name\":\"Preset A\"},"
            "{\"index\":3,\"name\":\"Preset B\"}]");
    }
    /* Card expansion list for Performance mode - only actual expansions, no factory */
    if (strcmp(key, "card_expansion_list") == 0) {
        if (inst->expansion_count == 0) {
            return snprintf(buf, buf_len, "[{\"index\":-1,\"name\":\"No expansions found\"}]");
        }
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < inst->expansion_count && written < buf_len - 100; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"index\":%d,\"name\":\"%s\",\"patch_count\":%d}",
                i, inst->expansions[i].name, inst->expansions[i].patch_count);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* User patch list for "Load User Patch" menu - returns JSON array of saved patches */
    if (strcmp(key, "user_patch_list") == 0 && inst->mcu) {
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < NUM_USER_PATCHES && written < buf_len - 100; i++) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (i * PATCH_SIZE);
            /* Check if slot has valid data (not 0xFF filled) */
            if (inst->mcu->nvram[offset] != 0xFF) {
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                /* Trim trailing spaces */
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
                if (written > 1) written += snprintf(buf + written, buf_len - written, ",");
                written += snprintf(buf + written, buf_len - written,
                    "{\"index\":%d,\"name\":\"%s\"}", i, name);
            }
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Save patch slot list - shows all 64 slots for saving */
    if (strcmp(key, "save_patch_slot_list") == 0) {
        int written = snprintf(buf, buf_len, "[");
        for (int i = 0; i < 64; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");

            const char *slot_name = "(empty)";
            char name_buf[16];

            if (inst->mcu) {
                uint32_t offset = NVRAM_PATCH_INTERNAL + (i * PATCH_SIZE);
                if (inst->mcu->nvram[offset] != 0xFF) {
                    /* Copy and trim patch name */
                    memcpy(name_buf, &inst->mcu->nvram[offset], 12);
                    name_buf[12] = '\0';
                    for (int j = 11; j >= 0 && name_buf[j] == ' '; j--) {
                        name_buf[j] = '\0';
                    }
                    slot_name = name_buf;
                }
            }

            written += snprintf(buf + written, buf_len - written,
                "{\"index\":%d,\"name\":\"%02d: %s\"}", i, i + 1, slot_name);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Load patch slot list - shows only non-empty slots for loading */
    if (strcmp(key, "load_patch_slot_list") == 0) {
        if (!inst->mcu) {
            return snprintf(buf, buf_len, "[{\"index\":-1,\"name\":\"Loading...\"}]");
        }
        int written = snprintf(buf, buf_len, "[");
        int first = 1;
        for (int i = 0; i < NUM_USER_PATCHES && written < buf_len - 100; i++) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (i * PATCH_SIZE);
            /* Only show slots with valid data */
            if (inst->mcu->nvram[offset] != 0xFF) {
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
                if (!first) written += snprintf(buf + written, buf_len - written, ",");
                first = 0;
                written += snprintf(buf + written, buf_len - written,
                    "{\"index\":%d,\"name\":\"%02d: %s\"}", i, i + 1, name);
            }
        }
        /* If no patches found, show message */
        if (first) {
            written += snprintf(buf + written, buf_len - written,
                "{\"index\":-1,\"name\":\"No saved patches\"}");
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }
    /* Save slot current index */
    if (strcmp(key, "save_slot") == 0) {
        return snprintf(buf, buf_len, "%d", inst->save_slot_index);
    }
    /* Save slot count for browser */
    if (strcmp(key, "save_slot_count") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_USER_PATCHES);
    }
    /* Save slot name for browser */
    if (strcmp(key, "save_slot_name") == 0 && inst->mcu) {
        int idx = inst->save_slot_index;
        if (idx >= 0 && idx < NUM_USER_PATCHES) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (idx * PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            if (inst->mcu->nvram[offset] != 0xFF) {
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
            } else {
                strcpy(name, "(empty)");
            }
            return snprintf(buf, buf_len, "%02d: %s", idx + 1, name);
        }
        return snprintf(buf, buf_len, "01: (empty)");
    }
    /* Load slot current index */
    if (strcmp(key, "load_slot") == 0) {
        return snprintf(buf, buf_len, "%d", inst->load_slot_index);
    }
    /* Load slot count for browser */
    if (strcmp(key, "load_slot_count") == 0) {
        return snprintf(buf, buf_len, "%d", NUM_USER_PATCHES);
    }
    /* Load slot name for browser */
    if (strcmp(key, "load_slot_name") == 0 && inst->mcu) {
        int idx = inst->load_slot_index;
        if (idx >= 0 && idx < NUM_USER_PATCHES) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (idx * PATCH_SIZE);
            char name[PATCH_NAME_LEN + 1];
            if (inst->mcu->nvram[offset] != 0xFF) {
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                int len = PATCH_NAME_LEN;
                while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == 0)) len--;
                name[len] = '\0';
            } else {
                strcpy(name, "(empty)");
            }
            return snprintf(buf, buf_len, "%02d: %s", idx + 1, name);
        }
        return snprintf(buf, buf_len, "01: (empty)");
    }
    /* User patch info: user_patch_<idx>_name */
    if (strncmp(key, "user_patch_", 11) == 0 && inst->mcu) {
        int idx = atoi(key + 11);
        if (strstr(key, "_name") && idx >= 0 && idx < NUM_USER_PATCHES) {
            uint32_t offset = NVRAM_PATCH_INTERNAL + (idx * PATCH_SIZE);
            if (inst->mcu->nvram[offset] != 0xFF) {
                char name[PATCH_NAME_LEN + 1];
                memcpy(name, &inst->mcu->nvram[offset], PATCH_NAME_LEN);
                name[PATCH_NAME_LEN] = '\0';
                return snprintf(buf, buf_len, "%s", name);
            }
            return snprintf(buf, buf_len, "(empty)");
        }
    }
    /* Individual expansion info: expansion_<idx>_name, expansion_<idx>_patch_count, expansion_<idx>_first_patch */
    if (strncmp(key, "expansion_", 10) == 0) {
        int idx = atoi(key + 10);
        if (idx >= 0 && idx < inst->expansion_count) {
            if (strstr(key, "_name")) {
                return snprintf(buf, buf_len, "%s", inst->expansions[idx].name);
            }
            if (strstr(key, "_patch_count")) {
                return snprintf(buf, buf_len, "%d", inst->expansions[idx].patch_count);
            }
            if (strstr(key, "_first_patch")) {
                return snprintf(buf, buf_len, "%d", inst->expansions[idx].first_global_index);
            }
        }
    }
    /* Bank queries: bank_<idx>_name, bank_<idx>_start, bank_<idx>_count */
    if (strncmp(key, "bank_", 5) == 0) {
        int idx = atoi(key + 5);
        if (strstr(key, "_name") && idx >= 0 && idx < inst->bank_count) {
            return snprintf(buf, buf_len, "%s", inst->bank_names[idx]);
        }
        if (strstr(key, "_start") && idx >= 0 && idx < inst->bank_count) {
            return snprintf(buf, buf_len, "%d", inst->bank_starts[idx]);
        }
        if (strstr(key, "_count") && idx >= 0 && idx < inst->bank_count) {
            int next_start = (idx + 1 < inst->bank_count) ? inst->bank_starts[idx + 1] : inst->total_patches;
            return snprintf(buf, buf_len, "%d", next_start - inst->bank_starts[idx]);
        }
    }
    /* Patch name queries: patch_<idx>_name */
    if (strncmp(key, "patch_", 6) == 0 && strstr(key, "_name")) {
        int idx = atoi(key + 6);
        if (idx >= 0 && idx < inst->total_patches) {
            return snprintf(buf, buf_len, "%s", inst->patches[idx].name);
        }
        return snprintf(buf, buf_len, "---");
    }

    /* UI hierarchy for shadow parameter editor
     * JV-880 has two modes: patch and performance
     * - Patch mode: browse patches → tones (1-4) → tone params
     * - Performance mode: browse performances → parts (1-8) → part params
     *
     * Tone params use: nvram_tone_<n>_<param> (n=0-3)
     * Patch common params use: nvram_patchCommon_<param>
     * Part params use: sram_part_<n>_<param> (n=0-7)
     */
    if (strcmp(key, "ui_hierarchy") == 0) {
        /* GROUND TRUTH: In a Signal Chain slot the platform's shadow_ui.js is the
         * live renderer (not this module's src/ui.js). That renderer keys every
         * param entry as "synth:<key>" via buildHierarchyParamKey(); it has NO
         * tone_section/tone_prefix support and DOES NOT carry a child_prefix
         * context into navigated sub-levels. So per-tone editing MUST use explicit
         * per-tone levels with fully-qualified nvram_tone_<n>_<param> keys (the
         * dx7 pattern: operators are written out explicitly, never via a
         * "selected operator" mechanism). We generate the 4 tones x 7 sections
         * via a snprintf loop. Phase A root levels (patch/patch_main/
         * patch_common) and the performance tree are unchanged. */

        /* Per-section param suffix lists (relative to a tone). Each entry is the
         * bare param suffix; the loop emits {"key":"nvram_tone_<n>_<suffix>",...}.
         * Labels come from chain_params (name field), so we only need keys here. */
        struct ToneSecParam { const char *suffix; const char *label; };
        struct ToneSection {
            const char *id;       /* sublevel id suffix, e.g. "wave" -> tone1_wave */
            const char *label;    /* menu label */
            const struct ToneSecParam *params;
            int param_count;
        };

        static const struct ToneSecParam wave_p[] = {
            {"toneswitch","Tone Switch"},{"wavegroup","Wave Group"},
            {"wavenumber","Wave Number"},{"fxmswitch","FXM"},{"fxmdepth","FXM Depth"},
        };
        /* NOTE: the renderer filters a level's knobs to keys ALSO present in
         * its params (shadow_ui.js applyHierarchyVisibilityFilters), so each
         * section's knob row = its FIRST 8 params. Order lists accordingly:
         * most-tweaked first. */
        static const struct ToneSecParam pitch_p[] = {
            {"pitchcoarse","Pitch Coarse"},{"pitchfine","Pitch Fine"},
            {"penvdepth","P.Env Depth"},
            {"penvtime1","P.Env T1"},{"penvtime2","P.Env T2"},
            {"penvtime4","P.Env T4"},
            {"pitchkeyfollow","Pitch KF"},{"randompitchdepth","Random Pitch"},
            {"penvtime3","P.Env T3"},
            {"penvlevel1","P.Env L1"},{"penvlevel2","P.Env L2"},
            {"penvlevel3","P.Env L3"},{"penvlevel4","P.Env L4"},
        };
        static const struct ToneSecParam filter_p[] = {
            {"cutofffrequency","Cutoff"},{"resonance","Resonance"},
            {"tvfenvdepth","F.Env Depth"},
            {"tvfenvtime1","F.Env T1"},{"tvfenvtime2","F.Env T2"},
            {"tvfenvlevel3","F.Env L3"},{"tvfenvtime4","F.Env T4"},
            {"filtermode","Filter Mode"},
            {"cutoffkeyfollow","Cutoff KF"},{"resonancemode","Reso Mode"},
            {"tvfenvvelocitylevelsense","F.Env Velo"},
            {"tvfenvtime3","F.Env T3"},
            {"tvfenvlevel1","F.Env L1"},{"tvfenvlevel2","F.Env L2"},
            {"tvfenvlevel4","F.Env L4"},
        };
        static const struct ToneSecParam amp_p[] = {
            {"level","Level"},{"pan","Pan"},
            {"tvaenvtime1","A.Env T1"},{"tvaenvtime2","A.Env T2"},
            {"tvaenvlevel3","A.Env L3"},{"tvaenvtime4","A.Env T4"},
            {"tvaenvvelocitylevelsense","A.Env Velo"},{"tvaenvvelocitycurve","A.Env Curve"},
            {"levelkeyfollow","Level KF"},{"panningkeyfollow","Pan KF"},
            {"tvaenvtime3","A.Env T3"},
            {"tvaenvlevel1","A.Env L1"},{"tvaenvlevel2","A.Env L2"},
            {"tonedelaymode","Delay Mode"},{"tonedelaytime","Delay Time"},
        };
        static const struct ToneSecParam lfo1_p[] = {
            {"lfo1form","LFO1 Wave"},{"lfo1rate","LFO1 Rate"},
            {"lfo1delay","LFO1 Delay"},{"lfo1fadetime","LFO1 Fade"},
            {"lfo1pitchdepth","LFO1 Pitch"},{"lfo1tvfdepth","LFO1 Filter"},
            {"lfo1tvadepth","LFO1 Amp"},{"lfo1offset","LFO1 Offset"},
            {"lfo1synchro","LFO1 Sync"},
        };
        static const struct ToneSecParam lfo2_p[] = {
            {"lfo2form","LFO2 Wave"},{"lfo2rate","LFO2 Rate"},
            {"lfo2delay","LFO2 Delay"},{"lfo2fadetime","LFO2 Fade"},
            {"lfo2pitchdepth","LFO2 Pitch"},{"lfo2tvfdepth","LFO2 Filter"},
            {"lfo2tvadepth","LFO2 Amp"},
        };
        static const struct ToneSecParam fx_p[] = {
            {"drylevel","Dry Level"},{"reverbsendlevel","Reverb Send"},
            {"chorussendlevel","Chorus Send"},
        };
        /* Control matrix: one page per source, 8 params each (Dest+Sens × 4
         * slots interleaved so each Dest sits next to its Sens). */
        static const struct ToneSecParam mod_p[] = {
            {"moddesta","Mod Dest A"},{"modsensa","Mod Sens A"},
            {"moddestb","Mod Dest B"},{"modsensb","Mod Sens B"},
            {"moddestc","Mod Dest C"},{"modsensc","Mod Sens C"},
            {"moddestd","Mod Dest D"},{"modsensd","Mod Sens D"},
        };
        static const struct ToneSecParam aft_p[] = {
            {"aftdesta","Aft Dest A"},{"aftsensa","Aft Sens A"},
            {"aftdestb","Aft Dest B"},{"aftsensb","Aft Sens B"},
            {"aftdestc","Aft Dest C"},{"aftsensc","Aft Sens C"},
            {"aftdestd","Aft Dest D"},{"aftsensd","Aft Sens D"},
        };
        static const struct ToneSecParam exp_p[] = {
            {"expdesta","Exp Dest A"},{"expsensa","Exp Sens A"},
            {"expdestb","Exp Dest B"},{"expsensb","Exp Sens B"},
            {"expdestc","Exp Dest C"},{"expsensc","Exp Sens C"},
            {"expdestd","Exp Dest D"},{"expsensd","Exp Sens D"},
        };

        #define TSEC(id,label,arr) { id, label, arr, (int)(sizeof(arr)/sizeof((arr)[0])) }
        static const struct ToneSection sections[] = {
            TSEC("wave","Wave",wave_p),
            TSEC("pitch","Pitch",pitch_p),
            TSEC("filter","Filter",filter_p),
            TSEC("amp","Amp",amp_p),
            TSEC("lfo1","LFO 1",lfo1_p),
            TSEC("lfo2","LFO 2",lfo2_p),
            TSEC("fx","FX Sends",fx_p),
            TSEC("mod","Mod Matrix",mod_p),
            TSEC("aft","Aftertouch",aft_p),
            TSEC("exp","Expression",exp_p),
        };
        #undef TSEC
        const int section_count = (int)(sizeof(sections)/sizeof(sections[0]));

        /* The per-tone knob row stays live on every tone sublevel. Knob keys are
         * fully qualified per tone so the encoders edit the displayed tone. */
        static const char *knob_suffix[8] = {
            "cutofffrequency","resonance","tvaenvtime1","tvaenvtime2",
            "tvaenvlevel3","tvaenvtime4","level","pan"
        };
        static const char *knob_labels =
            "\"Cut\",\"Res\",\"Atk\",\"Dcy\",\"Sus\",\"Rel\",\"Lvl\",\"Pan\"";

        int w = 0;
        #define APPEND(...) do { \
            int _n = snprintf(buf + w, buf_len - w, __VA_ARGS__); \
            if (_n < 0 || _n >= buf_len - w) return -1; \
            w += _n; \
        } while (0)

        /* ---- static prefix: header + Phase A patch levels + tone_selector ---- */
        APPEND("%s",
            "{"
            "\"modes\":[\"patch\",\"performance\"],"
            "\"mode_param\":\"mode\","
            "\"levels\":{"
                "\"patch\":{"
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"patch_main\","
                    "\"knobs\":[\"macro_cutoff\",\"macro_resonance\",\"macro_attack\",\"macro_decay\",\"macro_sustain\",\"macro_release\",\"macro_tvf_env_depth\",\"macro_lfo_depth\"],"
                    "\"knob_labels\":[\"Cut\",\"Res\",\"Atk\",\"Dcy\",\"Sus\",\"Rel\",\"Env\",\"LFO\"],"
                    "\"params\":[]"
                "},"
                "\"patch_main\":{"
                    "\"label\":\"Patch\","
                    "\"children\":null,"
                    "\"knobs\":[\"macro_cutoff\",\"macro_resonance\",\"macro_attack\",\"macro_decay\",\"macro_sustain\",\"macro_release\",\"macro_tvf_env_depth\",\"macro_lfo_depth\"],"
                    "\"knob_labels\":[\"Cut\",\"Res\",\"Atk\",\"Dcy\",\"Sus\",\"Rel\",\"Env\",\"LFO\"],"
                    "\"params\":["
                        "{\"key\":\"macro_cutoff\",\"label\":\"Cutoff\"},"
                        "{\"key\":\"macro_resonance\",\"label\":\"Resonance\"},"
                        "{\"key\":\"macro_attack\",\"label\":\"Attack\"},"
                        "{\"key\":\"macro_decay\",\"label\":\"Decay\"},"
                        "{\"key\":\"macro_sustain\",\"label\":\"Sustain\"},"
                        "{\"key\":\"macro_release\",\"label\":\"Release\"},"
                        "{\"key\":\"macro_tvf_env_depth\",\"label\":\"Filter Env\"},"
                        "{\"key\":\"macro_lfo_depth\",\"label\":\"LFO Depth\"},"
                        "{\"level\":\"patch_common\",\"label\":\"Common / Control\"},"
                        "{\"level\":\"effects\",\"label\":\"Effects\"},"
                        "{\"level\":\"tone_selector\",\"label\":\"Edit Tones\"},"
                        "{\"level\":\"save_slot\",\"label\":\"Save to Slot\"},"
                        "{\"level\":\"expansions\",\"label\":\"Jump to Expansion\"}"
                    "]"
                "},"
                "\"patch_common\":{"
                    "\"label\":\"Common / Control\","
                    "\"children\":null,"
                    /* knobs must be keys present in this level's params
                     * (renderer filters otherwise) */
                    "\"knobs\":[\"nvram_patchCommon_patchlevel\",\"nvram_patchCommon_patchpan\",\"nvram_patchCommon_analogfeel\",\"nvram_patchCommon_bendrangeup\",\"nvram_patchCommon_bendrangedown\",\"nvram_patchCommon_portamentoswitch\",\"nvram_patchCommon_portamentotime\",\"nvram_patchCommon_keyassign\"],"
                    "\"params\":["
                        "{\"key\":\"nvram_patchCommon_patchlevel\",\"label\":\"Patch Level\"},"
                        "{\"key\":\"nvram_patchCommon_patchpan\",\"label\":\"Patch Pan\"},"
                        "{\"key\":\"nvram_patchCommon_analogfeel\",\"label\":\"Analog Feel\"},"
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"},"
                        "{\"key\":\"nvram_patchCommon_bendrangeup\",\"label\":\"Bend Up\"},"
                        "{\"key\":\"nvram_patchCommon_bendrangedown\",\"label\":\"Bend Down\"},"
                        "{\"key\":\"nvram_patchCommon_portamentoswitch\",\"label\":\"Portamento\"},"
                        "{\"key\":\"nvram_patchCommon_portamentomode\",\"label\":\"Porta Mode\"},"
                        "{\"key\":\"nvram_patchCommon_portamentotype\",\"label\":\"Porta Type\"},"
                        "{\"key\":\"nvram_patchCommon_portamentotime\",\"label\":\"Porta Time\"},"
                        "{\"key\":\"nvram_patchCommon_keyassign\",\"label\":\"Key Assign\"},"
                        "{\"key\":\"nvram_patchCommon_sololegato\",\"label\":\"Solo Legato\"},"
                        "{\"key\":\"nvram_patchCommon_velocityswitch\",\"label\":\"Velocity Sw\"}"
                    "]"
                "},"
                "\"effects\":{"
                    "\"label\":\"Effects\","
                    "\"children\":null,"
                    "\"knobs\":[\"nvram_patchCommon_reverbtype\",\"nvram_patchCommon_reverblevel\",\"nvram_patchCommon_reverbtime\",\"nvram_patchCommon_reverbfeedback\",\"nvram_patchCommon_chorustype\",\"nvram_patchCommon_choruslevel\",\"nvram_patchCommon_chorusdepth\",\"nvram_patchCommon_chorusrate\"],"
                    "\"knob_labels\":[\"RvTy\",\"RvLv\",\"RvTm\",\"RvFb\",\"ChTy\",\"ChLv\",\"ChDp\",\"ChRt\"],"
                    "\"params\":["
                        "{\"key\":\"nvram_patchCommon_reverbtype\",\"label\":\"Reverb Type\"},"
                        "{\"key\":\"nvram_patchCommon_reverblevel\",\"label\":\"Reverb Level\"},"
                        "{\"key\":\"nvram_patchCommon_reverbtime\",\"label\":\"Reverb Time\"},"
                        "{\"key\":\"nvram_patchCommon_reverbfeedback\",\"label\":\"Reverb Feedback\"},"
                        "{\"key\":\"nvram_patchCommon_chorustype\",\"label\":\"Chorus Type\"},"
                        "{\"key\":\"nvram_patchCommon_choruslevel\",\"label\":\"Chorus Level\"},"
                        "{\"key\":\"nvram_patchCommon_chorusdepth\",\"label\":\"Chorus Depth\"},"
                        "{\"key\":\"nvram_patchCommon_chorusrate\",\"label\":\"Chorus Rate\"},"
                        "{\"key\":\"nvram_patchCommon_chorusfeedback\",\"label\":\"Chorus Feedback\"},"
                        "{\"key\":\"nvram_patchCommon_chorusoutput\",\"label\":\"Chorus Output\"}"
                    "]"
                "},"
                /* Tone selector: plain menu that navigates to one explicit
                 * per-tone level (tone1..tone4). No child_prefix here - the
                 * renderer does not carry that context into navigated levels. */
                "\"tone_selector\":{"
                    "\"label\":\"Tones\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":["
                        "{\"level\":\"tone1\",\"label\":\"Tone 1\"},"
                        "{\"level\":\"tone2\",\"label\":\"Tone 2\"},"
                        "{\"level\":\"tone3\",\"label\":\"Tone 3\"},"
                        "{\"level\":\"tone4\",\"label\":\"Tone 4\"},"
                        "{\"key\":\"link_tones\",\"label\":\"Link all to 1\"}"
                    "]"
                "},");

        /* ---- per-tone levels (explicit, dx7-style) ---- */
        for (int t = 0; t < 4; t++) {
            /* Build the fully-qualified knob array for this tone (reused on the
             * tone menu and every section sublevel). */
            char knobs_json[512];
            int kw = 0;
            kw += snprintf(knobs_json + kw, sizeof(knobs_json) - kw, "[");
            for (int k = 0; k < 8; k++) {
                kw += snprintf(knobs_json + kw, sizeof(knobs_json) - kw,
                    "%s\"nvram_tone_%d_%s\"", k ? "," : "", t, knob_suffix[k]);
            }
            kw += snprintf(knobs_json + kw, sizeof(knobs_json) - kw, "]");

            /* tone<N> menu: nav-only (no editable params). The renderer keeps a
             * level's FULL knob row only when the level has no editable params
             * visible; with any editable param, knobs are filtered to keys
             * present in params. Tone Switch lives in the Wave section. */
            APPEND("\"tone%d\":{"
                    "\"label\":\"Tone %d\","
                    "\"children\":null,"
                    "\"knobs\":%s,"
                    "\"knob_labels\":[%s],"
                    "\"params\":[",
                    t + 1, t + 1, knobs_json, knob_labels);
            for (int s = 0; s < section_count; s++) {
                APPEND("{\"level\":\"tone%d_%s\",\"label\":\"%s\"}%s",
                    t + 1, sections[s].id, sections[s].label,
                    (s == section_count - 1) ? "" : ",");
            }
            APPEND("%s", "]},");

            /* tone<N>_<section> leaf levels with fully-qualified keys.
             * Section knobs = the section's first up-to-8 params (renderer
             * filters knobs to keys present in this level's params). */
            for (int s = 0; s < section_count; s++) {
                const struct ToneSection *sec = &sections[s];
                APPEND("\"tone%d_%s\":{"
                        "\"label\":\"%s\","
                        "\"children\":null,"
                        "\"knobs\":[",
                    t + 1, sec->id, sec->label);
                int nk = sec->param_count < 8 ? sec->param_count : 8;
                for (int k = 0; k < nk; k++) {
                    APPEND("%s\"nvram_tone_%d_%s\"", k ? "," : "", t,
                        sec->params[k].suffix);
                }
                APPEND("%s", "],\"params\":[");
                for (int p = 0; p < sec->param_count; p++) {
                    APPEND("{\"key\":\"nvram_tone_%d_%s\",\"label\":\"%s\"}%s",
                        t, sec->params[p].suffix, sec->params[p].label,
                        (p == sec->param_count - 1) ? "" : ",");
                }
                APPEND("%s", "]},");
            }
        }

        /* ---- static suffix: save_slot + performance tree ---- */
        APPEND("%s",
                /* Save to user slot: items_param lists all 64 slots with their
                 * stored names; selecting one fires do_save_to_slot then returns
                 * to the patch edit menu via navigate_to. */
                "\"save_slot\":{"
                    "\"label\":\"Save to Slot\","
                    "\"items_param\":\"save_patch_slot_list\","
                    "\"select_param\":\"do_save_to_slot\","
                    "\"navigate_to\":\"patch\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "},"
                "\"performance\":{"
                    "\"list_param\":\"performance\","
                    "\"count_param\":\"num_performances\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"perf_main\","
                    "\"knobs\":[\"octave_transpose\"],"
                    "\"params\":[]"
                "},"
                "\"perf_main\":{"
                    "\"label\":\"Performance\","
                    "\"children\":null,"
                    "\"knobs\":[\"octave_transpose\"],"
                    "\"params\":["
                        "{\"level\":\"part_selector\",\"label\":\"Edit Parts\"},"
                        "{\"level\":\"load_expansion\",\"label\":\"Load Expansion\"},"
                        "{\"key\":\"octave_transpose\",\"label\":\"Octave\"}"
                    "]"
                "},"
                "\"load_expansion\":{"
                    "\"label\":\"Load Expansion\","
                    "\"items_param\":\"expansion_list\","
                    "\"select_param\":\"load_expansion\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "},"
                "\"part_selector\":{"
                    "\"label\":\"Parts\","
                    "\"children\":null,"
                    "\"child_prefix\":\"sram_part_\","
                    "\"child_count\":8,"
                    "\"child_label\":\"Part\","
                    "\"knobs\":[\"partlevel\",\"partpan\",\"reverbswitch\",\"chorusswitch\",\"partcoarsetune\",\"partfinetune\",\"internalkeyrangelower\",\"internalkeyrangeupper\"],"
                    "\"knob_labels\":[\"Lvl\",\"Pan\",\"Rev\",\"Cho\",\"CTn\",\"FTn\",\"KLo\",\"KHi\"],"
                    "\"params\":[\"patchbank\",\"patchnumber\",\"partlevel\",\"partpan\",\"reverbswitch\",\"chorusswitch\",\"partcoarsetune\",\"partfinetune\",\"internalkeyrangelower\",\"internalkeyrangeupper\",\"internalkeytranspose\"]"
                "},"
                "\"expansions\":{"
                    "\"label\":\"Jump to Expansion\","
                    "\"items_param\":\"expansion_list\","
                    "\"select_param\":\"jump_to_expansion\","
                    "\"navigate_to\":\"patch\","
                    "\"children\":null,"
                    "\"knobs\":[],"
                    "\"params\":[]"
                "}"
            "}"
        "}");

        #undef APPEND
        return w;
    }

    /* Chain params metadata for shadow parameter editor.
     *
     * The platform renderer (shadow_ui.js getParamMetadata) matches a param
     * entry's key EXACTLY against a chain_params key. Per-tone hierarchy entries
     * use fully-qualified keys (nvram_tone_<n>_<suffix>), so chain_params must
     * carry one metadata entry per (tone, suffix). We emit the static head/tail
     * verbatim and generate the tone block with a snprintf loop over 4 tones.
     * Part params stay bare-keyed: part_selector is a child_prefix level whose
     * inline params resolve the sram_part_<n>_ prefix at set/get time, while
     * metadata lookup uses the bare suffix. */
    if (strcmp(key, "chain_params") == 0) {
        /* Per-tone metadata table: {suffix, tail} where tail is the JSON object
         * body after the "key" field. Keys are emitted fully qualified below. */
        struct ToneMeta { const char *suffix; const char *tail; };
        static const struct ToneMeta tone_meta[] = {
            /* Wave */
            {"toneswitch",                 "\"name\":\"Tone Switch\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]"},
            {"wavegroup",                  "\"name\":\"Wave Group\",\"type\":\"enum\",\"min\":0,\"max\":3,\"options\":[\"INT-A\",\"INT-B\",\"EXP-A\",\"EXP-B\"]"},
            {"wavenumber",                 "\"name\":\"Wave Number\",\"type\":\"int\",\"min\":0,\"max\":255"},
            {"fxmswitch",                  "\"name\":\"FXM\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]"},
            {"fxmdepth",                   "\"name\":\"FXM Depth\",\"type\":\"int\",\"min\":1,\"max\":16"},
            /* Level/Pan */
            {"level",                      "\"name\":\"Level\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"pan",                        "\"name\":\"Pan\",\"type\":\"int\",\"min\":-64,\"max\":63,\"display\":\"pan\""},
            {"levelkeyfollow",             "\"name\":\"Level KF\",\"type\":\"int\",\"min\":0,\"max\":15"},
            {"panningkeyfollow",           "\"name\":\"Pan KF\",\"type\":\"int\",\"min\":0,\"max\":15"},
            /* Filter */
            {"cutofffrequency",            "\"name\":\"Cutoff\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"resonance",                  "\"name\":\"Resonance\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"filtermode",                 "\"name\":\"Filter Mode\",\"type\":\"enum\",\"options\":[\"Off\",\"LPF\",\"HPF\"]"},
            {"resonancemode",              "\"name\":\"Reso Mode\",\"type\":\"enum\",\"options\":[\"Soft\",\"Hard\"]"},
            {"cutoffkeyfollow",            "\"name\":\"Cutoff KF\",\"type\":\"int\",\"min\":0,\"max\":127"},
            /* Pitch */
            {"pitchcoarse",                "\"name\":\"Pitch Coarse\",\"type\":\"int\",\"min\":-48,\"max\":48,\"display\":\"signed\""},
            {"pitchfine",                  "\"name\":\"Pitch Fine\",\"type\":\"int\",\"min\":-50,\"max\":50,\"display\":\"signed\""},
            {"randompitchdepth",           "\"name\":\"Random Pitch\",\"type\":\"int\",\"min\":0,\"max\":7"},
            {"pitchkeyfollow",             "\"name\":\"Pitch KF\",\"type\":\"int\",\"min\":0,\"max\":15"},
            /* LFO1 */
            {"lfo1form",                   "\"name\":\"LFO1 Wave\",\"type\":\"enum\",\"min\":0,\"max\":5,\"options\":[\"TRI\",\"SIN\",\"SAW\",\"SQU\",\"RND1\",\"RND2\"]"},
            {"lfo1rate",                   "\"name\":\"LFO1 Rate\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"lfo1delay",                  "\"name\":\"LFO1 Delay\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"lfo1fadetime",               "\"name\":\"LFO1 Fade\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"lfo1offset",                 "\"name\":\"LFO1 Offset\",\"type\":\"enum\",\"min\":0,\"max\":4,\"options\":[\"-100\",\"-50\",\"0\",\"+50\",\"+100\"]"},
            {"lfo1synchro",                "\"name\":\"LFO1 Sync\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]"},
            {"lfo1pitchdepth",             "\"name\":\"LFO1 Pitch\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"lfo1tvfdepth",               "\"name\":\"LFO1 Filter\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"lfo1tvadepth",               "\"name\":\"LFO1 Amp\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            /* LFO2 */
            {"lfo2form",                   "\"name\":\"LFO2 Wave\",\"type\":\"enum\",\"min\":0,\"max\":5,\"options\":[\"TRI\",\"SIN\",\"SAW\",\"SQU\",\"RND1\",\"RND2\"]"},
            {"lfo2rate",                   "\"name\":\"LFO2 Rate\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"lfo2delay",                  "\"name\":\"LFO2 Delay\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"lfo2fadetime",               "\"name\":\"LFO2 Fade\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"lfo2pitchdepth",             "\"name\":\"LFO2 Pitch\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"lfo2tvfdepth",               "\"name\":\"LFO2 Filter\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"lfo2tvadepth",               "\"name\":\"LFO2 Amp\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            /* Pitch Envelope */
            {"penvdepth",                  "\"name\":\"P.Env Depth\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"penvtime1",                  "\"name\":\"P.Env T1\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"penvlevel1",                 "\"name\":\"P.Env L1\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"penvtime2",                  "\"name\":\"P.Env T2\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"penvlevel2",                 "\"name\":\"P.Env L2\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"penvtime3",                  "\"name\":\"P.Env T3\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"penvlevel3",                 "\"name\":\"P.Env L3\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"penvtime4",                  "\"name\":\"P.Env T4\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"penvlevel4",                 "\"name\":\"P.Env L4\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            /* Filter Envelope */
            {"tvfenvdepth",                "\"name\":\"F.Env Depth\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"tvfenvvelocitylevelsense",   "\"name\":\"F.Env Velo\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"tvfenvtime1",                "\"name\":\"F.Env T1\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvlevel1",               "\"name\":\"F.Env L1\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvtime2",                "\"name\":\"F.Env T2\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvlevel2",               "\"name\":\"F.Env L2\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvtime3",                "\"name\":\"F.Env T3\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvlevel3",               "\"name\":\"F.Env L3\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvtime4",                "\"name\":\"F.Env T4\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvfenvlevel4",               "\"name\":\"F.Env L4\",\"type\":\"int\",\"min\":0,\"max\":127"},
            /* Amp Envelope */
            {"tvaenvtime1",                "\"name\":\"A.Env T1\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvlevel1",               "\"name\":\"A.Env L1\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvtime2",                "\"name\":\"A.Env T2\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvlevel2",               "\"name\":\"A.Env L2\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvtime3",                "\"name\":\"A.Env T3\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvlevel3",               "\"name\":\"A.Env L3\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvtime4",                "\"name\":\"A.Env T4\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"tvaenvvelocitylevelsense",   "\"name\":\"A.Env Velo\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""},
            {"tvaenvvelocitycurve",        "\"name\":\"A.Env Curve\",\"type\":\"int\",\"min\":0,\"max\":6"},
            /* Velocity/Delay */
            {"velocityrangelower",         "\"name\":\"Vel Lo\",\"type\":\"int\",\"min\":1,\"max\":127"},
            {"velocityrangeupper",         "\"name\":\"Vel Hi\",\"type\":\"int\",\"min\":1,\"max\":127"},
            {"tonedelaymode",              "\"name\":\"Delay Mode\",\"type\":\"enum\",\"min\":0,\"max\":3,\"options\":[\"Normal\",\"Hold\",\"Play-Mate\",\"Clock\"]"},
            {"tonedelaytime",              "\"name\":\"Delay Time\",\"type\":\"int\",\"min\":0,\"max\":127"},
            /* Output */
            {"drylevel",                   "\"name\":\"Dry Level\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"reverbsendlevel",            "\"name\":\"Reverb Send\",\"type\":\"int\",\"min\":0,\"max\":127"},
            {"chorussendlevel",            "\"name\":\"Chorus Send\",\"type\":\"int\",\"min\":0,\"max\":127"},
            /* Control matrix: 3 sources x (4 dest enums + 4 signed sens). Dest
             * enum order matches the firmware's destination indices 0..12. */
            #define MXDEST(nm) "\"name\":\"" nm "\",\"type\":\"enum\",\"options\":[\"Off\",\"Pitch\",\"Cutoff\",\"Reso\",\"Level\",\"L1>Pit\",\"L2>Pit\",\"L1>TVF\",\"L2>TVF\",\"L1>TVA\",\"L2>TVA\",\"L1 Rate\",\"L2 Rate\"]"
            #define MXSENS(nm) "\"name\":\"" nm "\",\"type\":\"int\",\"min\":-63,\"max\":63,\"display\":\"signed\""
            {"moddesta", MXDEST("Mod Dest A")}, {"modsensa", MXSENS("Mod Sens A")},
            {"moddestb", MXDEST("Mod Dest B")}, {"modsensb", MXSENS("Mod Sens B")},
            {"moddestc", MXDEST("Mod Dest C")}, {"modsensc", MXSENS("Mod Sens C")},
            {"moddestd", MXDEST("Mod Dest D")}, {"modsensd", MXSENS("Mod Sens D")},
            {"aftdesta", MXDEST("Aft Dest A")}, {"aftsensa", MXSENS("Aft Sens A")},
            {"aftdestb", MXDEST("Aft Dest B")}, {"aftsensb", MXSENS("Aft Sens B")},
            {"aftdestc", MXDEST("Aft Dest C")}, {"aftsensc", MXSENS("Aft Sens C")},
            {"aftdestd", MXDEST("Aft Dest D")}, {"aftsensd", MXSENS("Aft Sens D")},
            {"expdesta", MXDEST("Exp Dest A")}, {"expsensa", MXSENS("Exp Sens A")},
            {"expdestb", MXDEST("Exp Dest B")}, {"expsensb", MXSENS("Exp Sens B")},
            {"expdestc", MXDEST("Exp Dest C")}, {"expsensc", MXSENS("Exp Sens C")},
            {"expdestd", MXDEST("Exp Dest D")}, {"expsensd", MXSENS("Exp Sens D")},
            #undef MXDEST
            #undef MXSENS
        };
        const int tone_meta_count = (int)(sizeof(tone_meta)/sizeof(tone_meta[0]));

        int w = 0;
        #define APPEND(...) do { \
            int _n = snprintf(buf + w, buf_len - w, __VA_ARGS__); \
            if (_n < 0 || _n >= buf_len - w) return -1; \
            w += _n; \
        } while (0)

        /* ---- static head: navigation + macros + patch common ---- */
        APPEND("%s",
            "["
            /* Basic navigation */
            "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999},"
            "{\"key\":\"performance\",\"name\":\"Performance\",\"type\":\"int\",\"min\":0,\"max\":47},"
            "{\"key\":\"part\",\"name\":\"Part\",\"type\":\"int\",\"min\":0,\"max\":7},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3},"
            "{\"key\":\"link_tones\",\"name\":\"Link all to 1\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            /* Macro controls (absolute-anchored: display = anchor tone value,
             * ranges match backing tone params; tvf_env_depth is signed). */
            "{\"key\":\"macro_cutoff\",\"name\":\"Cutoff\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"macro_resonance\",\"name\":\"Resonance\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"macro_attack\",\"name\":\"Attack\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"macro_decay\",\"name\":\"Decay\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"macro_release\",\"name\":\"Release\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"macro_sustain\",\"name\":\"Sustain\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"macro_tvf_env_depth\",\"name\":\"Filter Env\",\"type\":\"int\",\"min\":-63,\"max\":63,\"step\":1},"
            "{\"key\":\"macro_lfo_depth\",\"name\":\"LFO Depth\",\"type\":\"int\",\"min\":-63,\"max\":63,\"step\":1},"
            /* Patch common params */
            "{\"key\":\"nvram_patchCommon_patchlevel\",\"name\":\"Patch Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_patchpan\",\"name\":\"Patch Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_reverblevel\",\"name\":\"Reverb\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"nvram_patchCommon_reverbtime\",\"name\":\"Reverb Time\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_choruslevel\",\"name\":\"Chorus\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
            "{\"key\":\"nvram_patchCommon_chorusdepth\",\"name\":\"Chorus Depth\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_chorusrate\",\"name\":\"Chorus Rate\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_analogfeel\",\"name\":\"Analog Feel\",\"type\":\"int\",\"min\":0,\"max\":127},"
            /* Effects: reverb/chorus type + feedback + output */
            "{\"key\":\"nvram_patchCommon_reverbtype\",\"name\":\"Reverb Type\",\"type\":\"enum\",\"options\":[\"Room1\",\"Room2\",\"Stage1\",\"Stage2\",\"Hall1\",\"Hall2\",\"Delay\",\"Pan-Dly\"]},"
            "{\"key\":\"nvram_patchCommon_reverbfeedback\",\"name\":\"Reverb Feedback\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_chorustype\",\"name\":\"Chorus Type\",\"type\":\"enum\",\"options\":[\"Chorus1\",\"Chorus2\",\"Chorus3\"]},"
            "{\"key\":\"nvram_patchCommon_chorusfeedback\",\"name\":\"Chorus Feedback\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_chorusoutput\",\"name\":\"Chorus Output\",\"type\":\"enum\",\"options\":[\"Mix\",\"Reverb\"]},"
            /* Control / play */
            "{\"key\":\"nvram_patchCommon_bendrangeup\",\"name\":\"Bend Up\",\"type\":\"int\",\"min\":0,\"max\":12},"
            "{\"key\":\"nvram_patchCommon_bendrangedown\",\"name\":\"Bend Down\",\"type\":\"int\",\"min\":0,\"max\":48},"
            "{\"key\":\"nvram_patchCommon_portamentoswitch\",\"name\":\"Portamento\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"nvram_patchCommon_portamentomode\",\"name\":\"Porta Mode\",\"type\":\"enum\",\"options\":[\"Legato\",\"Normal\"]},"
            "{\"key\":\"nvram_patchCommon_portamentotype\",\"name\":\"Porta Type\",\"type\":\"enum\",\"options\":[\"Time\",\"Rate\"]},"
            "{\"key\":\"nvram_patchCommon_portamentotime\",\"name\":\"Porta Time\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"nvram_patchCommon_keyassign\",\"name\":\"Key Assign\",\"type\":\"enum\",\"options\":[\"Poly\",\"Solo\"]},"
            "{\"key\":\"nvram_patchCommon_sololegato\",\"name\":\"Solo Legato\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"nvram_patchCommon_velocityswitch\",\"name\":\"Velocity Sw\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},");

        /* ---- per-tone metadata (fully-qualified keys, 4 tones) ---- */
        for (int t = 0; t < 4; t++) {
            for (int m = 0; m < tone_meta_count; m++) {
                APPEND("{\"key\":\"nvram_tone_%d_%s\",%s},",
                    t, tone_meta[m].suffix, tone_meta[m].tail);
            }
        }

        /* ---- static tail: part params (bare keys; sram_part_<n>_ added by the
         * part_selector child_prefix at set/get time) ---- */
        APPEND("%s",
            /* Part params (suffix only - child_prefix adds sram_part_N_) */
            "{\"key\":\"patchbank\",\"name\":\"Bank\",\"type\":\"enum\",\"options\":[\"Internal\",\"Preset A\",\"Preset B\"]},"
            "{\"key\":\"partlevel\",\"name\":\"Part Level\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"partpan\",\"name\":\"Part Pan\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"patchnumber\",\"name\":\"Patch\",\"type\":\"int\",\"min\":0,\"max\":63},"
            "{\"key\":\"reverbswitch\",\"name\":\"Reverb\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"chorusswitch\",\"name\":\"Chorus\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
            "{\"key\":\"partcoarsetune\",\"name\":\"Coarse Tune\",\"type\":\"int\",\"min\":16,\"max\":112},"
            "{\"key\":\"partfinetune\",\"name\":\"Fine Tune\",\"type\":\"int\",\"min\":14,\"max\":114},"
            "{\"key\":\"internalkeyrangelower\",\"name\":\"Key Lo\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"internalkeyrangeupper\",\"name\":\"Key Hi\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"key\":\"internalkeytranspose\",\"name\":\"Transpose\",\"type\":\"int\",\"min\":16,\"max\":112}"
        "]");

        #undef APPEND
        return w;
    }

#if JV880_PERF_STATS
    if (strcmp(key, "perf_stats") == 0) {
        /* Return the last formatted perf report line (empty string until first report fires) */
        return snprintf(buf, buf_len, "%s", inst->perf_stats_buf);
    }
#endif /* JV880_PERF_STATS */

    return -1;
}

/* v2: Get error - returns error message if module is in error state */
static int v2_get_error(void *instance, char *buf, int buf_len) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst || !inst->load_error[0]) return 0;  /* No error */

    int len = strlen(inst->load_error);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, inst->load_error, len);
    buf[len] = '\0';
    return len;
}

/* v2: Render block */
static void v2_render_block(void *instance, int16_t *out, int frames) {
    jv880_instance_t *inst = (jv880_instance_t*)instance;
    if (!inst || !inst->initialized || !inst->thread_running || !inst->loading_complete) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    pthread_mutex_lock(&inst->ring_mutex);
    int avail = v2_ring_available(inst);
    int to_read = (avail < frames) ? avail : frames;

    /* Track buffer levels for diagnostics */
    inst->render_count++;
    if (avail < inst->min_buffer_level || inst->min_buffer_level == 0) {
        inst->min_buffer_level = avail;
    }

    for (int i = 0; i < to_read; i++) {
        out[i * 2 + 0] = inst->audio_ring[inst->ring_read * 2 + 0] >> OUTPUT_GAIN_SHIFT;
        out[i * 2 + 1] = inst->audio_ring[inst->ring_read * 2 + 1] >> OUTPUT_GAIN_SHIFT;
        inst->ring_read = (inst->ring_read + 1) % AUDIO_RING_SIZE;
    }
    pthread_mutex_unlock(&inst->ring_mutex);

    /* Pad with silence if underrun */
    if (to_read < frames) {
        inst->underrun_count++;
        jv_debug("[JV880] UNDERRUN #%d: needed %d, had %d (min_level=%d, renders=%d)\n",
                inst->underrun_count, frames, avail, inst->min_buffer_level, inst->render_count);
        inst->min_buffer_level = 9999;  /* Reset for next period */
    }
    for (int i = to_read; i < frames; i++) {
        out[i * 2 + 0] = 0;
        out[i * 2 + 1] = 0;
    }

    /* Handle deferred selections - only after warmup is complete */
    if (inst->warmup_remaining <= 0) {
        /* Handle deferred performance selection after mode switch has been processed */
        if (inst->pending_perf_select > 0) {
            inst->pending_perf_select--;
            if (inst->pending_perf_select == 0) {
                /* Mode switch has had time to process, now select performance */
                jv_debug("[v2_render_block] Executing deferred performance select: %d\n",
                        inst->current_performance);
                v2_select_performance(inst, inst->current_performance);
            }
        }

        /* Handle deferred patch selection after mode switch has been processed */
        if (inst->pending_patch_select > 0) {
            inst->pending_patch_select--;
            if (inst->pending_patch_select == 0) {
                /* Mode switch has had time to process, now select patch */
                jv_debug("[v2_render_block] Executing deferred patch select: %d\n",
                        inst->current_patch);
                v2_select_patch(inst, inst->current_patch);
            }
        }

        /* Handle deferred cross-expansion patch load (debounce) */
        if (inst->deferred_patch_countdown > 0) {
            inst->deferred_patch_countdown--;
            if (inst->deferred_patch_countdown == 0) {
                jv_debug("[v2_render_block] Executing deferred expansion patch: %d\n",
                        inst->deferred_patch_index);
                v2_select_patch(inst, inst->deferred_patch_index);
            }
        }
    }
}

/* v2 API struct */
static plugin_api_v2_t jv880_api_v2 = {
    MOVE_PLUGIN_API_VERSION_2,
    v2_create_instance,
    v2_destroy_instance,
    v2_on_midi,
    v2_set_param,
    v2_get_param,
    v2_get_error,
    v2_render_block
};

/* v2 Entry Point (default visibility required: built with -fvisibility=hidden) */
extern "C" __attribute__((visibility("default")))
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    (void)host;
    jv_debug("[JV880] v2 API initialized\n");
    return &jv880_api_v2;
}

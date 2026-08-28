/*
 * Drives the real dsp.so through the v2 plugin ABI, on the build host.
 *
 * Two jobs, selected by argv[3]:
 *
 *   dump <key>...   print what get_param returns, one key per line
 *   roundtrip       write every performance param and read it back
 *
 * The round-trip is the point. Performance common byte 12 carries keymode,
 * reverbtype AND chorustype; byte 16 carries choruslevel with chorusoutput in
 * its top bit; part byte 0 carries internalswitch alongside the transmit
 * switch, output select and transmit channel. A wrong shift or width in those
 * tables does not fail loudly -- it silently corrupts the sibling sharing the
 * byte. So every write is followed by a re-read of the SIBLINGS, not only of
 * the key written.
 *
 * The emulator is never ticked (render_block is not called), so this needs no
 * real ROMs: the SRAM the performance lives in is allocated either way, and
 * these parameters are plain memory plus a queued SysEx.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
    uint32_t api_version;
    void *(*create_instance)(const char *, const char *);
    void (*destroy_instance)(void *);
    void (*on_midi)(void *, const uint8_t *, int, int);
    void (*set_param)(void *, const char *, const char *);
    int (*get_param)(void *, const char *, char *, int);
    void (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

static plugin_api_v2_t *api;
static void *inst;
static int failures;

/* SHADOW_PARAM_VALUE_LEN in the host -- the real ceiling on the wire. A
 * contract that does not fit here is a contract nothing can read. */
#define WIRE_LIMIT 65536

static char big[1 << 20];

static const char *rd(const char *k) {
    memset(big, 0, WIRE_LIMIT + 1);
    int n = api->get_param(inst, k, big, WIRE_LIMIT);
    if (n < 0) return NULL;
    return big;
}

static void expect(const char *k, const char *want) {
    const char *got = rd(k);
    if (!got) { printf("  FAIL %s: read returned nothing\n", k); failures++; return; }
    if (strcmp(got, want) != 0) {
        printf("  FAIL %s: wrote %s, read back %s\n", k, want, got);
        failures++;
    }
}

static void set(const char *k, const char *v) { api->set_param(inst, k, v); }

struct KV { const char *key; const char *val; };

static int roundtrip(void) {
    /* ---- performance common: byte 12, three params in one byte ---- */
    printf("perf common byte 12 (keymode / reverbtype / chorustype):\n");
    struct KV b12[] = {
        {"sram_perfCommon_keymode",    "2"},
        {"sram_perfCommon_reverbtype", "7"},
        {"sram_perfCommon_chorustype", "2"},
    };
    for (int i = 0; i < 3; i++) set(b12[i].key, b12[i].val);
    for (int i = 0; i < 3; i++) expect(b12[i].key, b12[i].val);
    /* Move ONE and require the other two to hold. */
    set("sram_perfCommon_reverbtype", "3");
    expect("sram_perfCommon_reverbtype", "3");
    expect("sram_perfCommon_keymode", "2");
    expect("sram_perfCommon_chorustype", "2");

    /* ---- performance common: byte 16, level in the low 7 bits ---- */
    printf("perf common byte 16 (choruslevel / chorusoutput):\n");
    set("sram_perfCommon_choruslevel", "127");
    set("sram_perfCommon_chorusoutput", "1");
    expect("sram_perfCommon_choruslevel", "127");
    expect("sram_perfCommon_chorusoutput", "1");
    set("sram_perfCommon_chorusoutput", "0");
    expect("sram_perfCommon_choruslevel", "127");
    expect("sram_perfCommon_chorusoutput", "0");

    printf("perf common plain bytes:\n");
    struct KV plain[] = {
        {"sram_perfCommon_reverblevel",    "100"},
        {"sram_perfCommon_reverbtime",      "64"},
        {"sram_perfCommon_reverbfeedback",  "12"},
        {"sram_perfCommon_chorusdepth",     "90"},
        {"sram_perfCommon_chorusrate",       "5"},
        {"sram_perfCommon_chorusfeedback",  "33"},
    };
    for (size_t i = 0; i < sizeof(plain)/sizeof(plain[0]); i++) {
        set(plain[i].key, plain[i].val);
        expect(plain[i].key, plain[i].val);
    }

    /* ---- part controls that had no UI entry, or no reader ---- */
    printf("part internalswitch (byte 0 bit 7):\n");
    set("sram_part_2_internalswitch", "On");
    expect("sram_part_2_internalswitch", "On");
    set("sram_part_2_internalswitch", "Off");
    expect("sram_part_2_internalswitch", "Off");

    printf("part velocity pair (sense is centred at 64, max is direct):\n");
    struct KV vel[] = {
        {"sram_part_2_internalvelocitysense", "64"},
        {"sram_part_2_internalvelocitysense", "127"},
        {"sram_part_2_internalvelocitysense", "1"},
        {"sram_part_2_internalvelocitymax", "100"},
    };
    for (size_t i = 0; i < sizeof(vel)/sizeof(vel[0]); i++) {
        set(vel[i].key, vel[i].val);
        expect(vel[i].key, vel[i].val);
    }

    /* One part must not write over its neighbour. */
    printf("part isolation:\n");
    set("sram_part_3_internalvelocitymax", "7");
    expect("sram_part_2_internalvelocitymax", "100");
    expect("sram_part_3_internalvelocitymax", "7");

    return failures;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: probe <dsp.so> <module_dir> dump|roundtrip [keys...]\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    plugin_api_v2_t *(*init)(const void *) =
        (plugin_api_v2_t *(*)(const void *))dlsym(h, "move_plugin_init_v2");
    if (!init) { fprintf(stderr, "no move_plugin_init_v2\n"); return 2; }
    api = init(NULL);
    if (!api || api->api_version != 2) { fprintf(stderr, "bad plugin api\n"); return 2; }
    inst = api->create_instance(argv[2], "{}");
    if (!inst) { fprintf(stderr, "create_instance returned NULL\n"); return 3; }

    if (strcmp(argv[3], "dump") == 0) {
        for (int i = 4; i < argc; i++) {
            const char *v = rd(argv[i]);
            if (!v) { fprintf(stderr, "%s: get_param FAILED\n", argv[i]); return 4; }
            fprintf(stderr, "%s: %zu bytes (limit %d)\n", argv[i], strlen(v), WIRE_LIMIT);
            printf("%s\n", v);
        }
        return 0;
    }

    /* Wait for the ROM load thread to allocate the emulator -- every SRAM
     * accessor is guarded on inst->mcu, so a read that answers at all is the
     * readiness signal. Bounded, so a genuinely broken load fails the test
     * instead of hanging it. */
    for (int i = 0; i < 200 && !rd("sram_perfCommon_keymode"); i++) usleep(50000);
    if (!rd("sram_perfCommon_keymode")) {
        fprintf(stderr, "the emulator never came up -- nothing to round-trip\n");
        return 3;
    }

    int f = roundtrip();
    printf(f ? "\nFAILURES: %d\n" : "\nall round-trips OK (%d failures)\n", f);
    return f ? 1 : 0;
}

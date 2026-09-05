/*
 * Drives the real dsp.so through the v2 ABI to simulate a Schwung set
 * save/reload of the synth state, on the build host.
 *
 *   save <preset> <cutoff>       create, wait for loading_complete, select
 *                                the patch, edit tone-0 cutoff, print
 *                                get_param("state") on stdout.
 *   restore <statefile> <preset> create and, BEFORE loading completes, apply
 *                                set_param("preset") then set_param("state")
 *                                in the order chain_patch.c uses at set load
 *                                (the deferred-restore path). statefile "-"
 *                                sends preset only. Then wait and print
 *                                preset and cutoff.
 *
 * The emulator is never ticked; the working patch is plain NVRAM, so
 * zero-filled ROM stand-ins are enough (see jv880_probe.c).
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
static char big[1 << 20];

static const char *rd(const char *k) {
    memset(big, 0, sizeof(big));
    int n = api->get_param(inst, k, big, 65536);
    if (n < 0) return NULL;
    return big;
}

static void rds(const char *k, char *out, int out_len) {
    const char *v = rd(k);
    snprintf(out, out_len, "%s", v ? v : "(null)");
}

static int wait_loaded(void) {
    for (int i = 0; i < 600; i++) {           /* up to 60 s */
        const char *v = rd("loading_complete");
        if (v && strcmp(v, "1") == 0) return 0;
        usleep(100000);
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s dsp.so dir save <preset> <cutoff>\n"
                        "       %s dsp.so dir restore <statefile|-> <preset>\n",
                argv[0], argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    plugin_api_v2_t *(*init)(const void *) =
        (plugin_api_v2_t *(*)(const void *))dlsym(h, "move_plugin_init_v2");
    if (!init) { fprintf(stderr, "no move_plugin_init_v2\n"); return 2; }
    static void *host_api[64];                 /* zeroed host api, all optional */
    api = init(host_api);
    if (!api || api->api_version != 2) { fprintf(stderr, "bad api\n"); return 2; }

    const char *mode = argv[3];

    if (strcmp(mode, "save") == 0) {
        inst = api->create_instance(argv[2], "{}");
        if (!inst) { fprintf(stderr, "create failed\n"); return 2; }
        if (wait_loaded()) { fprintf(stderr, "load timeout\n"); return 2; }
        api->set_param(inst, "preset", argv[4]);
        api->set_param(inst, "nvram_tone_0_cutofffrequency", argv[5]);
        char cbuf[64], pbuf[64];
        rds("nvram_tone_0_cutofffrequency", cbuf, sizeof(cbuf));
        rds("preset", pbuf, sizeof(pbuf));
        fprintf(stderr, "edit readback: cutoff=%s preset=%s\n", cbuf, pbuf);
        const char *st = rd("state");
        if (!st || !st[0]) { fprintf(stderr, "state read failed\n"); return 2; }
        fputs(st, stdout);
        return 0;
    }

    if (strcmp(mode, "restore") == 0) {
        static char state[131072]; state[0] = 0;
        if (strcmp(argv[4], "-") != 0) {
            FILE *f = fopen(argv[4], "r");
            if (!f) { fprintf(stderr, "open %s failed\n", argv[4]); return 2; }
            size_t n = fread(state, 1, sizeof(state) - 1, f);
            state[n] = 0; fclose(f);
        }
        inst = api->create_instance(argv[2], "{}");
        if (!inst) { fprintf(stderr, "create failed\n"); return 2; }
        /* chain_patch.c order: preset first, then state — immediately, i.e.
         * while the ROM load is still running (the deferred path). */
        api->set_param(inst, "preset", argv[5]);
        if (state[0]) api->set_param(inst, "state", state);
        if (wait_loaded()) { fprintf(stderr, "load timeout\n"); return 2; }
        usleep(300000);                        /* let the deferred apply settle */
        char cbuf[64], pbuf[64];
        rds("preset", pbuf, sizeof(pbuf));
        rds("nvram_tone_0_cutofffrequency", cbuf, sizeof(cbuf));
        printf("preset=%s cutoff=%s\n", pbuf, cbuf);
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}

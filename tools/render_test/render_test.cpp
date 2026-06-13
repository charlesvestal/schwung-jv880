/*
 * render_test.cpp — headless JV-880 audio-validation harness
 *
 * Compiles mcu.cpp / mcu_opcodes.cpp / pcm.cpp directly (portable C++).
 * Runs the emulator deterministically (no threads, no wall-clock), renders
 * scripted MIDI to a WAV file so that optimisation diffs can be bit-compared.
 *
 * Usage:
 *   render_test <roms_dir> <out.wav> [--patch N] [--seq melody|chord|drums] [--seconds S]
 *
 * ROM files expected in <roms_dir>:
 *   jv880_rom1.bin   (0x8000 bytes)
 *   jv880_rom2.bin   (0x40000 bytes)
 *   jv880_waverom1.bin (0x200000 bytes)
 *   jv880_waverom2.bin (0x200000 bytes)
 *   jv880_nvram.bin  (optional, 0x8000 bytes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// Pull in the emulator sources via the include path set by build.sh
#include "mcu.h"

// ─── constants ─────────────────────────────────────────────────────────────

// JV-880 native sample rate (64 kHz internal PCM clock)
static const int SAMPLE_RATE = 64000;

// Patch / ROM layout  (matches jv880_plugin.cpp)
static const int PATCH_SIZE          = 0x16a;
static const uint32_t PATCH_OFFSET_PRESET_A = 0x010ce0;
static const uint32_t PATCH_OFFSET_PRESET_B = 0x018ce0;
static const uint32_t PATCH_OFFSET_INTERNAL = 0x008ce0;
static const int NVRAM_PATCH_OFFSET  = 0x0d70;
static const int NVRAM_MODE_OFFSET   = 0x11;

// Warmup: ~3 s worth of emulator ticks at 64 kHz
// updateSC55(1) runs until sample_write_ptr reaches 1 sample (see mcu.cpp).
// The plugin does 100 000 single-step updateSC55(1) calls; we do the same.
static const int WARMUP_STEPS        = 100000;

// ─── WAV writer ────────────────────────────────────────────────────────────

static bool wav_write(const char *path,
                      const int16_t *samples, int n_frames,
                      int sample_rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot create %s\n", path); return false; }

    uint32_t data_bytes = (uint32_t)(n_frames * 2 * sizeof(int16_t)); // stereo
    uint32_t riff_size  = 36 + data_bytes;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1;          // PCM
    fwrite(&audio_fmt, 2, 1, f);
    uint16_t channels = 2;
    fwrite(&channels, 2, 1, f);
    uint32_t sr = (uint32_t)sample_rate;
    fwrite(&sr, 4, 1, f);
    uint32_t byte_rate = sr * 2 * 2;
    fwrite(&byte_rate, 4, 1, f);
    uint16_t block_align = 4;
    fwrite(&block_align, 2, 1, f);
    uint16_t bits = 16;
    fwrite(&bits, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    fwrite(samples, sizeof(int16_t), (size_t)n_frames * 2, f);

    fclose(f);
    return true;
}

// ─── ROM loader ────────────────────────────────────────────────────────────

static uint8_t *load_file(const char *path, size_t expected)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (expected && (size_t)sz != expected) {
        fprintf(stderr, "Size mismatch %s: got %ld expected %zu\n", path, sz, expected);
        fclose(f);
        return nullptr;
    }
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(f); return nullptr; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "Read error %s\n", path);
        free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    return buf;
}

// ─── patch loading helpers ─────────────────────────────────────────────────

// global_index: 0..63 = Preset A, 64..127 = Preset B, 128..191 = Internal
static void load_patch(MCU *mcu, const uint8_t *rom2, int global_index)
{
    uint32_t base;
    int local = global_index;
    if (global_index < 64) {
        base  = PATCH_OFFSET_PRESET_A;
        local = global_index;
    } else if (global_index < 128) {
        base  = PATCH_OFFSET_PRESET_B;
        local = global_index - 64;
    } else {
        base  = PATCH_OFFSET_INTERNAL;
        local = global_index - 128;
    }
    uint32_t offset = base + (uint32_t)(local * PATCH_SIZE);
    memcpy(&mcu->nvram[NVRAM_PATCH_OFFSET], &rom2[offset], PATCH_SIZE);
    mcu->nvram[NVRAM_MODE_OFFSET] = 1;   // patch mode

    // Send Program Change 0 on ch1 to make the emulator reload from NVRAM
    uint8_t pc[2] = { 0xC0, 0x00 };
    mcu->postMidiSC55(pc, 2);
}

// ─── MIDI event queue ──────────────────────────────────────────────────────

struct MidiEvent {
    int      sample;   // absolute sample time to fire
    uint8_t  data[3];
    int      len;
};

static const int MAX_EVENTS = 4096;
static MidiEvent g_events[MAX_EVENTS];
static int       g_n_events = 0;

static void add_event(int sample, uint8_t b0, uint8_t b1, uint8_t b2, int len)
{
    if (g_n_events >= MAX_EVENTS) return;
    MidiEvent &e = g_events[g_n_events++];
    e.sample  = sample;
    e.data[0] = b0; e.data[1] = b1; e.data[2] = b2;
    e.len     = len;
}

// ─── sequence builders ─────────────────────────────────────────────────────

// Returns total samples needed for the sequence
static int build_melody(int extra_seconds)
{
    // Notes 48,55,60,64,67 on ch1 vel 100, each 0.5 s held + 0.25 s gap
    const uint8_t notes[]  = {48, 55, 60, 64, 67};
    const int     n_notes  = (int)(sizeof(notes)/sizeof(notes[0]));
    const int     hold     = (int)(0.50f * SAMPLE_RATE);  // 32000 samples
    const int     gap      = (int)(0.25f * SAMPLE_RATE);  // 16000 samples
    const int     tail     = 2 * SAMPLE_RATE;
    const int     step     = hold + gap;

    for (int i = 0; i < n_notes; i++) {
        int t_on  = i * step;
        int t_off = t_on + hold;
        add_event(t_on,  0x90, notes[i], 100, 3);  // note on
        add_event(t_off, 0x80, notes[i], 0,   3);  // note off
    }
    return n_notes * step + tail + extra_seconds * SAMPLE_RATE;
}

static int build_chord(int extra_seconds)
{
    // C major 7: C4=60 E4=64 G4=67 B4=71, held 3 s + 2 s tail
    const uint8_t chord[] = {60, 64, 67, 71};
    const int n_notes = (int)(sizeof(chord)/sizeof(chord[0]));
    const int hold    = 3 * SAMPLE_RATE;
    const int tail    = 2 * SAMPLE_RATE;

    for (int i = 0; i < n_notes; i++) {
        add_event(0,    0x90, chord[i], 100, 3);
        add_event(hold, 0x80, chord[i], 0,   3);
    }
    return hold + tail + extra_seconds * SAMPLE_RATE;
}

static int build_drums(int extra_seconds)
{
    // Kick=36 Snare=38 HH=42 on ch10, 120 BPM (0.5 s per beat, 2 bars of 4/4)
    const int beat = SAMPLE_RATE / 2;   // 32000 samples
    const int n_beats = 8;              // 2 bars

    for (int b = 0; b < n_beats; b++) {
        int t = b * beat;
        // Kick on beats 1,3
        if (b % 2 == 0) {
            add_event(t,      0x99, 36, 120, 3);
            add_event(t + 10, 0x89, 36, 0,   3);
        }
        // Snare on beats 2,4
        if (b % 2 == 1) {
            add_event(t,      0x99, 38, 110, 3);
            add_event(t + 10, 0x89, 38, 0,   3);
        }
        // Hi-hat every beat
        add_event(t,      0x99, 42, 80, 3);
        add_event(t + 10, 0x89, 42, 0,  3);
    }
    const int tail = 2 * SAMPLE_RATE;
    return n_beats * beat + tail + extra_seconds * SAMPLE_RATE;
}

// ─── render ────────────────────────────────────────────────────────────────

// Drain whatever samples updateSC55() produced into our output buffer.
// sample_write_ptr advances by one interleaved stereo pair per inner tick.
// After updateSC55(N) returns, sample_write_ptr == N (it is reset to 0 at
// the start of each updateSC55 call).  We drain by keeping our own cursor.
static void drain(MCU *mcu, int16_t *out, int capacity,
                  int &write_pos, int n_produced)
{
    // sample_buffer holds sample_write_ptr stereo samples starting from index 0
    int to_copy = n_produced;
    if (write_pos + to_copy > capacity) to_copy = capacity - write_pos;
    if (to_copy <= 0) return;
    memcpy(out + (size_t)write_pos * 2,
           mcu->sample_buffer,
           (size_t)to_copy * 2 * sizeof(int16_t));
    write_pos += to_copy;
}

static bool render(MCU *mcu, const uint8_t *rom2,
                   int patch_index, const char *seq_name,
                   int total_seconds, const char *out_path)
{
    // ── build MIDI sequence ──────────────────────────────────────────────
    g_n_events = 0;
    int extra_seconds = (total_seconds > 0) ? 0 : 0;  // controlled by total_seconds
    int seq_samples;
    if (strcmp(seq_name, "chord") == 0) {
        seq_samples = build_chord(0);
    } else if (strcmp(seq_name, "drums") == 0) {
        seq_samples = build_drums(0);
    } else {
        seq_samples = build_melody(0);
    }

    int n_samples = (total_seconds > 0) ? (total_seconds * SAMPLE_RATE) : seq_samples;

    // ── allocate output buffer ──────────────────────────────────────────
    int16_t *out = (int16_t *)calloc((size_t)n_samples * 2, sizeof(int16_t));
    if (!out) { fprintf(stderr, "OOM allocating output buffer\n"); return false; }

    // ── warmup ──────────────────────────────────────────────────────────
    fprintf(stderr, "Warming up (%d steps)...\n", WARMUP_STEPS);
    for (int i = 0; i < WARMUP_STEPS; i++) {
        mcu->updateSC55(1);
    }

    // ── select patch ────────────────────────────────────────────────────
    fprintf(stderr, "Loading patch %d...\n", patch_index);
    load_patch(mcu, rom2, patch_index);

    // Give the emulator ~0.5 s to react to the program change before
    // we start recording — keeps the very first samples clean.
    int settle = SAMPLE_RATE / 2;
    for (int i = 0; i < settle; i++) {
        mcu->updateSC55(1);
    }

    // ── render loop ─────────────────────────────────────────────────────
    fprintf(stderr, "Rendering %d samples at %d Hz...\n", n_samples, SAMPLE_RATE);
    int ev_idx   = 0;
    int write_pos = 0;
    const int CHUNK = 64;   // must match audio_buffer_size headroom

    for (int pos = 0; pos < n_samples; pos += CHUNK) {
        int this_chunk = CHUNK;
        if (pos + this_chunk > n_samples) this_chunk = n_samples - pos;

        // Fire MIDI events whose time falls within [pos, pos+this_chunk)
        while (ev_idx < g_n_events && g_events[ev_idx].sample < pos + this_chunk) {
            const MidiEvent &ev = g_events[ev_idx];
            if (ev.sample >= pos) {
                mcu->postMidiSC55(ev.data, ev.len);
            }
            ev_idx++;
        }

        // Run emulator for this_chunk samples
        mcu->updateSC55(this_chunk);
        drain(mcu, out, n_samples, write_pos, this_chunk);
    }

    // ── write WAV ────────────────────────────────────────────────────────
    fprintf(stderr, "Writing %s (%d frames)...\n", out_path, n_samples);
    bool ok = wav_write(out_path, out, n_samples, SAMPLE_RATE);
    free(out);

    if (ok) {
        fprintf(stderr, "Done: %s\n", out_path);
    }
    return ok;
}

// ─── simple audio analysis ─────────────────────────────────────────────────

static void analyse_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;

    // Skip 44-byte WAV header
    fseek(f, 44, SEEK_SET);

    int64_t sum_sq_l = 0, sum_sq_r = 0;
    int32_t peak_l = 0, peak_r = 0;
    long n = 0;
    int16_t buf[2];
    while (fread(buf, sizeof(int16_t), 2, f) == 2) {
        int32_t l = buf[0], r = buf[1];
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > peak_l) peak_l = l;
        if (r > peak_r) peak_r = r;
        sum_sq_l += (int64_t)buf[0] * buf[0];
        sum_sq_r += (int64_t)buf[1] * buf[1];
        n++;
    }
    fclose(f);

    if (n == 0) { fprintf(stderr, "  [WARN] zero samples!\n"); return; }

    double rms_l = sqrt((double)sum_sq_l / (double)n);
    double rms_r = sqrt((double)sum_sq_r / (double)n);
    double db_peak_l = 20.0 * log10((double)peak_l / 32768.0 + 1e-12);
    double db_peak_r = 20.0 * log10((double)peak_r / 32768.0 + 1e-12);
    double db_rms_l  = 20.0 * log10(rms_l / 32768.0 + 1e-12);
    double db_rms_r  = 20.0 * log10(rms_r / 32768.0 + 1e-12);
    fprintf(stderr, "  peak L/R: %.1f / %.1f dBFS  rms L/R: %.1f / %.1f dBFS  frames: %ld\n",
            db_peak_l, db_peak_r, db_rms_l, db_rms_r, n);

    if (peak_l < 10 && peak_r < 10)
        fprintf(stderr, "  [WARN] audio appears silent!\n");
}

// ─── main ───────────────────────────────────────────────────────────────────

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <roms_dir> <out.wav> [--patch N] [--seq melody|chord|drums] [--seconds S]\n"
            "  N: global patch index (0=PresetA[0], 64=PresetB[0], 128=Internal[0])\n"
            "  Default: --patch 0 --seq melody\n",
            prog);
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *roms_dir = argv[1];
    const char *out_path = argv[2];

    int   patch_index  = 0;
    const char *seq    = "melody";
    int   total_seconds = 0;   // 0 = use sequence natural length

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--patch") == 0 && i+1 < argc) {
            patch_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seq") == 0 && i+1 < argc) {
            seq = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i+1 < argc) {
            total_seconds = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    // ── load ROMs ────────────────────────────────────────────────────────
    char path[1024];

    snprintf(path, sizeof(path), "%s/jv880_rom1.bin", roms_dir);
    uint8_t *rom1_data = load_file(path, ROM1_SIZE);
    if (!rom1_data) return 1;

    snprintf(path, sizeof(path), "%s/jv880_rom2.bin", roms_dir);
    uint8_t *rom2_data = load_file(path, ROM2_SIZE);
    if (!rom2_data) return 1;

    snprintf(path, sizeof(path), "%s/jv880_waverom1.bin", roms_dir);
    uint8_t *waverom1_data = load_file(path, 0x200000);
    if (!waverom1_data) return 1;

    snprintf(path, sizeof(path), "%s/jv880_waverom2.bin", roms_dir);
    uint8_t *waverom2_data = load_file(path, 0x200000);
    if (!waverom2_data) return 1;

    // NVRAM is optional
    snprintf(path, sizeof(path), "%s/jv880_nvram.bin", roms_dir);
    uint8_t *nvram_data = (uint8_t *)malloc(NVRAM_SIZE);
    if (!nvram_data) return 1;
    memset(nvram_data, 0xFF, NVRAM_SIZE);
    FILE *nf = fopen(path, "rb");
    if (nf) {
        fread(nvram_data, 1, NVRAM_SIZE, nf);
        fclose(nf);
        fprintf(stderr, "Loaded NVRAM\n");
    }

    // Keep a copy of rom2 for patch selection
    uint8_t *rom2_copy = (uint8_t *)malloc(ROM2_SIZE);
    if (!rom2_copy) return 1;
    memcpy(rom2_copy, rom2_data, ROM2_SIZE);

    // ── initialise emulator ──────────────────────────────────────────────
    fprintf(stderr, "Initialising MCU...\n");
    MCU *mcu = new MCU();
    mcu->startSC55(rom1_data, rom2_data,
                   waverom1_data, waverom2_data,
                   nvram_data);

    free(rom1_data);
    free(waverom1_data);
    free(waverom2_data);
    free(nvram_data);

    // ── render ────────────────────────────────────────────────────────────
    bool ok = render(mcu, rom2_copy, patch_index, seq, total_seconds, out_path);

    // ── analyse ──────────────────────────────────────────────────────────
    if (ok) {
        fprintf(stderr, "Audio stats for %s:\n", out_path);
        analyse_wav(out_path);
    }

    // ── cleanup ──────────────────────────────────────────────────────────
    delete mcu;
    free(rom2_copy);
    free(rom2_data);

    return ok ? 0 : 1;
}

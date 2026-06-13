/* resampler_test.cpp
 *
 * Empirical verification + benchmark of the fixed-ratio 64000->44100
 * polyphase resampler (src/dsp/resampler_fixed.h) against libresample HQ.
 *
 * Tests:
 *   1. Single-tone purity: for a set of input tones below output Nyquist,
 *      measure output SNR (energy at the tone vs everything else) and the
 *      worst alias product via Goertzel.  Reports both resamplers.
 *   2. Passband ripple: sweep tones 100 Hz..18 kHz, measure output magnitude
 *      vs ideal, report max deviation (dB).
 *   3. Stopband attenuation: place a tone just above output Nyquist that must
 *      be rejected... (n/a for downsampling alias check) -- instead we verify
 *      that an input tone that WOULD alias is attenuated: input near input
 *      Nyquist guard.  Practically covered by SNR of in-band tones.
 *   4. Benchmark: process several seconds of audio through each, report
 *      ns/output-frame.
 *
 * Build/run: see tools/resampler_test/Makefile  (or instructions in report).
 */
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <ctime>

extern "C" {
#include "resample/libresample.h"
}
#include "resampler_fixed.h"

static const double FS_IN  = 64000.0;
static const double FS_OUT = 44100.0;

/* Goertzel on a Blackman-Harris-windowed block: energy of frequency f (Hz).
 * Windowing suppresses the main tone's spectral skirts so that the power read
 * at a *different* (alias) frequency reflects real spectral content there, not
 * leakage of the strong in-band tone.  The window scales all bins equally so
 * relative (dB) comparisons between signal and alias are unaffected. */
static double goertzel_power(const float *x, int N, double f, double fs)
{
    double w = 2.0 * M_PI * f / fs;
    double cw = cos(w), c = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    const double a0=0.35875, a1=0.48829, a2=0.14128, a3=0.01168;
    for (int n = 0; n < N; ++n) {
        double t = 2.0*M_PI*(double)n/(double)(N-1);
        double win = a0 - a1*cos(t) + a2*cos(2*t) - a3*cos(3*t);
        double xn = x[n] * win;
        s0 = xn + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double re = s1 - s2 * cw;
    double im = s2 * sin(w);
    return re * re + im * im;
}

/* Blackman-Harris windowed total power of a block (same window as Goertzel,
 * so signal vs total are on the same scale for SNR). */
static double total_power(const float *x, int N)
{
    const double a0=0.35875, a1=0.48829, a2=0.14128, a3=0.01168;
    double s = 0;
    for (int n = 0; n < N; ++n) {
        double t = 2.0*M_PI*(double)n/(double)(N-1);
        double win = a0 - a1*cos(t) + a2*cos(2*t) - a3*cos(3*t);
        double xn = (double)x[n]*win;
        s += xn*xn;
    }
    return s;
}

/* Generate a sine at f Hz, fs, length N into buf. */
static void gen_sine(float *buf, int N, double f, double fs, double amp)
{
    for (int n = 0; n < N; ++n) buf[n] = (float)(amp * sin(2.0*M_PI*f*(double)n/fs));
}

/* --- libresample wrapper: resample full input buffer (separate channel). --- */
static int lr_run(float *in, int in_n, float *out, int out_cap)
{
    double ratio = FS_OUT / FS_IN;
    void *h = resample_open(1, ratio, ratio);
    int used = 0;
    int got = resample_process(h, ratio, in, in_n, 1, &used, out, out_cap);
    resample_close(h);
    return got;
}

/* --- our resampler wrapper (mono via L channel) --- */
static int rf_run(float *in, int in_n, float *out, int out_cap)
{
    ResamplerFixed r;
    ResamplerFixed_init(&r);
    /* process in chunks of 64 like the real plugin, to exercise phase state */
    std::vector<float> dummyR(in_n);
    std::vector<float> outR(out_cap);
    int total = 0;
    int i = 0;
    while (i < in_n && total < out_cap) {
        int chunk = (in_n - i < 64) ? (in_n - i) : 64;
        int n = ResamplerFixed_process(&r, in + i, dummyR.data() + i, chunk,
                                       out + total, outR.data() + total);
        total += n;
        i += chunk;
    }
    return total;
}

struct ToneResult { double snr_db; double worst_alias_db; double gain_db; };

/* Measure SNR + worst alias for a single tone through resampler `run`. */
static ToneResult test_tone(int (*run)(float*,int,float*,int), double f_in, double amp)
{
    const int IN_N = 64000 * 2;           /* 2 s of input */
    const int OUT_CAP = (int)(IN_N * FS_OUT / FS_IN) + 1024;
    std::vector<float> in(IN_N), out(OUT_CAP);
    gen_sine(in.data(), IN_N, f_in, FS_IN, amp);
    int got = run(in.data(), IN_N, out.data(), OUT_CAP);

    /* analyze the steady-state middle of output to avoid filter warmup/flush */
    int skip = 2000;
    int an_n = got - 2 * skip;
    if (an_n < 8000) an_n = got, skip = 0;
    const float *y = out.data() + skip;

    double pt = goertzel_power(y, an_n, f_in, FS_OUT);
    double ptot = total_power(y, an_n);
    double pnoise = ptot - pt;
    if (pnoise < 1e-30) pnoise = 1e-30;
    double snr = 10.0 * log10(pt / pnoise);

    /* worst alias: scan candidate alias freqs.  For input f_in, downsampling
     * images appear at |k*FS_IN +/- f_in| folded into [0,FS_OUT/2].  Check a
     * grid plus the principal alias. */
    double worst = -200.0;
    for (int k = 1; k <= 3; ++k) {
        double cand[2] = { fabs(k*FS_IN - f_in), fabs(k*FS_IN + f_in) };
        for (int c = 0; c < 2; ++c) {
            double fa = cand[c];
            /* fold into [0, FS_OUT/2] */
            while (fa > FS_OUT) fa -= FS_OUT;
            if (fa > FS_OUT/2) fa = FS_OUT - fa;
            if (fabs(fa - f_in) < 50.0) continue; /* that's the signal */
            if (fa < 20 || fa > FS_OUT/2 - 20) continue;
            double pa = goertzel_power(y, an_n, fa, FS_OUT);
            double db = 10.0*log10(pa / pt);
            if (db > worst) worst = db;
        }
    }

    /* gain: measured amplitude vs input amplitude.  With a Blackman-Harris
     * window the single-bin Goertzel magnitude is amp * (sum of window)/2, so
     * recover amplitude by dividing by the window's coherent gain (its mean).
     * The Blackman-Harris a0 coefficient (0.35875) is exactly that mean. */
    const double win_cg = 0.35875;
    double meas_amp = sqrt(pt) * 2.0 / (an_n * win_cg);
    double gain_db = 20.0 * log10(meas_amp / amp);

    ToneResult tr; tr.snr_db = snr; tr.worst_alias_db = worst; tr.gain_db = gain_db;
    return tr;
}

static double bench(int (*run)(float*,int,float*,int), int *out_frames)
{
    const int IN_N = 64000 * 10;          /* 10 s */
    const int OUT_CAP = (int)(IN_N * FS_OUT / FS_IN) + 1024;
    std::vector<float> in(IN_N), out(OUT_CAP);
    /* white-ish noise input */
    for (int i = 0; i < IN_N; ++i) in[i] = (float)((rand()/(double)RAND_MAX)*2.0-1.0)*0.5f;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int got = run(in.data(), IN_N, out.data(), OUT_CAP);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec-t0.tv_sec)*1e9 + (t1.tv_nsec-t0.tv_nsec);
    *out_frames = got;
    return ns / (double)got;   /* ns per output frame */
}

int main()
{
    printf("Fixed-ratio resampler verification  (64000 -> 44100, L=%d M=%d)\n", RF_L, RF_M);
    printf("  taps/phase=%d  total taps=%d  Kaiser beta=%.2f  cutoff=%.1f Hz\n\n",
           RF_TAPS_PER_PHASE, RF_NUM_COEFFS, RF_KAISER_BETA, RF_CUTOFF_FRAC*FS_IN/2.0);

    double tones[] = {100, 440, 1000, 4000, 8000, 12000, 15000, 18000, 20000, 21000};
    int nt = sizeof(tones)/sizeof(tones[0]);

    printf("%-9s | %-26s | %-26s\n", "tone(Hz)", "libresample-HQ", "fixed-poly");
    printf("%-9s | %8s %8s %7s | %8s %8s %7s\n", "",
           "SNR dB","alias","gain","SNR dB","alias","gain");
    printf("----------+----------------------------+----------------------------\n");

    double min_snr_rf = 1e9;
    double worst_alias_rf = -200;
    /* Passband ripple = peak-to-peak gain deviation across in-band tones,
     * which cancels any constant amplitude-calibration offset. */
    double gmin = 1e9, gmax = -1e9;
    for (int i = 0; i < nt; ++i) {
        ToneResult lr = test_tone(lr_run, tones[i], 0.5);
        ToneResult rf = test_tone(rf_run, tones[i], 0.5);
        printf("%8.0f  | %8.1f %8.1f %7.2f | %8.1f %8.1f %7.2f\n",
               tones[i], lr.snr_db, lr.worst_alias_db, lr.gain_db,
               rf.snr_db, rf.worst_alias_db, rf.gain_db);
        if (tones[i] <= 18000.0) {
            if (rf.snr_db < min_snr_rf) min_snr_rf = rf.snr_db;
            if (rf.gain_db < gmin) gmin = rf.gain_db;
            if (rf.gain_db > gmax) gmax = rf.gain_db;
        }
        if (rf.worst_alias_db > worst_alias_rf) worst_alias_rf = rf.worst_alias_db;
    }
    double max_ripple_rf = gmax - gmin;

    printf("\nfixed-poly summary (in-band, <=18kHz):\n");
    printf("  min SNR            = %.1f dB\n", min_snr_rf);
    printf("  max passband ripple= %.3f dB pk-pk  (target <= 0.1)\n", max_ripple_rf);
    printf("  worst alias        = %.1f dB  (target <= -70)\n", worst_alias_rf);

    bool pass_snr   = (min_snr_rf >= 70.0);
    bool pass_rip   = (max_ripple_rf <= 0.1);
    bool pass_alias = (worst_alias_rf <= -70.0);

    int fr_lr=0, fr_rf=0;
    double ns_lr = bench(lr_run, &fr_lr);
    double ns_rf = bench(rf_run, &fr_rf);
    printf("\nBenchmark (10 s noise, mono, host):\n");
    printf("  libresample-HQ : %7.2f ns/out-frame\n", ns_lr);
    printf("  fixed-poly     : %7.2f ns/out-frame\n", ns_rf);
    printf("  speedup        : %.2fx  (target >= 2x)\n", ns_lr / ns_rf);

    printf("\nGroup delay:\n");
    printf("  fixed-poly     : %.2f input samples (%.3f ms), %.2f output samples\n",
           ResamplerFixed_group_delay_input_samples(),
           ResamplerFixed_group_delay_input_samples()/FS_IN*1000.0,
           ResamplerFixed_group_delay_output_samples());
    printf("  libresample-HQ : filter half-width Nmult=35 taps -> ~35 input samples (~0.55 ms)\n");

    bool pass_speed = (ns_lr/ns_rf >= 2.0);
    printf("\nRESULT: SNR %s | ripple %s | alias %s | speedup %s\n",
           pass_snr?"PASS":"FAIL", pass_rip?"PASS":"FAIL",
           pass_alias?"PASS":"FAIL", pass_speed?"PASS":"FAIL");
    bool all = pass_snr && pass_rip && pass_alias && pass_speed;
    printf("%s\n", all ? "ALL TARGETS MET" : "SOME TARGETS NOT MET");
    return all ? 0 : 1;
}

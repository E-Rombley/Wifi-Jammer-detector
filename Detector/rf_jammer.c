// RF Jammer Detector & Triangulator
//
// Phase 1 — Baseline: measures average noise floor per channel
// Phase 2 — Monitor:  flags channels where power spikes above baseline
// Phase 3 — Triangulate: user moves HackRF to 3+ known positions;
//            RSS trilateration estimates jammer (x,y) in metres
//
// Compile: gcc -o rf_jammer rf_jammer.c -lhackrf -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <libhackrf/hackrf.h>

// ─── Constants ───────────────────────────────────────────────────────────────

#define TOTAL_CHANNELS      39   // 14 x 2.4GHz + 25 x 5GHz
#define SAMPLE_RATE         20000000   // 20 MHz
#define LNA_GAIN            40
#define VGA_GAIN            32
#define BASELINE_SAMPLES    5          // scans averaged for baseline
#define JAM_THRESHOLD_DB    10.0f      // dB above baseline = jamming
#define MAX_POSITIONS       16         // max triangulation readings
#define PATH_LOSS_EXP       3.0        // ~3 for indoor/urban
#define REF_DIST_M          1.0        // reference distance (metres)

// ─── Structs ─────────────────────────────────────────────────────────────────

struct channel {
    int     number;
    float   frequency;   // MHz
    float   baseline;    // averaged noise floor (linear power)
    float   current;     // latest reading
};

// One RSS reading taken at a known position
struct position_sample {
    double x, y;         // metres (user-supplied)
    float  power;        // measured linear power on jammed channel
};

// ─── Globals ─────────────────────────────────────────────────────────────────

struct channel         channels[TOTAL_CHANNELS];
struct position_sample samples[MAX_POSITIONS];
int                    sample_count     = 0;
int                    current_ch_idx   = 0;
volatile int           running          = 1;

// ─── Helpers ─────────────────────────────────────────────────────────────────

void handle_sigint(int sig) { (void)sig; running = 0; }

static float power_to_db(float linear) {
    if (linear <= 0.0f) return -999.0f;
    return 10.0f * log10f(linear);
}

// Standard 5GHz channel numbers (formula: freq = 5000 + ch*5 MHz)
static const int ch5ghz[] = {
    36, 40, 44, 48,                          // UNII-1
    52, 56, 60, 64,                          // UNII-2A
    100,104,108,112,116,120,124,128,132,136,140,144,  // UNII-2C
    149,153,157,161,165                      // UNII-3
};

void init_channels(void) {
    // 2.4GHz channels 1-14
    for (int i = 0; i < 14; i++) {
        channels[i].number    = i + 1;
        channels[i].frequency = (i < 13) ? 2412.0f + i * 5.0f : 2484.0f;
        channels[i].baseline  = 0.0f;
        channels[i].current   = 0.0f;
    }
    // 5GHz channels
    int n5 = (int)(sizeof(ch5ghz) / sizeof(ch5ghz[0]));
    for (int i = 0; i < n5; i++) {
        channels[14 + i].number    = ch5ghz[i];
        channels[14 + i].frequency = 5000.0f + ch5ghz[i] * 5.0f;
        channels[14 + i].baseline  = 0.0f;
        channels[14 + i].current   = 0.0f;
    }
}

// ─── HackRF ──────────────────────────────────────────────────────────────────

int rx_callback(hackrf_transfer *transfer) {
    int8_t *s   = (int8_t *)transfer->buffer;
    int     len = transfer->valid_length;
    if (len < 2) return 0;
    double  p   = 0.0;

    for (int i = 0; i + 1 < len; i += 2) {
        double I = s[i], Q = s[i + 1];
        p += I * I + Q * Q;
    }
    p /= (len / 2);

    channels[current_ch_idx].current = (float)p;
    return 0;
}

// Open HackRF once and return the handle. Caller must call hackrf_device_close().
static hackrf_device *hackrf_open_device(void) {
    hackrf_device *dev = NULL;
    if (hackrf_init() != HACKRF_SUCCESS) {
        fprintf(stderr, "[!] hackrf_init failed\n");
        return NULL;
    }
    if (hackrf_open(&dev) != HACKRF_SUCCESS) {
        fprintf(stderr, "[!] hackrf_open failed\n");
        hackrf_exit();
        return NULL;
    }
    hackrf_set_sample_rate(dev, SAMPLE_RATE);
    hackrf_set_amp_enable(dev, 0);
    hackrf_set_lna_gain(dev, LNA_GAIN);
    hackrf_set_vga_gain(dev, VGA_GAIN);
    return dev;
}

static void hackrf_close_device(hackrf_device *dev) {
    hackrf_close(dev);
    hackrf_exit();
}

static void scan_with_device(hackrf_device *dev, float out[TOTAL_CHANNELS]) {
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        uint64_t hz = (uint64_t)(channels[i].frequency * 1e6);
        hackrf_set_freq(dev, hz);
        current_ch_idx = i;
        hackrf_start_rx(dev, rx_callback, NULL);
        struct timespec ts = {0, 100000000L};  // 100ms
        nanosleep(&ts, NULL);
        hackrf_stop_rx(dev);
        out[i] = channels[i].current;
    }
}

// One-shot scan (opens and closes HackRF).
void scan_once(float out[TOTAL_CHANNELS]) {
    hackrf_device *dev = hackrf_open_device();
    if (!dev) return;
    scan_with_device(dev, out);
    hackrf_close_device(dev);
}

// Scan a single channel and return its power (used during triangulation).
float scan_channel(int ch_idx) {
    float buf[TOTAL_CHANNELS] = {0};
    scan_once(buf);
    return buf[ch_idx];
}

// ─── Phase 1: Baseline ────────────────────────────────────────────────────────

// Returns false if HackRF failed or all readings were zero.
bool build_baseline(void) {
    printf("\n[baseline] Collecting %d scans to establish noise floor...\n",
           BASELINE_SAMPLES);

    hackrf_device *dev = hackrf_open_device();
    if (!dev) return false;

    float acc[TOTAL_CHANNELS] = {0};

    for (int s = 0; s < BASELINE_SAMPLES; s++) {
        float out[TOTAL_CHANNELS] = {0};
        scan_with_device(dev, out);
        for (int i = 0; i < TOTAL_CHANNELS; i++)
            acc[i] += out[i];
        printf("[baseline] scan %d/%d done\n", s + 1, BASELINE_SAMPLES);
    }

    hackrf_close_device(dev);

    // Validate — if all zero the device didn't deliver samples
    float total = 0.0f;
    for (int i = 0; i < TOTAL_CHANNELS; i++) total += acc[i];
    if (total == 0.0f) {
        fprintf(stderr, "[!] Baseline got only zero readings — "
                "check HackRF connection.\n");
        return false;
    }

    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        channels[i].baseline = acc[i] / BASELINE_SAMPLES;
        printf("[baseline] Ch%2d (%.0f MHz): %.2f dB\n",
               channels[i].number, channels[i].frequency,
               power_to_db(channels[i].baseline));
    }
    printf("[baseline] Done.\n\n");
    return true;
}

// ─── Phase 2: Continuous monitor ─────────────────────────────────────────────

// Returns the index of the most-jammed channel, or -1 if nothing detected.
int detect_jamming(void) {
    float  out[TOTAL_CHANNELS];
    int    jammed_ch  = -1;
    float  max_excess = 0.0f;

    scan_once(out);

    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        float db_now      = power_to_db(out[i]);
        float db_baseline = power_to_db(channels[i].baseline);
        float excess      = db_now - db_baseline;

        if (excess >= JAM_THRESHOLD_DB) {
            printf("[!!!] JAMMING on Ch%2d (%.0f MHz)  "
                   "baseline=%.1f dB  current=%.1f dB  excess=+%.1f dB\n",
                   channels[i].number, channels[i].frequency,
                   db_baseline, db_now, excess);
            if (excess > max_excess) {
                max_excess = excess;
                jammed_ch  = i;
            }
        }
    }
    return jammed_ch;
}

// ─── Phase 3: Triangulation ───────────────────────────────────────────────────

// Convert linear power reading to estimated distance using log-distance path loss.
//   RSSI_dB = ref_power_dB - 10*n*log10(d / d0)
//   => d = d0 * 10^((ref_power_dB - RSSI_dB) / (10*n))
//
// ref_power is taken as the strongest reading among collected samples
// (closest position to the jammer).
static double power_to_distance(float power, float ref_power) {
    double db_diff = power_to_db(ref_power) - power_to_db(power);
    return REF_DIST_M * pow(10.0, db_diff / (10.0 * PATH_LOSS_EXP));
}

// Weighted least-squares trilateration.
// Each sample contributes a circle: (x-xi)^2 + (y-yi)^2 = di^2
// Linearise by subtracting the last equation from all others → Ax = b.
void trilaterate(void) {
    if (sample_count < 3) {
        printf("[tri] Need at least 3 position samples (have %d).\n",
               sample_count);
        return;
    }

    // Find strongest reading to use as reference
    float ref_power = 0.0f;
    for (int i = 0; i < sample_count; i++)
        if (samples[i].power > ref_power)
            ref_power = samples[i].power;

    int    n  = sample_count;
    int    n1 = n - 1;
    double xn = samples[n1].x, yn = samples[n1].y;
    double dn = power_to_distance(samples[n1].power, ref_power);

    // Build A (n1 x 2) and b (n1 x 1)
    double A[MAX_POSITIONS][2], b[MAX_POSITIONS];

    for (int i = 0; i < n1; i++) {
        double xi = samples[i].x, yi = samples[i].y;
        double di = power_to_distance(samples[i].power, ref_power);

        A[i][0] = 2.0 * (xi - xn);
        A[i][1] = 2.0 * (yi - yn);
        b[i]    = xi*xi - xn*xn + yi*yi - yn*yn + dn*dn - di*di;
    }

    // Normal equations: (A^T A) x = A^T b  →  solve 2x2 system
    double ATA[2][2] = {{0}}, ATb[2] = {0};
    for (int i = 0; i < n1; i++) {
        ATA[0][0] += A[i][0] * A[i][0];
        ATA[0][1] += A[i][0] * A[i][1];
        ATA[1][0] += A[i][1] * A[i][0];
        ATA[1][1] += A[i][1] * A[i][1];
        ATb[0]    += A[i][0] * b[i];
        ATb[1]    += A[i][1] * b[i];
    }

    double det = ATA[0][0] * ATA[1][1] - ATA[0][1] * ATA[1][0];
    if (fabs(det) < 1e-9) {
        printf("[tri] Positions are collinear — cannot triangulate.\n");
        return;
    }

    double est_x = (ATb[0] * ATA[1][1] - ATb[1] * ATA[0][1]) / det;
    double est_y = (ATA[0][0] * ATb[1] - ATA[1][0] * ATb[0])  / det;

    printf("\n[tri] Estimated jammer position: (%.2f m, %.2f m)\n",
           est_x, est_y);
    printf("[tri] Distances from each sample point:\n");
    for (int i = 0; i < sample_count; i++) {
        double d = power_to_distance(samples[i].power, ref_power);
        printf("       position (%5.1f, %5.1f) → %.2f m  "
               "(power %.1f dB)\n",
               samples[i].x, samples[i].y,
               d, power_to_db(samples[i].power));
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(void) {
    signal(SIGINT, handle_sigint);
    init_channels();

    // ── Phase 1: baseline ──
    if (!build_baseline()) return 1;

    // ── Phase 2: monitor until jamming found ──
    printf("[monitor] Scanning continuously. Press Ctrl+C to stop.\n");
    int jammed_ch = -1;
    while (running && jammed_ch == -1) {
        jammed_ch = detect_jamming();
        if (jammed_ch == -1)
            printf("[monitor] No jamming detected.\n");
        sleep(1);
    }

    if (!running || jammed_ch == -1) {
        printf("[*] Stopped.\n");
        return 0;
    }

    // ── Phase 3: triangulation ──
    printf("\n[tri] Jammer found on Ch%d (%.0f MHz).\n",
           channels[jammed_ch].number, channels[jammed_ch].frequency);
    printf("[tri] Move the HackRF to different positions and record readings.\n");
    printf("[tri] Enter at least 3 positions. Type 'done' when finished.\n\n");

    char input[64];
    while (sample_count < MAX_POSITIONS) {
        printf("[tri] Position %d — enter x y (metres, space-separated) "
               "or 'done': ", sample_count + 1);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        if (strncmp(input, "done", 4) == 0) break;

        double x, y;
        if (sscanf(input, "%lf %lf", &x, &y) != 2) {
            printf("[tri] Invalid input, try again.\n");
            continue;
        }

        printf("[tri] Scanning at (%.1f, %.1f)...\n", x, y);
        float power = scan_channel(jammed_ch);

        samples[sample_count].x     = x;
        samples[sample_count].y     = y;
        samples[sample_count].power = power;
        sample_count++;

        printf("[tri] Recorded power: %.1f dB\n\n", power_to_db(power));
    }

    trilaterate();

    printf("[*] Done.\n");
    return 0;
}

// RF Jammer Detector
//
// Uses hackrf_sweep for continuous spectrum monitoring across 2.4GHz and 5GHz.
// Builds a rolling baseline, then watches for sustained power spikes.
// When jamming is confirmed it switches the AP to the cleanest channel.
//
// Requires: hackrf_sweep (in PATH), iw
// Compile:  gcc -o rf_jammer rf_jammer.c -lm
// Run:      sudo ./rf_jammer

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

// ─── Config ───────────────────────────────────────────────────────────────────

#define AP_IFACE          "wlan1"    // AP interface to hop channels on
#define SWEEP_ARGS        "hackrf_sweep -f 2400:5850 -w 200000 -l 40 -g 32"
#define JAM_THRESHOLD_DB  10.0f     // dB above baseline = jamming
#define BASELINE_SWEEPS   20        // full sweeps averaged for baseline
#define CONFIRM_COUNT     3         // consecutive detections before acting
#define CHAN_BW_MHZ       10.0f     // ±MHz around channel centre to accumulate

// ─── Channel table ────────────────────────────────────────────────────────────

#define TOTAL_CHANNELS 39

static struct channel {
    int   number;
    float freq_mhz;    // centre frequency
    float baseline;    // rolling mean power (dBFS)
    float current;     // latest mean power (dBFS)
    int   bin_count;   // bins accumulated this sweep (internal)
    float bin_sum;     // power sum this sweep (internal)
} channels[TOTAL_CHANNELS];

static const int ch5ghz[] = {
    36,40,44,48,
    52,56,60,64,
    100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165
};

static void init_channels(void) {
    for (int i = 0; i < 14; i++) {
        channels[i].number   = i + 1;
        channels[i].freq_mhz = (i < 13) ? 2412.0f + i * 5.0f : 2484.0f;
    }
    int n5 = (int)(sizeof(ch5ghz) / sizeof(ch5ghz[0]));
    for (int i = 0; i < n5; i++) {
        channels[14 + i].number   = ch5ghz[i];
        channels[14 + i].freq_mhz = 5000.0f + ch5ghz[i] * 5.0f;
    }
}

// ─── Globals ─────────────────────────────────────────────────────────────────

static volatile int running = 1;
static void handle_sigint(int s) { (void)s; running = 0; }

// ─── Channel helpers ──────────────────────────────────────────────────────────

// Map a frequency (MHz) to a channel index, or -1 if it doesn't fit.
static int freq_to_ch_idx(float freq_mhz) {
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        if (fabsf(freq_mhz - channels[i].freq_mhz) <= CHAN_BW_MHZ)
            return i;
    }
    return -1;
}

static int get_cleanest_channel(void) {
    int   best = 0;
    float lo   = channels[0].baseline;
    for (int i = 1; i < TOTAL_CHANNELS; i++)
        if (channels[i].baseline > 0.0f && channels[i].baseline < lo) {
            lo = channels[i].baseline;
            best = i;
        }
    return channels[best].number;
}

static void switch_channel(int ch_num) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "iw dev %s set channel %d", AP_IFACE, ch_num);
    printf("[*] Switching %s to channel %d\n", AP_IFACE, ch_num);
    system(cmd);
}

// ─── hackrf_sweep parser ──────────────────────────────────────────────────────

// hackrf_sweep CSV line:
//   date, time, hz_low, hz_high, hz_bin_width, num_samples, dB0, dB1, ...
//
// Returns true when a complete sweep (all channels updated) finishes.
// We detect sweep boundaries by watching for hz_low to wrap back to 2.4GHz
// after having seen 5GHz lines.

static bool parse_line(const char *line) {
    long long hz_low, hz_high;
    double    hz_step;
    int       num_samples;
    // Skip date and time fields
    const char *p = strchr(line, ',');  // skip date
    if (!p) return false;
    p++;
    p = strchr(p, ',');                 // skip time
    if (!p) return false;
    p++;

    if (sscanf(p, " %lld, %lld, %lf, %d,",
               &hz_low, &hz_high, &hz_step, &num_samples) != 4)
        return false;

    // Advance past the 4 numeric fields to the dB values
    for (int i = 0; i < 4; i++) {
        p = strchr(p, ',');
        if (!p) return false;
        p++;
    }

    // Accumulate each dB bin into whichever channel it belongs to
    float bin_freq_mhz = hz_low / 1e6f + (hz_step / 1e6f) / 2.0f;
    char *end;
    while (*p) {
        double db = strtod(p, &end);
        if (end == p) break;
        p = end;
        if (*p == ',') p++;

        int idx = freq_to_ch_idx(bin_freq_mhz);
        if (idx >= 0) {
            channels[idx].bin_sum   += (float)db;
            channels[idx].bin_count++;
        }
        bin_freq_mhz += hz_step / 1e6f;
    }

    return true;
}

// Flush accumulated bins into channels[].current, reset for next sweep.
static void flush_sweep(void) {
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        if (channels[i].bin_count > 0) {
            channels[i].current   = channels[i].bin_sum / channels[i].bin_count;
            channels[i].bin_sum   = 0.0f;
            channels[i].bin_count = 0;
        }
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(void) {
    signal(SIGINT, handle_sigint);
    init_channels();

    printf("[*] Starting hackrf_sweep...\n");
    FILE *sw = popen(SWEEP_ARGS, "r");
    if (!sw) { perror("[!] popen hackrf_sweep"); return 1; }

    char   line[4096];
    int    sweep_count    = 0;
    bool   baseline_done  = false;
    int    confirm        = 0;        // consecutive jamming detections
    long long prev_hz_low = -1;

    printf("[baseline] Collecting %d sweeps...\n", BASELINE_SWEEPS);

    while (running && fgets(line, sizeof(line), sw)) {
        // Detect start of a new sweep by hz_low going back to ~2.4GHz
        long long hz_low = -1;
        const char *p = strchr(line, ',');
        if (p) { p++; p = strchr(p, ','); }
        if (p) sscanf(p + 1, " %lld", &hz_low);

        bool new_sweep = (prev_hz_low >= 0 && hz_low < prev_hz_low);
        prev_hz_low    = hz_low;

        if (new_sweep) {
            flush_sweep();
            sweep_count++;

            if (!baseline_done) {
                // Accumulate into baseline
                for (int i = 0; i < TOTAL_CHANNELS; i++)
                    channels[i].baseline =
                        ((channels[i].baseline * (sweep_count - 1)) +
                          channels[i].current) / sweep_count;

                printf("[baseline] sweep %d/%d\r", sweep_count, BASELINE_SWEEPS);
                fflush(stdout);

                if (sweep_count >= BASELINE_SWEEPS) {
                    baseline_done = true;
                    printf("\n[baseline] Done.\n");
                    for (int i = 0; i < TOTAL_CHANNELS; i++)
                        printf("  Ch%3d (%7.1f MHz): %.1f dBFS\n",
                               channels[i].number, channels[i].freq_mhz,
                               channels[i].baseline);
                    printf("[monitor] Watching for jamming (threshold +%.0f dB)...\n",
                           JAM_THRESHOLD_DB);
                }
                continue;
            }

            // Update rolling baseline slowly (10% weight on new data)
            for (int i = 0; i < TOTAL_CHANNELS; i++)
                if (channels[i].current != 0.0f)
                    channels[i].baseline =
                        channels[i].baseline * 0.9f +
                        channels[i].current  * 0.1f;

            // Check for jamming
            int    jammed_idx = -1;
            float  max_excess = 0.0f;

            for (int i = 0; i < TOTAL_CHANNELS; i++) {
                float excess = channels[i].current - channels[i].baseline;
                if (excess >= JAM_THRESHOLD_DB) {
                    printf("[!!!] JAMMING Ch%3d (%.0f MHz)  "
                           "baseline=%.1f  current=%.1f  excess=+%.1f dB\n",
                           channels[i].number, channels[i].freq_mhz,
                           channels[i].baseline, channels[i].current, excess);
                    if (excess > max_excess) {
                        max_excess = excess;
                        jammed_idx = i;
                    }
                }
            }

            if (jammed_idx >= 0) {
                confirm++;
                if (confirm >= CONFIRM_COUNT) {
                    int clean = get_cleanest_channel();
                    printf("[*] Confirmed jamming on Ch%d — hopping to Ch%d\n",
                           channels[jammed_idx].number, clean);
                    switch_channel(clean);
                    confirm = 0;

                    // Re-baseline after hop
                    printf("[baseline] Re-baselining after channel hop...\n");
                    baseline_done = false;
                    sweep_count   = 0;
                    for (int i = 0; i < TOTAL_CHANNELS; i++)
                        channels[i].baseline = 0.0f;
                }
            } else {
                confirm = 0;
            }
        }

        parse_line(line);
    }

    pclose(sw);
    printf("[*] Stopped.\n");
    return 0;
}

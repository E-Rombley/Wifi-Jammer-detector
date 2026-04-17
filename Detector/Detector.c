// SUMMARY
// The SDR actively scans the 2.4GHz spectrum to find clean channels.
// When a deauth attack is detected (50+ deauth frames from same MAC in 1 second),
// it switches wlan1 to the cleanest available channel.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include <libhackrf/hackrf.h>

// ─── Constants ───────────────────────────────────────────────────────────────

#define MAX_FRAMES       255
#define TOTAL_CHANNELS   14
#define DEAUTH_THRESHOLD 50       // frames from same MAC within TIME_WINDOW = attack
#define TIME_WINDOW      1        // seconds
#define SAMPLE_RATE      20000000 // 20 MHz
#define LNA_GAIN         40
#define VGA_GAIN         32

// ─── Structs ─────────────────────────────────────────────────────────────────

struct frames {
    uint8_t  mac[6];
    time_t   first_seen;
    int      counter;
};

struct channel {
    int   channel_number;
    float frequency;   // MHz
    float noise_floor; // measured power (lower = cleaner)
};

// ─── Globals ─────────────────────────────────────────────────────────────────

struct frames  frame[MAX_FRAMES];
struct channel channels[TOTAL_CHANNELS];
int frame_count = 0;

// ─── Channel Utilities ───────────────────────────────────────────────────────

// Populate channel list with standard 2.4GHz Wi-Fi channels
void init_channels(void) {
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        channels[i].channel_number = i + 1;
        channels[i].frequency      = (i < 13) ? 2412.0f + i * 5.0f : 2484.0f;  // ch14 is +12 MHz from ch13
        channels[i].noise_floor    = 0.0f;
    }
}

// Return the channel number with the lowest measured signal strength
int get_cleanest_channel(void) {
    int   best_channel = 1;
    float lowest_sig   = channels[0].noise_floor;

    for (int i = 1; i < TOTAL_CHANNELS; i++) {
        if (channels[i].noise_floor < lowest_sig) {
            lowest_sig   = channels[i].noise_floor;
            best_channel = channels[i].channel_number;
        }
    }
    return best_channel;
}

// Switch wlan1 to a given channel number
void switch_channel(int channel_num) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "iw wlan1 set channel %d", channel_num);
    printf("[*] Switching to channel %d (%.0f MHz)...\n",
           channel_num, channels[channel_num - 1].frequency);
    system(cmd);
}

// ─── HackRF RX Callback ──────────────────────────────────────────────────────

int current_channel_idx = 0;  // set by scan_spectrum before each hackrf_start_rx

int rx_callback(hackrf_transfer *transfer) {

    int8_t  *samples = (int8_t *)transfer->buffer;
    int      len     = transfer->valid_length;
    double   power   = 0.0;

    // Compute mean power of IQ samples
    for (int i = 0; i + 1 < len; i += 2) {
        double I = samples[i];
        double Q = samples[i + 1];
        power += I * I + Q * Q;
    }
    power /= (len / 2);

    // Store power into the channel this callback was registered for
    channels[current_channel_idx].noise_floor = (float)power;

    return 0;  // 0 = keep receiving
}

// ─── HackRF Scan ─────────────────────────────────────────────────────────────

// Opens HackRF, tunes to each 2.4GHz channel, collects samples, then stops.
void scan_spectrum(void) {
    hackrf_device *device = NULL;
    int ret;

    ret = hackrf_init();
    if (ret != HACKRF_SUCCESS) {
        fprintf(stderr, "[!] hackrf_init failed: %s\n", hackrf_error_name(ret));
        return;
    }

    ret = hackrf_open(&device);
    if (ret != HACKRF_SUCCESS) {
        fprintf(stderr, "[!] hackrf_open failed: %s\n", hackrf_error_name(ret));
        hackrf_exit();
        return;
    }

    hackrf_set_sample_rate(device, SAMPLE_RATE);
    hackrf_set_amp_enable(device, 0);
    hackrf_set_lna_gain(device, LNA_GAIN);
    hackrf_set_vga_gain(device, VGA_GAIN);

    // Tune to each channel and collect a short burst of samples
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        uint64_t freq_hz = (uint64_t)(channels[i].frequency * 1e6);
        hackrf_set_freq(device, freq_hz);

        current_channel_idx = i;  // pin callback to the channel we just tuned
        hackrf_start_rx(device, rx_callback, NULL);
        // Collect for ~100ms per channel
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
        hackrf_stop_rx(device);

        printf("[scan] Channel %2d (%.0f MHz) — power: %.2f\n",
               channels[i].channel_number,
               channels[i].frequency,
               channels[i].noise_floor);
    }

    hackrf_close(device);
    hackrf_exit();
}

// ─── Deauth Detection ────────────────────────────────────────────────────────

// Call this with each incoming deauth frame's source MAC address.
// Returns true if an attack threshold is exceeded.
bool check_deauth(uint8_t *incoming_mac) {
    bool found_match  = false;
    int  matched_index = -1;

    // Search for existing MAC in our table
    for (int i = 0; i < frame_count; i++) {
        if (memcmp(frame[i].mac, incoming_mac, 6) == 0) {
            frame[i].counter++;
            found_match    = true;
            matched_index  = i;
            break;
        }
    }

    // New MAC — add it if there's space
    if (!found_match) {
        if (frame_count >= MAX_FRAMES) {
            fprintf(stderr, "[!] Frame table full, cannot track new MAC.\n");
            return false;
        }
        memcpy(frame[frame_count].mac, incoming_mac, 6);
        frame[frame_count].counter    = 1;
        frame[frame_count].first_seen = time(NULL);
        matched_index                 = frame_count;
        frame_count++;
    }

    // Check if threshold exceeded within time window
    time_t now          = time(NULL);
    time_t elapsed_time = now - frame[matched_index].first_seen;

    if (frame[matched_index].counter >= DEAUTH_THRESHOLD && elapsed_time <= TIME_WINDOW) {
        printf("[!!!] DEAUTH ATTACK DETECTED from MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               incoming_mac[0], incoming_mac[1], incoming_mac[2],
               incoming_mac[3], incoming_mac[4], incoming_mac[5]);
        // Reset counter so we don't keep triggering
        frame[matched_index].counter    = 0;
        frame[matched_index].first_seen = now;
        return true;
    }

    // Reset counter if outside the time window (stale data)
    if (elapsed_time > TIME_WINDOW) {
        frame[matched_index].counter    = 1;
        frame[matched_index].first_seen = now;
    }

    return false;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(void) {
    printf("[*] Initialising channel list...\n");
    init_channels();

    printf("[*] Scanning spectrum with HackRF...\n");
    scan_spectrum();

    // ── Demo: simulate a deauth attack from a fake MAC ──
    // In production this would come from a packet capture loop
    // (e.g. libpcap listening on a monitor-mode interface).
    printf("[*] Simulating deauth flood for demonstration...\n");
    uint8_t attacker_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

    for (int i = 0; i < 55; i++) {
        if (check_deauth(attacker_mac)) {
            // Attack confirmed — pick cleanest channel and hop
            int clean = get_cleanest_channel();
            switch_channel(clean);
            break;
        }
    }

    printf("[*] Done.\n");
    return 0;
}

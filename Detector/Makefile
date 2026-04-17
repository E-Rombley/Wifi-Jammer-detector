CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIBS    = -lhackrf -lpcap -lpthread -lm

all: detector rf_jammer

detector: Detector.c
	$(CC) $(CFLAGS) -o detector Detector.c $(LIBS)

rf_jammer: rf_jammer.c
	$(CC) $(CFLAGS) -o rf_jammer rf_jammer.c -lhackrf -lm

clean:
	rm -f detector rf_jammer

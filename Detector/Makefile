CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIBS    = -lhackrf -lpcap -lpthread -lm

detector: Detector.c
	$(CC) $(CFLAGS) -o detector Detector.c $(LIBS)

clean:
	rm -f detector

CC     = gcc
CFLAGS = -Wall -Wextra -O2
LIBS   = -lhackrf -lm

jammer: jammer.c
	$(CC) $(CFLAGS) -o jammer jammer.c $(LIBS)

clean:
	rm -f jammer

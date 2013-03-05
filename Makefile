
CFLAGS:=-Wall

# Uncomment this to include a timestamp in DCC replies. The standard mandates
# it, but JMRI doesn't like it
# CFLAGS:=$(CFLAGS) -DINCLUDETIME

all: gpiodcc

dcc.o: dcc.c dcc.h srcp.h
srcp.o: srcp.c srcp.h dcc.h

%.o: %.c
	gcc $(CFLAGS) -c $<

gpiodcc: dcc.o srcp.o
	gcc -o $@ $^ -lrt -lpthread

clean:
	rm -f gpiodcc *.o

.PHONY: all clean

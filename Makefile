CC = gcc
LD = gcc
RM = rm -f

CFLAGS = -Wall -O2 -std=c99 -pthread -I. -Isrc -Iutils
LDFLAGS = -pthread

SRCDIR = src
UTILDIR = utils

SRCS = $(SRCDIR)/miner.c $(SRCDIR)/stratum.c $(SRCDIR)/json.c \
       $(SRCDIR)/sha256.c $(SRCDIR)/yespower.c $(SRCDIR)/blake2b.c
OBJS = $(SRCS:.c=.o)
TARGET = power2b-miner

# ARM 32-bit optimizations
ARM_CFLAGS = -march=armv7-a -mfpu=neon -mfloat-abi=hard -O3
ARM_TARGET = power2b-miner-arm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(OBJS) $(LDFLAGS) -o $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

arm: $(SRCS)
	$(CC) $(CFLAGS) $(ARM_CFLAGS) $(SRCS) $(LDFLAGS) -o $(ARM_TARGET)

clean:
	$(RM) $(OBJS) $(TARGET) $(ARM_TARGET)
	$(RM) $(SRCDIR)/*.o

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all arm clean install

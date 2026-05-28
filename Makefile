CC = gcc
LD = gcc
RM = rm -f

ARCH := $(shell uname -m)
SRCDIR = src

SRCS = $(SRCDIR)/miner.c $(SRCDIR)/stratum.c $(SRCDIR)/json.c \
       $(SRCDIR)/sha256.c $(SRCDIR)/yespower.c $(SRCDIR)/blake2b.c
OBJS = $(SRCS:.c=.o)
TARGET = power2b-miner

CFLAGS = -std=c99 -pthread -I. -Isrc -Iutils

# Auto-detected architecture optimizations
ifneq (,$(filter aarch64 arm64,$(ARCH)))
  CFLAGS += -O3 -funroll-loops -march=armv8-a+simd -mtune=cortex-a75
  TARGET := $(TARGET)-aarch64
else ifneq (,$(filter armv7l armv8l,$(ARCH)))
  CFLAGS += -O3 -funroll-loops -march=armv7-a -mfpu=neon -mfloat-abi=hard
  TARGET := $(TARGET)-arm32
else ifneq (,$(filter x86_64 amd64,$(ARCH)))
  CFLAGS += -O3 -funroll-loops -march=native
else
  CFLAGS += -O3 -funroll-loops
endif

LDFLAGS = -pthread

all: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)
	@echo "=== Built: $(TARGET) on $(ARCH) ==="

clean:
	$(RM) $(OBJS) $(SRCDIR)/*.o power2b-miner power2b-miner-arm power2b-miner-aarch64 power2b-miner-arm32

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all clean install

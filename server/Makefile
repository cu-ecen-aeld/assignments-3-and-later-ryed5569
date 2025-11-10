# Cross-compile friendly Makefile for aesdsocket
# Usage examples:
#   make                      # native build
#   make CROSS_COMPILE=aarch64-none-linux-gnu-  # cross build
#   make clean

ifeq ($(CROSS_COMPILE),)
CC ?= gcc
else
CC := $(CROSS_COMPILE)gcc
endif

CFLAGS ?= -Wall -Wextra -O2 -g -DUSE_AESD_CHAR_DEVICE=1
LDFLAGS ?=
TARGET := aesdsocket
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)

.PHONY: all default clean

all: $(TARGET)
default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

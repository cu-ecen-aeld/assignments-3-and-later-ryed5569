# Cross-compile friendly Makefile for aesdsocket

# Toolchain
CC ?= $(CROSS_COMPILE)gcc

# Put include paths and -D defines in CPPFLAGS; append so OE flags are kept
CPPFLAGS += -DUSE_AESD_CHAR_DEVICE=1 \
            -I$(CURDIR)/aesd-char-driver \
            -I$(CURDIR)/../aesd-char-driver

# Add warnings/opts to CFLAGS; append so OE flags are kept
CFLAGS   += -Wall -Wextra -O2 -g

# Keep LDFLAGS for OE; put libs in LDLIBS
LDLIBS   += -pthread

TARGET := aesdsocket
SRCS   := aesdsocket.c
OBJS   := $(SRCS:.c=.o)

.PHONY: all default clean

all: $(TARGET)
default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
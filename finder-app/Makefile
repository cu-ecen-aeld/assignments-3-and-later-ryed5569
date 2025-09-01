# finder-app/Makefile

TARGET := writer
SRCS   := writer.c
OBJS   := $(SRCS:.c=.o)

# If CROSS_COMPILE is unset, CC becomes 'gcc'.
# If set to 'aarch64-none-linux-gnu-', CC becomes 'aarch64-none-linux-gnu-gcc'.
CC     := $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -O2 -std=c11
LDFLAGS?=

.PHONY: all clean
all: $(TARGET)            # default target

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

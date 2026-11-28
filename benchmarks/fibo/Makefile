CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?= -lm

TARGET = fibo
SRCS = fibo.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean

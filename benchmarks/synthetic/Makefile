CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -pthread
LDFLAGS ?= -pthread -lm

TARGET = synthetic
SRCS = synthetic.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu99
TARGET = proxy
SRCS = proxy.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf cache

.PHONY: all clean
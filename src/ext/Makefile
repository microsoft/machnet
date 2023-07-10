CC = gcc
CFLAGS = -Wall -fPIC
LDFLAGS = -shared
LIBS = -luuid
TARGET = libnsaas_shim.so
SRCS = nsaas.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

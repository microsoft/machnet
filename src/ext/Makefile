CC = gcc
CFLAGS = -Wall -fPIC
LDFLAGS = -shared
LIBS = -luuid
TARGET = libmachnet_shim.so
SRCS = machnet.c
INC = ../include
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) -I$(INC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

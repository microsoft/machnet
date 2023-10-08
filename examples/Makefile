CC = g++
CFLAGS = -I../src/ext -L../ -lmachnet_shim -lrt -lgflags -Wl,-rpath,..
LIBS = $(LIBDPDK_LIBRARIES)

all: hello_world

hello_world: hello_world.cc
	$(CC) -o $@ $< $(CFLAGS) $(LIBS)

clean:
	rm -f hello_world

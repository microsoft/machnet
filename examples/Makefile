CC = g++
CFLAGS = -I../src/ext -L../ -lmachnet_shim -lrt -lgflags -Wl,-rpath,../build/src/ext
LIBS = $(LIBDPDK_LIBRARIES)

all: hello_world

hello_world: hello_world.cc
	$(CC) -o $@ $< $(CFLAGS) $(LIBS)

clean:
	rm -f hello_world


CC = g++
CFLAGS = -I../src/ext -L../ -lmachnet_shim -lrt -lgflags -Wl,-rpath,..

all: hello_world

hello_world: hello_world.cc
	$(CC) -o $@ $< $(CFLAGS)

clean:
	rm -f hello_world

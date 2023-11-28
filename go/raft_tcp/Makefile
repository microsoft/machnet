# Makefile

# Go parameters
GOCMD=go
GOBUILD=$(GOCMD) build
GOCLEAN=$(GOCMD) clean

# Binary names
SERVER_BINARY_NAME=server
CLIENT_BINARY_NAME=client

# Build directories
SERVER_BUILD_DIR=.
CLIENT_BUILD_DIR=./cmd/client.go

all: server client

server:
	$(GOBUILD) -o $(SERVER_BINARY_NAME) $(SERVER_BUILD_DIR)

client:
	$(GOBUILD) -o $(CLIENT_BINARY_NAME) $(CLIENT_BUILD_DIR)

clean:
	$(GOCLEAN)
	rm -f $(SERVER_BINARY_NAME)
	rm -f $(CLIENT_BINARY_NAME)
	rm -rf data

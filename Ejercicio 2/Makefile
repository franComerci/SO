CC=gcc
CFLAGS=-Wall -Werror -std=c99

all: server client

server: src/server.c
	$(CC) $(CFLAGS) src/server.c -o server

client: src/client.c
	$(CC) $(CFLAGS) src/client.c -o client

clean:
	rm -f server client

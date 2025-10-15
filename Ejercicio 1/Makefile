# Elige compilador automáticamente: clang -> gcc -> cc
CC ?= $(shell command -v clang 2>/dev/null || command -v gcc 2>/dev/null || echo cc)

# Flags robustas + fuentes GNU/POSIX
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c11 -O2 -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
SRC    = src/main.c src/ipc.c
OUT    = tp1

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDFLAGS = -pthread -lrt
else
  LDFLAGS =
endif

.PHONY: all build clean run

all: build

build:
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

run: build
	./$(OUT)

clean:
	rm -f $(OUT)

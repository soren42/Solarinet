# Host-side unit tests for the firmware's pure-C state machines.
# Nothing here participates in the UF2 build — plain gcc, no toolchain, no SDK.
#
#   make        build and run
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g

SRC = panelLinkTest.c ../panelLink.c ../../protocol.c
BIN = panelLinkTest

.PHONY: all clean
all: $(BIN)
	./$(BIN)

$(BIN): $(SRC) ../panelLink.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f $(BIN)

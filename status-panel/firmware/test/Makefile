# Host-side unit tests for the firmware's pure-C state machines.
# Nothing here participates in the UF2 build — plain gcc, no toolchain, no SDK.
#
#   make        build and run every suite
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g

LINK_SRC = panelLinkTest.c ../panelLink.c ../../protocol.c
CTL_SRC  = panelCtlTest.c ../panelCtl.c ../../protocol.c
BINS     = panelLinkTest panelCtlTest

.PHONY: all clean
all: $(BINS)
	./panelLinkTest
	./panelCtlTest

panelLinkTest: $(LINK_SRC) ../panelLink.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(LINK_SRC)

panelCtlTest: $(CTL_SRC) ../panelCtl.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(CTL_SRC)

clean:
	rm -f $(BINS)

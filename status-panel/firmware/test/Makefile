# Host-side unit tests for the firmware's pure-C state machines.
# Nothing here participates in the UF2 build — plain gcc, no toolchain, no SDK.
#
#   make        build and run every suite
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g

LINK_SRC = panelLinkTest.c ../panelLink.c ../../protocol.c
CTL_SRC  = panelCtlTest.c ../panelCtl.c ../../protocol.c
CFG_SRC  = panelScreenCfgTest.c ../panelScreenCfg.c ../../protocol.c
# A1 renderer under ASAN/UBSAN — the review-proven hubs[-2] class must crash
# loudly here, never ship (REVIEW-AW M3/M4). Needs the fb/env/screens units.
A1_SRC   = panelScreensATest.c ../panelScreensA.c ../panelFb.c ../panelHist.c ../panelFont.c ../../protocol.c
BINS     = panelLinkTest panelCtlTest panelScreenCfgTest panelScreensATest

.PHONY: all clean
all: $(BINS)
	./panelLinkTest
	./panelCtlTest
	./panelScreenCfgTest
	./panelScreensATest

panelLinkTest: $(LINK_SRC) ../panelLink.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(LINK_SRC)

panelCtlTest: $(CTL_SRC) ../panelCtl.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(CTL_SRC)

panelScreensATest: $(A1_SRC)
	$(CC) $(CFLAGS) -Wno-unused-parameter -I../.. -fsanitize=address,undefined -o $@ $(A1_SRC) -lm

panelScreenCfgTest: $(CFG_SRC) ../panelScreenCfg.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(CFG_SRC)

clean:
	rm -f $(BINS)

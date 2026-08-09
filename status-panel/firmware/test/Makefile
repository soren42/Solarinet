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
HELP_SRC = panelHelpTest.c ../panelHelp.c
# A1 renderer under ASAN/UBSAN — the review-proven hubs[-2] class must crash
# loudly here, never ship (REVIEW-AW M3/M4). Needs the fb/env/screens units.
A1_SRC   = panelScreensATest.c ../panelScreensA.c ../panelFb.c ../panelHist.c ../panelFont.c ../../protocol.c
# Framebuffer-assertion parity harness (PF1): renders all twelve REAL screens
# from fixtures/parity-fixture.json and asserts on lit pixels. It observes the
# renderers through ld --wrap rather than reimplementing any of them, so every
# framebuffer and text primitive has to be interceptable at link time.
PARITY_SRC  = panelParityTest.c testJson.c \
              ../panelScreensA.c ../panelScreensB.c ../panelScreensC.c \
              ../panelScreensD.c ../panelInlay.c ../panelHelpOverlay.c \
              ../panelFb.c ../panelHist.c ../panelFont.c ../../protocol.c
PARITY_WRAP = -Wl,--wrap=panelFbSet -Wl,--wrap=panelFbSetRgb \
              -Wl,--wrap=panelFbAdd -Wl,--wrap=panelFbDim \
              -Wl,--wrap=panelFbDimAll -Wl,--wrap=panelText \
              -Wl,--wrap=panelTextOver -Wl,--wrap=panelBig \
              -Wl,--wrap=panelScroll
BINS     = panelLinkTest panelCtlTest panelScreenCfgTest panelHelpTest panelScreensATest panelParityTest

.PHONY: all clean
all: $(BINS)
	./panelLinkTest
	./panelCtlTest
	./panelScreenCfgTest
	./panelHelpTest
	# LeakSanitizer cannot initialize when this suite runs under ptrace-based
	# harnesses; ASAN/UBSAN bounds checks remain enabled for this renderer test.
	ASAN_OPTIONS=detect_leaks=0 ./panelScreensATest
	# Golden framebuffers live in ../../fixtures/golden; the binary resolves
	# them relative to this directory, so run it from here.
	ASAN_OPTIONS=detect_leaks=0 ./panelParityTest

panelLinkTest: $(LINK_SRC) ../panelLink.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(LINK_SRC)

panelCtlTest: $(CTL_SRC) ../panelCtl.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(CTL_SRC)

panelScreensATest: $(A1_SRC)
	$(CC) $(CFLAGS) -Wno-unused-parameter -I../.. -fsanitize=address,undefined -o $@ $(A1_SRC) -lm

panelParityTest: $(PARITY_SRC) testJson.h ../panelScreens.h ../panelFb.h ../panelFont.h ../panelHelpOverlay.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -I../.. -fsanitize=address,undefined -o $@ $(PARITY_SRC) $(PARITY_WRAP) -lm

panelScreenCfgTest: $(CFG_SRC) ../panelScreenCfg.h ../../protocol.h
	$(CC) $(CFLAGS) -o $@ $(CFG_SRC)

panelHelpTest: $(HELP_SRC) ../panelHelp.h
	$(CC) $(CFLAGS) -o $@ $(HELP_SRC)

clean:
	rm -f $(BINS)

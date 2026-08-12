# Host-side provisioning tool (CONTRACT-SW §13). Plain C99 + termios; the
# only project dependency is the shared frame codec.
CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -O1 -g
# clock_gettime/CLOCK_MONOTONIC and termios need POSIX visibility under c99.
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

.PHONY: all smoke clean
all: panelProv fakePanel

panelProv: panelProv.c ../protocol.c ../protocol.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ panelProv.c ../protocol.c

# Test double: the real handler/store stack behind a pty (see smoke.sh).
fakePanel: fakePanel.c ../protocol.c ../firmware/panelProv.c ../firmware/panelProvStore.c ../firmware/panelProv.h ../firmware/panelProvStore.h ../protocol.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ fakePanel.c ../protocol.c ../firmware/panelProv.c ../firmware/panelProvStore.c

smoke: all
	./smoke.sh

clean:
	rm -f panelProv fakePanel

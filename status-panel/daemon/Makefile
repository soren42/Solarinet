CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Werror
CPPFLAGS += -I. -Ivendor -I..
LDLIBS += -lcurl

all: solariPanel

solariPanel: solariPanel.c ../protocol.c vendor/cJSON.c ../protocol.h vendor/cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ solariPanel.c ../protocol.c vendor/cJSON.c $(LDLIBS)

test: tests/codec_test
	./tests/codec_test

tests/codec_test: tests/codec_test.c solariPanel.c ../protocol.c ../protocol.h vendor/cJSON.c vendor/cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -DSOLARI_PANEL_TEST -o $@ tests/codec_test.c solariPanel.c ../protocol.c vendor/cJSON.c $(LDLIBS)

clean:
	rm -f solariPanel tests/codec_test

.PHONY: all test clean

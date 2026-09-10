CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Werror
CPPFLAGS += -I. -Ivendor -I..
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_FOUND := $(shell pkg-config --exists openssl && echo yes)
ifeq ($(OPENSSL_FOUND),)
$(error OpenSSL development files are required (pkg-config openssl))
endif
CPPFLAGS += $(OPENSSL_CFLAGS)
LDLIBS += -lcurl -lssl -lcrypto

all: solariPanel

solariPanel: solariPanel.c listener.c listener.h ../protocol.c vendor/cJSON.c ../protocol.h vendor/cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ solariPanel.c listener.c ../protocol.c vendor/cJSON.c $(LDLIBS)

test: tests/codec_test tests/listener_test
	./tests/codec_test
	./tests/listener_test.sh

tests/codec_test: tests/codec_test.c solariPanel.c listener.c listener.h ../protocol.c ../protocol.h vendor/cJSON.c vendor/cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -DSOLARI_PANEL_TEST -o $@ tests/codec_test.c solariPanel.c listener.c ../protocol.c vendor/cJSON.c $(LDLIBS)

tests/listener_test: tests/listener_test.c solariPanel.c listener.c listener.h ../protocol.c ../protocol.h vendor/cJSON.c vendor/cJSON.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -DSOLARI_PANEL_TEST -DWRITE_MS=300u -o $@ tests/listener_test.c solariPanel.c listener.c ../protocol.c vendor/cJSON.c $(LDLIBS)

clean:
	rm -f solariPanel tests/codec_test tests/listener_test

.PHONY: all test clean

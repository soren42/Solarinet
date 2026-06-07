# Convenience wrapper over CMake (§12.4) — muscle-memory targets.
# The real build logic lives in CMakeLists.txt; this just spares the fingers.
BUILD ?= build

.PHONY: all core spool client monitor server test cross-arm64 cross-arm32 windows clean

# Phase 1 core + tests is the default: zero external deps, always builds.
all: core

core:
	cmake -S . -B $(BUILD) -DSOLARI_BUILD_TESTS=ON && cmake --build $(BUILD)

# Core + the SQLite store-and-forward spool (links the platform libsqlite3).
spool:
	cmake -S . -B $(BUILD) -DSOLARI_BUILD_TESTS=ON -DSOLARI_WITH_SQLITE=ON && cmake --build $(BUILD)

client:
	cmake -S . -B $(BUILD) -DSOLARI_BUILD_SERVER=OFF -DSOLARI_WITH_IO=ON && cmake --build $(BUILD) --target solariClient

monitor:
	cmake -S . -B $(BUILD) -DSOLARI_BUILD_SERVER=OFF -DSOLARI_WITH_IO=ON && cmake --build $(BUILD) --target solariMonitor

server:
	cmake -S . -B $(BUILD) -DSOLARI_BUILD_SERVER=ON -DSOLARI_WITH_IO=ON && cmake --build $(BUILD) --target solariServer

cross-arm64:
	cmake -S . -B build-arm64 \
	  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake \
	  -DSOLARI_BUILD_SERVER=ON -DSOLARI_WITH_IO=ON && cmake --build build-arm64

cross-arm32:
	cmake -S . -B build-arm32 \
	  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm32.cmake \
	  -DSOLARI_BUILD_SERVER=OFF -DSOLARI_WITH_IO=ON && cmake --build build-arm32 --target solariClient solariMonitor

windows:
	cmake -S . -B build-win \
	  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-x86_64.cmake \
	  -DSOLARI_BUILD_SERVER=OFF -DSOLARI_WITH_IO=ON && cmake --build build-win --target solariClient

test:
	cmake -S . -B $(BUILD) -DSOLARI_BUILD_TESTS=ON && cmake --build $(BUILD) && ctest --test-dir $(BUILD) --output-on-failure

clean:
	rm -rf build build-*

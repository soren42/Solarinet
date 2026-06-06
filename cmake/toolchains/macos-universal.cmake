# Native macOS build producing a universal (arm64 + x86_64) binary.
# Intended to run on a macOS host with Xcode command-line tools.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS universal slices")
# CMAKE_OSX_DEPLOYMENT_TARGET may be set by the operator as needed, e.g.:
# set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")

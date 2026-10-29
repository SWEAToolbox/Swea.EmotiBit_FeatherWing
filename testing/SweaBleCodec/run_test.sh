#!/bin/bash
# Host unit test for SweaBleCodec.h with g++ (Linux/macOS/MinGW).
set -e
cd "$(dirname "$0")"
mkdir -p build
g++ -std=c++17 -Wall -Wextra -o build/test_codec test_codec.cpp
./build/test_codec

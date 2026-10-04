#!/bin/sh
# host build of the PDF engine for testing: sh tools/pdftest/build.sh  ->  tools/pdftest/pdftest.exe
cd "$(dirname "$0")/../.." || exit 1
K=src/kernel
mkdir -p build/host
FL="-std=c++20 -O2 -g -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-class-memaccess -fno-exceptions -fno-rtti -I$K"
g++ $FL -c $K/mm/lib.cpp -o build/host/lib.o -Dmemcpy=lib_memcpy -Dmemmove=lib_memmove -Dmemset=lib_memset -Dmemcmp=lib_memcmp -Datexit=lib_atexit 2>&1 | head -30
g++ $FL -Wl,--allow-multiple-definition -o tools/pdftest/pdftest.exe $K/pdf/*.cpp $K/gfx/imgdec.cpp $K/text.cpp tools/pdftest/shim.cpp tools/pdftest/host_main.cpp tools/pdftest/stubs.cpp build/host/lib.o 2>&1 | head -${LINES_MAX:-60}

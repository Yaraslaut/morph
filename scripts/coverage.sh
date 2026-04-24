#!/usr/bin/env bash
# Usage: bash scripts/coverage.sh
# Run from repo root after: cmake --build --preset coverage && ctest --preset coverage
set -euo pipefail

OUT="build/coverage"
TEST_EXE="$OUT/tests/async_framework_tests"
MERGED="$OUT/merged.profdata"
REPORT_DIR="$OUT/html"

PROFILES=$(find "$OUT" -name "*.profraw" 2>/dev/null | tr '\n' ' ')
if [ -z "$PROFILES" ]; then
    echo "ERROR: No .profraw files found in $OUT." >&2
    echo "Did you set LLVM_PROFILE_FILE and run ctest --preset coverage?" >&2
    exit 1
fi

llvm-profdata-20 merge -sparse $PROFILES -o "$MERGED"

mkdir -p "$REPORT_DIR"
llvm-cov-20 show "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -format=html \
    -output-dir="$REPORT_DIR" \
    -ignore-filename-regex="vcpkg_installed|catch2"

echo "Coverage report: $REPORT_DIR/index.html"

llvm-cov-20 report "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -ignore-filename-regex="vcpkg_installed|catch2"

llvm-cov-20 export "$TEST_EXE" \
    -instr-profile="$MERGED" \
    -format=lcov \
    -ignore-filename-regex="vcpkg_installed|catch2" \
    > "$OUT/coverage.lcov"

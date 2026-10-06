#!/usr/bin/env bash
# Unit tests for the in-process diff/merge engine (src/diff.c).
# Runs the standalone difftest binary; exits non-zero on any failure.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

make -C "$HERE/.." test-diff

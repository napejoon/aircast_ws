#!/bin/sh
# Every test in the repo, one command -- the same three jobs .github/workflows/ci.yml
# runs, in the same way. A toolchain that is not installed is skipped with a line
# on stderr, never silently, because a local machine rarely has all three.
set -e

python -m pytest -q

if command -v cmake >/dev/null 2>&1; then
  cmake -S receiver -B receiver/build -DCMAKE_C_FLAGS="-Wall -Werror"
  cmake --build receiver/build
  ./receiver/build/aircast-receiver --selftest
else
  echo "SKIP receiver/: cmake not on PATH" >&2
fi

if command -v flutter >/dev/null 2>&1; then
  (cd sender && flutter test)
else
  echo "SKIP sender/: flutter not on PATH" >&2
fi

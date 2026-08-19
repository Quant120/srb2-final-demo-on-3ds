#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/src"
exec make -f Makefile.3ds "$@"

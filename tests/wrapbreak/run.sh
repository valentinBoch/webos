#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -I src \
  tests/wrapbreak/test_wrap.c src/ui/wrapbreak.c -o "$tmp/test-wrap"
"$tmp/test-wrap"

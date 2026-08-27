#!/bin/sh
#
# Use the native unhare artifact to inspect executables built by the other
# jobs.  Linux and FreeBSD inputs must take the ELF section path; macOS inputs
# must take the format-independent whole-file fallback.

set -e

native=$1
artifacts=$2
tests=$3
prog="$artifacts/unhare-$native/unhare"

chmod +x "$prog"

for platform in freebsd linux macos; do
	[ "$platform" = "$native" ] && continue

	input="$artifacts/unhare-$platform/unhare"
	output="cross-output/$platform/unhare"
	log="cross-$platform-unhare.log"
	./"$prog" -v -o "$output" "$input" 2>"$log"
	if [ "$platform" = macos ]; then
		grep -q 'scanning whole file' "$log"
	else
		grep -q 'found \.bun section' "$log"
	fi
	diff -ru "$tests" "$output/tests"

	input="$artifacts/unhare-$platform/bun-native"
	[ -f "$input" ] || continue
	output="cross-output/$platform/bun"
	log="cross-$platform-bun.log"
	./"$prog" -v -o "$output" "$input" 2>"$log"
	if [ "$platform" = macos ]; then
		grep -q 'scanning whole file' "$log"
	else
		grep -q 'found \.bun section' "$log"
	fi
	extracted=$(find "$output" -type f)
	test "$(find "$output" -type f | wc -l | tr -d ' ')" = 1
	grep -q 'unhare real Bun fixture:' "$extracted"
	iconv -f UTF-8 -t UTF-8 "$extracted" >/dev/null
	if command -v node >/dev/null 2>&1; then
		node --check "$extracted"
	fi
done

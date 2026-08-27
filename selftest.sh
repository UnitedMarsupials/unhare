#!/bin/sh
#
# Check a freshly built unhare against the tests/ tree bundled into it.
#
# The program is asked to unpack itself (-t), and the result compared
# against the directory mkbunfs bundled -- so the comparison is with what
# went in, not with unhare's own idea of the answer.  The same bundle is
# then read back out of a file with no ELF structure around it, which is
# the only way to reach the fallback scan: once the section is found, the
# search never looks at the rest of the file.
#
# Which of the two paths ran is checked as well.  Both find this bundle,
# so a broken section lookup would otherwise sit unnoticed behind a
# fallback that quietly picks up the slack.

set -e

prog=${1:-./unhare}
mkbunfs=${2:-./mkbunfs}
tests=${3:-./tests}

[ -x "$prog" ] || { echo "$0: no such program: $prog" >&2; exit 1; }
[ -x "$mkbunfs" ] || { echo "$0: no such program: $mkbunfs" >&2; exit 1; }
[ -d "$tests" ] || { echo "$0: no such directory: $tests" >&2; exit 1; }

name=$(basename "$tests")
work=$(mktemp -d -t unhare-selftest)
trap 'rm -rf "$work"' EXIT HUP INT TERM

fail() {
	echo "self-test FAILED: $1" >&2
	exit 1
}

# The bundle in our own .bun section, reached through -t.
"$prog" -t -v -o "$work/self" 2>"$work/self.log" >/dev/null
grep -q 'found \.bun section' "$work/self.log" ||
    fail "-t did not find the .bun section (fell back to scanning?)"
diff -ru "$tests" "$work/self/$name" ||
    fail "-t output differs from $tests"

# The same bundle with no section to find, forcing the fallback scan.
"$mkbunfs" -f "$work/buried" "$tests"
"$prog" -v -o "$work/scan" "$work/buried" 2>"$work/scan.log" >/dev/null
grep -q 'scanning whole file' "$work/scan.log" ||
    fail "the buried payload was not read through the fallback scan"
diff -ru "$tests" "$work/scan/$name" ||
    fail "fallback scan output differs from $tests"

# An existing file must stop the whole run, and -f must let it through.
if "$prog" -t -o "$work/self" 2>"$work/clobber.log"; then
	fail "a second extraction into the same directory was allowed"
fi
grep -q 'already exists' "$work/clobber.log" ||
    fail "the refusal to overwrite did not say why"
"$prog" -t -f -o "$work/self" || fail "-f did not permit overwriting"
diff -ru "$tests" "$work/self/$name" ||
    fail "-f output differs from $tests"

# Nothing may be written when a collision is found, not even the files
# that come before it in the bundle.  The decoy stands in for the last
# file of the tree, so anything written at all would show up beside it.
last=$(cd "$tests" && find . -type f | sort | tail -1)
mkdir -p "$work/partial/$name/$(dirname "$last")"
: > "$work/partial/$name/$last"
"$prog" -t -o "$work/partial" 2>/dev/null &&
    fail "extraction over an existing $last was allowed"
[ "$(find "$work/partial" -type f | wc -l | tr -d ' ')" = 1 ] ||
    fail "a refused run left files behind"

# A symbolic link in the way must stop the write, not be followed
# through: the refusal of ".." in a name is worth nothing if a link
# already sitting in the output tree can carry the file elsewhere.
mkdir -p "$work/link/$name/$(dirname "$last")"
printf 'untouched\n' > "$work/decoy-target"
ln -s "$work/decoy-target" "$work/link/$name/$last"
"$prog" -t -f -o "$work/link" 2>/dev/null &&
    fail "wrote through a symbolic link"
[ "$(cat "$work/decoy-target")" = untouched ] ||
    fail "a symbolic link was followed out of the output directory"

# The same for a symlinked directory along the way.
mkdir -p "$work/linkdir/$name" "$work/outside"
ln -s "$work/outside" "$work/linkdir/$name/$(dirname "$last")"
"$prog" -t -o "$work/linkdir" 2>/dev/null &&
    fail "descended through a symlinked directory"
[ "$(find "$work/outside" -type f | wc -l | tr -d ' ')" = 0 ] ||
    fail "a symlinked directory carried the write outside"

# -l lists what would be written and touches nothing: the same names as
# a real run produces, and no directory left behind.
"$prog" -t -l -o "$work/listed" > "$work/listed.txt" ||
    fail "-l failed"
[ -e "$work/listed" ] &&
    fail "-l created the output directory"
"$prog" -t -o "$work/real" >/dev/null || fail "extraction failed"
(cd "$work/real" && find . -type f | sed 's|^\./||' | sort) > "$work/real.txt"
sed "s|^$work/listed/||" "$work/listed.txt" | sort > "$work/names.txt"
cmp -s "$work/real.txt" "$work/names.txt" ||
    fail "-l listed names a real run did not produce"

# The size -l reports is the size the file would have on disk, which for
# a UTF-16 module is not the size it occupies in the bundle.
"$prog" -t -l -v -o "$work/sz" 2>/dev/null |
    sed -E "s|^$work/sz/||; s| \(([0-9]+) bytes[^)]*\).*$| \1|" |
    sort > "$work/sz-said.txt"
"$prog" -t -o "$work/szreal" >/dev/null || fail "extraction failed"
(cd "$work/szreal" && find . -type f | sed 's|^\./||' | sort |
    while read -r p; do echo "$p $(stat -f %z "$p")"; done) > "$work/sz-real.txt"
cmp -s "$work/sz-said.txt" "$work/sz-real.txt" ||
    fail "-l reported sizes that a real run did not produce"

# A -o pattern names one place per bundle.  This program carries a
# single bundle, so the pattern must still work and must still land
# somewhere derived from the section it came from.
"$prog" -t -l -o 'pat%u' > "$work/pat.txt" || fail "-o pattern rejected"
grep -q '^pat[0-9][0-9]*/' "$work/pat.txt" ||
    fail "-o pattern did not spell out the section number"

# A pattern that could not tell two bundles apart is refused only when
# there are two; with one, it is nobody's business.
"$prog" -t -l -o "$work/plain" >/dev/null || fail "-o without a conversion refused for a single bundle"

# Conversions that an unsigned argument cannot answer are refused.
for bad in 'x%s' 'x%n' 'x%'; do
	"$prog" -t -l -o "$bad" >/dev/null 2>&1 &&
	    fail "-o accepted $bad"
done

# Asking for a section that is not here fails rather than silently
# unpacking something else.
"$prog" -t -l -s 4294967295 -o 'x%u' >/dev/null 2>&1 &&
    fail "-s accepted a section that is not here"

echo "self-test passed: $(find "$tests" -type f | wc -l | tr -d ' ')" \
    "files match; overwrite and symlink refusals hold"

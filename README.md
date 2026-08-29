# unhare

`unhare` extracts the filesystem bundled into a [Bun](https://bun.sh)
standalone binary -- the kind produced by `bun build --compile` -- writing
each embedded module out as a regular file.

The bundle is located by asking libelf for the `.bun` section of the
executable, so a payload that is not the last thing in the file is found
just as reliably as one appended to the end.  If no such section holds a
bundle -- a stripped binary, an older Bun release that merely
concatenated the payload, or a Mach-O or PE binary built for another
platform -- `unhare` scans the whole file for the bundle trailer
instead.  The search knows nothing of ELF, so a binary of any format
unpacks so long as the bundle is somewhere in it.

## Building

```sh
make
make test
make install
```

The `BSDmakefile` drives `bsd.prog.mk`, which does the heavy lifting.
The only dependency is libelf.  It comes from the base system on FreeBSD;
on Linux, install the distribution's development package.

macOS's `/usr/bin/make` is GNU make 3.81, so it reads `GNUmakefile` without
another make implementation being installed.  Apple supplies neither
libelf nor its headers.  After building libelf separately, pass its install
prefix to GNU make, for example `make LIBELF_PREFIX=/path/to/libelf`.
The macOS SDK does not declare `reallocarray(3)`, so `compat.h` supplies a
local overflow-checking implementation there.

The GitHub Actions macOS job installs Homebrew's prebuilt libelf bottle on
the disposable runner and passes its prefix to GNU make.  This is a CI
dependency only; building locally does not require Homebrew.

Installation goes under `PREFIX`, which defaults to `/usr/local` -- so
the program lands in `/usr/local/bin` and its manual page in
`/usr/local/man/man1`, rather than in the base system's `/usr/bin`.
`BINDIR` and `MANDIR` can be overridden individually; note that
`bsd.man.mk` appends the section digit to `MANDIR`, so it has to end in
`man`.

## Usage

```
usage: unhare [-flv] [-o pattern] [-s section] [-x pattern] file
       unhare -t [-flv] [-o pattern] [-s section] [-x pattern]
       unhare -h
```

- `-l` -- list the files that would be written, one to a line, and write
  none of them.  Nothing is created, not even the output directory, and
  an existing file in the way is neither an error nor worth remarking
  on.  The names are the ones a real run would produce, so what is
  listed is what would appear.  A single `-v` annotates them, the
  listing being the point already.
- `-o pattern` -- write the extracted files below the directory *pattern*
  names, creating it and any missing parents.  Defaults to the current
  directory.  A single `printf(3)` integer conversion is replaced by the
  index of the section a bundle came from, so `-o out%u` unpacks section
  29 into `out29`; a bundle found by scanning answers to zero.  Only
  integer conversions are allowed.  With more than one bundle to unpack,
  a pattern without a conversion is an error.
- `-s section` -- unpack only the bundle in that section; repeatable.
  The default is all of them.
- `-h` -- print the usage to stdout and exit successfully.  A malformed
  command line prints it to stderr instead and fails.  See "Exit
  status" below.
- `-f` -- overwrite files that are already there.  Without it, nothing is
  written at all when any target exists already, so a run that would
  clobber something leaves the directory as it found it rather than
  stopping partway through.
- `-t` -- unpack the bundle built into `unhare` itself, taking no `file`
  operand.  See "Self-test" below.
- `-v` -- write the name of each extracted file to standard output, one
  to a line, and report on standard error where the bundle was found and
  how much was written.  Given twice, annotate each name with the bytes
  written, loader, module format, encoding and side, and report the
  bundle header as well.  The listing goes to stdout and everything else
  to stderr, so a single `-v` leaves a list of names fit to pipe onward.
- `-x pattern` -- skip the modules whose names match the shell glob
  *pattern*; repeatable.  The rules are `tar --exclude`'s, matched with
  `fnmatch(3)` against the name the bundle records -- the name `-l`
  lists, less its `-o` directory.  A `*` spans the separators, so
  `*.node` reaches an entry however deeply it is buried, and each
  pattern is tried again at every component boundary, so `node_modules/*`
  catches a nested tree as readily as a top-level one.  A file in the way
  of an excluded module is in the way of nothing, and no longer stops the
  run.

See `unhare.1` for the full description.

## Exit status

Success exits 0; a failure exits with whichever code from
`<sysexits.h>` fits it, rather than a bare 1:

| code | | meaning |
| --- | ---: | --- |
| `EX_USAGE` | 64 | unknown option, or the wrong number of operands |
| `EX_DATAERR` | 65 | the file holds no bundle that can be made sense of |
| `EX_NOINPUT` | 66 | the file cannot be read at all |
| `EX_SOFTWARE` | 70 | the ELF library is too old to talk to |
| `EX_OSERR` | 71 | the system denied us memory, or an answer |
| `EX_CANTCREAT` | 73 | the output cannot be laid down, an existing file among the reasons |
| `EX_IOERR` | 74 | writing the output failed partway through |

## Self-test

```sh
make test
```

The `tests/` directory is bundled into `unhare` itself: `mkbunfs` walks
it at build time and emits `bunfs.c`, a byte array placed in a `.bun`
section by a section attribute, which the linker then puts in the
program.  `unhare -t` unpacks that bundle again -- reading the file
named by `argv[0]`, or, when that names no readable file, the one the
kernel reports for the process -- and `make test` compares the result
against the original `tests/`.  The comparison is therefore against
what went in, not against unhare's own idea of the answer.

Each directory is bundled under the Bun encoding it is named for, so
that one tree exercises all three:

| directory | bun encoding | stored as |
| --- | --- | --- |
| `US-ASCII` | latin1 | one byte per character |
| `UTF-16` | utf16 | converted to UTF-16LE, and back on the way out |
| anything else | binary | verbatim |

Binary is the right choice for a legacy eight-bit encoding such as
`KOI8-U`, whose bytes mean nothing to Bun and which must survive the
round trip untouched.

The files under `UTF-16` are themselves UTF-8 on disk; the conversion
happens on the way into the bundle and is undone on the way out.  A file
already in UTF-16 could not round-trip, since unhare's business is to
hand back decoded UTF-8 -- see "What a bundle does not record" below.

Running `unhare` on its own executable, with or without `-t`, therefore
unpacks that tree rather than reporting that nothing was found.  Note
that `unhare -t` with no `-o` unpacks into the current directory, which
in the source tree means `tests/` itself; the refusal to overwrite is
what stops that from quietly reverting an edited fixture.

---

## Notes on the format, and on other extractors

Everything below was established by testing against some real ELF
binaries; see "How this was verified".

### Why the Rust `unbun` fails on many binaries

The reference implementation, [`unbun`](https://github.com/ooojustin/unbun)
(crates: `unbun`, `unbun-cli`), locates the payload in
`format/mod.rs::find_trailer_in` by seeking to `end - 8192` and searching
**only the last 8 KiB** of the file for the `\n---- Bun! ----\n` magic.
It never consults the ELF structure at all.

That works when Bun's payload is the last thing in the file, but not when
a section header table, symbol table or debug information follows the
`.bun` section -- the trailer falls outside the window and the tool
reports "No Bun trailer found. Not a Bun standalone binary."

Some ELF binaries tested put ~56-59 KB of data after the payload, so
`unbun` fails on all of them:

| binary | `.bun` ends at | file size | trailing data |
| --- | ---: | ---: | ---: |
| A | 342,580,790 | 342,636,848 | 56,058 B |
| B | 377,510,615 | 377,568,472 | 57,857 B |
| C | 391,889,215 | 391,948,592 | 59,377 B |
| D | 247,848,277 | 247,905,800 | 57,523 B |

`unhare` handles all four; `unbun` handles none of them.

### Extraction results

| binary | modules | extracted | JS files | native addons |
| --- | ---: | ---: | ---: | ---: |
| A | 11 | 38.0 MB | 6 | 3 |
| B | 1385 | 45.9 MB | 1380 | 3 |
| C | 1387 | 45.9 MB | 1382 | 3 |
| D | 1576 | 45.0 MB | 1407 | 3 |

All 4359 modules extracted, none skipped, roughly 0.4 s per binary.

### Encoding value 2 is UTF-16LE, not UTF-8

A module entry records the encoding of its contents.  Readers that follow
an older revision of the format -- `unbun` among them -- name value 2
`utf8` and write its contents out verbatim.  It is not UTF-8: it holds
UTF-16LE code units, and writing those bytes through yields text files no
other program can read.

Bun stores strings the way JavaScriptCore holds them, either 8-bit
(Latin-1) or 16-bit (UTF-16), so value 2 is the 16-bit case.  `unhare`
reports it as `utf16` and decodes it to UTF-8 on write, handling
surrogate pairs, unpaired surrogates (which become U+FFFD) and odd
lengths.

Measured across all four binaries:

- all 76 encoding-2 modules are UTF-16LE;
- all 4255 Latin-1 modules are single-byte, and **none** contains a byte
  >= 0x80, so that path needs no conversion;
- only one of the four uses encoding 2 at all, which is why the
  mislabelling does not show up when testing against the others.

Decoding took that binary's text assets from 93/165 to 165/165 valid
UTF-8.

Note that this makes the output decoded rather than a byte-faithful copy
of the bundle.  That is deliberate: the original sources were UTF-8, and
UTF-16 storage is a JavaScriptCore implementation detail.  Should a
byte-faithful mode ever be wanted, it belongs behind a new flag rather
than as a change of default.

### What a bundle does not record

Which of the three encodings Bun picks is decided by the contents, not
by anything about the file on disk.  Measured over the 4331 text and
script modules of the binaries tested, the rule is exact:

> pure ASCII is stored latin1; **any** non-ASCII character at all makes
> the module utf16.

Not "whatever does not fit in Latin-1": of the 76 utf16 modules, 64
contain nothing above U+00FF and would have fitted in latin1 easily --
32 of them reach no further than U+00A7 `§`.  Only 12 go past the
Latin-1 range, as far as U+1F50D.  That is JavaScriptCore's string
representation showing through: Bun decodes the source as UTF-8, and the
decoder yields an 8-bit string only for pure ASCII, never narrowing a
16-bit one back down afterwards.

The consequence matters more than the mechanism.  The encoding field
describes how the decoded string sits in the binary; **the encoding of
the original file is recorded nowhere**.  For a text module the original
bytes are therefore not recoverable even in principle -- only its
characters.  Byte-exact round-tripping is possible only for `binary`
modules, which Bun stores verbatim.

So no extractor can hand back a non-UTF-8 text file's original bytes.
The information is gone at bundle time, and this is why the bundled
fixtures under a `utf16` directory have to be UTF-8 on disk.

### Unpacking is an untrusted operation

The bundle is someone else's data, and two of its edges cut.

**A name is checked, a destination is not.**  Refusing `..` in a module
name stops the name from pointing outside the output directory; it does
nothing about a symbolic link already sitting there.  Before this was
fixed, a link left where a module would land carried the write straight
out of the directory, and a symlinked *directory* component did it
without even needing `-f`.  So the final component is opened
`O_NOFOLLOW`, and every directory beneath the output must be a real
directory, `lstat` confirming it.  The directory named by `-o` is the
user's own and stays exempt.

**A name is not fit to print.**  Module names reached the terminal raw,
carrying escape sequences that can rewrite what the user is shown, and
newlines that forge extra entries in a listing `-v` invites you to pipe
into `xargs`.  Control characters are now escaped as `\033` and the
like; bytes at or above 0x80 pass through, so UTF-8 names stay legible
and nothing that could begin an escape sequence survives.

### Two things libelf settles

**Section names are not unique.**  Nothing in ELF forbids two sections
called `.bun`, and `objcopy` will make a second without complaint.  So
all of them are gathered and every bundle among them is unpacked, not
merely the first found.  A section whose bytes hold no trailer is
skipped rather than treated as fatal, and if none holds one the whole
file is scanned as before.

Two bundles cannot share a directory: module names are a bundle's own
business, and nothing keeps them apart across bundles -- the same name
in both is not a conflict to be resolved but two different files.  So
`-o` is a pattern, and a `printf(3)` integer conversion in it is
replaced by the index of the section the bundle came from.  Asking for
two bundles with a pattern that names one place is refused rather than
quietly letting the second write over the first.  Only integer
conversions are accepted; a pattern arrives from the command line, so
anything it might do beyond spelling a number is refused rather than
passed to `printf`.

**An ELF file is already mapped.**  `elf_rawdata()` returns a pointer into
libelf's own image of the file rather than a copy -- measurably so:
`d_buf - elf_rawfile() == sh_offset` exactly.  So there is nothing for
this program to map when reading an ELF section.  The cost is that the ELF
handle must outlive the extraction, since the bytes die with `elf_end()`.

This extends further than it first appears: `elf_rawfile()` returns the
whole file even when it is not an ELF object at all.  `elf_begin()` on
`/etc/motd` yields a descriptor of kind `ELF_K_NONE`, and `elf_rawfile()`
still hands back its bytes on the ELF implementations tested.

The standalone libelf 0.8.13 available for macOS behaves the same way.
Its raw-file view can be scanned across an entire signed Bun 1.4.0 Mach-O
executable, and it finds the bundle preceding the code signature.  The same
macOS build also reads `.bun` sections from ELF executables built on Linux
and FreeBSD.  The program calls neither `mmap` nor `munmap` itself.

That fallback is not hypothetical: Bun builds Mach-O on macOS and PE on
Windows, and the trailer search knows nothing of ELF, so those unpack here
too.

### Other observations

- `.bun` sections in these binaries are marked `WA` (writable, alloc)
  rather than read-only.  This does not affect extraction, but do not
  match on section flags.
- `elf_getshdrstrndx()` is used in place of reading `e_shstrndx`
  directly.  An object with more than `SHN_LORESERVE` sections keeps the
  real index in the `sh_link` of section 0, and the header field reads
  `SHN_XINDEX` instead of a usable index.
- Module names are skipped, with a warning, when they would escape the
  output directory.  Bun's `/$bunfs/` and `B:\~BUN\` prefixes are
  stripped, a leading DOS drive letter is dropped, and `node:fs` style
  names are left alone.
- Bytecode, module metadata and source maps stored alongside a module are
  reported under `-v` but are not written out.  Bun's source maps are a
  separate serialised format whose `sourcesContent` entries are
  individually Zstd-compressed, so decoding them would pull in libzstd.

### How this was verified

Against the ELF binaries tested:

- byte-identical output to an independent reimplementation that parses
  the ELF section table by hand, without libelf;
- every extracted JS file parses under `node --check`, ESM-aware
  (4175/4175);
- every `.node` addon is a structurally valid ELF shared object;
- every text asset is valid UTF-8;
- the UTF-16 decoder's output matches Perl's `Encode` byte-for-byte on
  all 76 real files.

Against synthetic binaries covering `.bun` near EOF, `.bun` buried deep,
`SHN_XINDEX`, an appended payload, a buried payload with no section,
path-traversal names, and UTF-16 decoder edges (surrogate pairs, unpaired
surrogates, high surrogate at EOF, UTF-8 length boundaries, odd length).

Roughly 1400 fuzz cases -- random corruption, offsets-header corruption,
truncation, and cases forcing the UTF-16 path -- produced no crashes.

On Linux and macOS CI, the current official Bun release compiles
`.github/fixtures/native.ts` into native ELF and Mach-O executables.  Each
build runs its native fixture, extracts it, checks that the output is valid
UTF-8 and asks Node.js to parse the extracted JavaScript.

Each operating-system job first builds and tests its own `unhare`, then
uploads it as a workflow artifact.  Linux and macOS include their real Bun
executable too.  After all three builds finish, a second job on each system
downloads the artifacts and uses its native `unhare` to extract the other
systems' executables.  The synthetic bundles are compared byte-for-byte with
`tests/`; the Bun-produced modules are checked for their marker and encoding,
and parsed with Node.js where it is installed.  This exercises ELF section
lookup and Mach-O fallback on every host, including FreeBSD reading both real
Bun formats.  The transient artifacts use GitHub's minimum one-day retention.

A note for anyone writing test fixtures: real Bun labels ASCII content
Latin-1 (encoding 1), never 2.  A fixture that says 2 will be decoded as
UTF-16 and mangled, correctly.

### What the self-test does and does not reach

Two things about `make test` are worth knowing before trusting it.

**Finding the section hides the fallback scan.**  When the `.bun`
section is found, the trailer search only ever sees that section -- a
few hundred bytes.  A regression in the whole-file fallback is
therefore invisible to a test that only reads the section: capping the
search at the last 8 KiB, the very bug that makes `unbun` fail, passed
such a test unnoticed.  That is why `mkbunfs -f` exists, writing the
same bundle into a file with no ELF structure around it.

**And the fallback hides the section lookup.**  Both paths find this
bundle, so breaking the libelf lookup altogether changes nothing the
output can show -- the fallback quietly picks up the slack.  The test
therefore also checks, from `-v`, which path each case actually took.

On macOS the generated array is placed in the Mach-O section
`__DATA,.bun`, since a Mach-O section attribute must name its segment too.
libelf does not read Mach-O, so `-t` necessarily reaches that bundle through
the whole-file fallback there; the self-test can check the libelf section
path only on an ELF build host.

**The filler has to be hostile on purpose.**  The buried fixture puts
the payload between two blocks of filler full of newlines and near
misses of the trailer, and leaves a stretch with no newline just past
the payload.  That last detail is what forces the backward search to
step from the newline ending the trailer onto the one beginning it.
Without it, an off-by-one in the search bound survives the test by
luck -- it did, until the filler was arranged to catch it.

Deliberately breaking the program is the only way to know a test bites.
These are caught: section lookup ignored, trailer search capped at
8 KiB, off-by-one in the backward search, module stride changed from 52,
a slice read from the wrong offset, bundle prefix left unstripped,
parent directories not created, `-t` resolving to the wrong path,
UTF-16 decoding applied where it should not be, the pre-flight
existence check removed, `-f` ignored, and `-f` forced always on.
So are five ways of getting `-x` wrong: a pattern that never matches,
`FNM_PATHNAME` stopping a `*` at the separators, matching only at the
start of a name rather than at every component boundary, the pre-flight
existence check left to trip over an excluded module, and a second `-x`
quietly dropped.

**Astral characters earn their place.**  The three characters ending
`tests/UTF-16/mao.txt` are not decoration.  Each is a surrogate pair, so
they reach the pair-joining branch and the four-byte UTF-8 encoder that
the Chinese characters alone -- all of them BMP, all three bytes --
never touch.  Each was picked for a fault the others miss:

| character | surrogates | reaches |
| --- | --- | --- |
| U+1F4A9 | D83D DCA9 | a low surrogate near the bottom of the range |
| U+1F3AD | D83C DFAD | a low surrogate near the top of it |
| U+20BB7 | D842 DFB7 | the four-byte lead byte |

The spread across the low-surrogate range is what makes narrowing that
bound detectable from either end; one pair would have missed it, and so
would an obvious choice such as U+1F921, whose low surrogate is DD21 and
sits comfortably inside a wrong bound.  U+20BB7 matters for a different
reason: `cp >> 18` and `cp >> 17` both yield zero below U+20000, so no
emoji can tell a broken lead byte from a working one -- every emoji is
below U+1FC00.  A CJK extension B character is past that line.

Three decoder faults survive, and none is reachable from any well-formed
tree:

- narrowing the **high**-surrogate bound needs a high surrogate of
  0xDB00 or above, hence a character at U+D0000 or beyond -- plane 13
  and up, nothing but tags and private use;
- telling `cp >> 18` from `cp >> 19` needs bit 18 set, hence U+40000 or
  beyond, and planes 4 through 13 are entirely unassigned;
- an **unpaired** surrogate cannot appear in the fixtures at all, since
  UTF-8 cannot encode one and the bundle is built from UTF-8 files.
  Only a hand-built bundle reaches that path.

The first two would need a character that does not exist or that no
editor will keep intact, so they are left to the hand-built fixtures.

One more guard is deliberately beyond reach.  `write_file` opens with
`O_EXCL` unless forced, but the pre-flight pass has already rejected
every collision by the time it runs, so dropping it changes nothing
the test can see.  It earns its place only for a bundle naming two
modules the same, where neither exists beforehand and the second write
would quietly land on the first.  A directory tree cannot express
that, filenames within one being unique, so no fixture here reaches
it.

Not covered by the self-test either, and left to testing against real
binaries: `SHN_XINDEX` objects, path-traversal names, Windows-style
names and drive letters, and malformed or truncated bundles.  The
traversal guard in particular is invisible here -- every fixture name
is an ordinary relative path, so the branch that rejects an escaping
name never runs.

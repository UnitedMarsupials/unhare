/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mikhail Teterin
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * unhare -- extract the filesystem bundled into a Bun standalone binary.
 *
 * The payload Bun appends to (or embeds in) an executable is laid out as:
 *
 *	[blob: byte_count bytes]
 *	[struct offsets: 32 bytes]
 *	[trailer: "\n---- Bun! ----\n", 16 bytes]
 *	[total size: uint64_t -- only when appended past the last section]
 *
 * The blob holds a table of fixed-size module descriptors along with every
 * string and file body they point at; all descriptor offsets are relative
 * to the start of the blob.
 *
 * Bun places this payload in a dedicated ".bun" section.  We ask libelf
 * for that section rather than guessing where it lives, which keeps us
 * working on objects whose payload is not the last thing in the file --
 * anything carrying a section header table, symbols or debug information
 * past the bundle.  Only if no section holds a bundle -- a stripped
 * binary, an older Bun release that simply concatenated the payload, or
 * a Mach-O or PE binary from another platform -- do we fall back to
 * scanning the whole file.
 *
 * ELF section bytes and the whole-file fallback come from libelf, whose
 * raw-file view remains valid until elf_end(3).
 *
 * Failures exit with the code from <sysexits.h> that fits: EX_USAGE for
 * a malformed command line, EX_NOINPUT when the named file cannot be
 * read at all, EX_DATAERR when it can but holds no bundle we can make
 * sense of, EX_CANTCREAT when the output cannot be laid down, EX_IOERR
 * when writing it fails partway, and EX_OSERR when the system denies us
 * memory or an answer.
 */

/*
 * asprintf(3), memrchr(3), reallocarray(3) and strsep(3) are behind
 * this on glibc; it has to precede every header.
 */
#if !defined(_GNU_SOURCE) && !defined(__FreeBSD__)
#define	_GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <gelf.h>
#include <libelf.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "compat.h"

#define	BUN_SECTION	".bun"
#define	BUN_TRAILER	"\n---- Bun! ----\n"
#define	BUN_TRAILER_LEN	16

#define	OFFSETS_SIZE	32
#define	MODULE_SIZE	52

/*
 * Bun stores module text the way JavaScriptCore holds a string: either
 * 8-bit (Latin-1) or 16-bit (UTF-16LE).  Readers following an older
 * revision of the format name the 16-bit case "utf8", which it is not.
 */
#define	ENC_BINARY	0
#define	ENC_LATIN1	1
#define	ENC_UTF16	2

/* One bundle, and the section index it is to be named after. */
struct bundle {
	const uint8_t	*base;
	size_t		 len;
	size_t		 index;
};

/* Bun's StringPointer: a { offset, length } pair relative to the blob. */
struct sliceptr {
	uint32_t	 offset;
	uint32_t	 len;
};

/* Bun's Offsets, the 32 bytes immediately preceding the trailer. */
struct offsets {
	uint64_t	 byte_count;
	struct sliceptr	 modules;
	uint32_t	 entry_id;
	struct sliceptr	 argv;
	uint32_t	 flags;
};

/* Bun's CompiledModuleGraphFile, one 52-byte entry per bundled file. */
struct module {
	struct sliceptr	 name;
	struct sliceptr	 content;
	struct sliceptr	 source_map;
	struct sliceptr	 bytecode;
	struct sliceptr	 module_info;
	struct sliceptr	 bytecode_origin;
	uint8_t		 encoding;
	uint8_t		 loader;
	uint8_t		 format;
	uint8_t		 side;
};

static int	 force;
static int	 listonly;
static int	 verbose;
static const char
		**includes;		/* name patterns given as operands */
static size_t	 nincludes;
static const char
		**excludes;		/* name patterns given by -x */
static size_t	 nexcludes;
static size_t	*wanted;		/* section indices asked for by -s */
static size_t	 nwanted;

static void	 add_pattern(const char *, const char ***, size_t *);
static char	*clean_name(const char *);
static char	*escaped(const char *);
static bool	 excluded(const char *);
static bool	 included(const char *);
static void	 decode_module(const uint8_t *, struct module *);
static void	 decode_offsets(const uint8_t *, struct offsets *);
static void	 decode_sliceptr(const uint8_t *, struct sliceptr *);
static const char
		*encoding_name(uint8_t);
static void	 extract(const uint8_t *, size_t, const char *);
static char	*expand_pattern(const char *, size_t);
static size_t	 find_bundles(Elf *, const char *, struct bundle **);
static int	 pattern_conversions(const char *);
static void	 select_section(const char *);
static bool	 wants(size_t);
static const uint8_t
		*find_trailer(const uint8_t *, size_t);
static const char
		*format_name(uint8_t);
static const char
		*loader_name(uint8_t);
static bool	 matches(const char * const *, size_t, const char *);
static char	*module_name(const uint8_t *, const struct module *);
static void	 make_parents(const char *, const char *);
static void	 make_path(const char *);
static int	 open_input(const char *);
static char	*safe_path(const char *, const char *);
static const char
		*self_path(const char *);
static const char
		*side_name(uint8_t);
static void	 slice_check(const struct sliceptr *, uint64_t, const char *);
static void	 usage(int) __dead2;
static size_t	 write_file(const char *, const uint8_t *, size_t);
static uint8_t	*utf16_decode(const char *, const uint8_t *, size_t,
		    size_t *);
static size_t	 write_utf16(const char *, const uint8_t *, size_t);

int
main(int argc, char *argv[])
{
	struct bundle *list;
	const char *file, *pattern, *self;
	char *outdir;
	Elf *elf;
	size_t i, n;
	int ch, fd, tflag;

	self = argv[0];
	pattern = ".";
	tflag = 0;
	while ((ch = getopt(argc, argv, "fhlo:s:tvx:")) != -1) {
		switch (ch) {
		case 'f':
			force = 1;
			break;
		case 'h':
			usage(0);
			/* NOTREACHED */
		case 'l':
			listonly = 1;
			break;
		case 'o':
			pattern = optarg;
			break;
		case 's':
			select_section(optarg);
			break;
		case 't':
			tflag = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'x':
			add_pattern(optarg, &excludes, &nexcludes);
			break;
		default:
			usage(EX_USAGE);
		}
	}
	argc -= optind;
	argv += optind;

	/*
	 * With -t we read the bundle built into this very program,
	 * rather than one named on the command line.
	 */
	if (tflag) {
		file = self_path(self);
	} else {
		if (argc < 1)
			usage(EX_USAGE);
		file = argv[0];
		argc--;
		argv++;
	}
	for (i = 0; i < (size_t)argc; i++)
		add_pattern(argv[i], &includes, &nincludes);

	fd = open_input(file);

	/*
	 * Prefer the section libelf points us at.  Confining the trailer
	 * search to it also keeps us from tripping over a copy of the
	 * magic sitting in unrelated data.
	 *
	 * Whatever libelf hands back points into its own image of the
	 * file, so the descriptor and the ELF handle both have to outlive
	 * the extraction; only a file libelf will not touch is mapped
	 * here.
	 */
	if (elf_version(EV_CURRENT) == EV_NONE)
		errx(EX_SOFTWARE, "libelf initialization failed: %s",
		    elf_errmsg(-1));

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (elf == NULL)
		errx(EX_OSERR, "%s: elf_begin: %s", file, elf_errmsg(-1));

	list = NULL;
	n = 0;
	if (elf_kind(elf) == ELF_K_ELF)
		n = find_bundles(elf, file, &list);
	else if (verbose > 0)
		warnx("%s: not an ELF object, scanning whole file", file);

	/*
	 * Nothing found where we were told to look is an error, not a
	 * reason to go looking elsewhere: scanning the whole file would
	 * turn up some other section's bundle and hand it over as though
	 * it were the one asked for.  A bundle found by scanning answers
	 * to section zero, so -s 0 still reaches it.
	 */
	if (n == 0 && !wants(0))
		errx(EX_DATAERR, "%s: no bundle in any %s section asked for",
		    file, BUN_SECTION);

	if (n == 0) {
		if (elf_kind(elf) == ELF_K_ELF && verbose > 0)
			warnx("%s: no bundle in a %s section, scanning whole "
			    "file", file, BUN_SECTION);
		/* A scanned bundle answers to section zero. */
		list = malloc(sizeof(*list));
		if (list == NULL)
			err(EX_OSERR, "malloc");
		list->base = (const uint8_t *)elf_rawfile(elf, &list->len);
		if (list->base == NULL)
			errx(EX_OSERR, "%s: elf_rawfile: %s", file,
			    elf_errmsg(-1));
		list->index = 0;
		n = 1;
	}

	/*
	 * Every bundle asked for is unpacked, each below its own name.
	 * Two of them would otherwise write over one another, module
	 * names being a bundle's own business and nothing that keeps
	 * them apart across bundles.
	 *
	 * They are gathered before any is unpacked rather than taken as
	 * they are found, because whether the pattern will do depends on
	 * how many there turn out to be, and that has to be settled
	 * before the first file is written.  Unpacking as we went would
	 * lay down the first bundle and only then discover the second
	 * had nowhere of its own to go -- and under -f would let it
	 * quietly overwrite the first.
	 */
	/* Checks the pattern as well as counting, so call it either way. */
	if (pattern_conversions(pattern) == 0 && n > 1)
		errx(EX_USAGE, "-o: %zu bundles to unpack and \"%s\" names "
		    "one place; give it a %%u", n, pattern);

	for (i = 0; i < n; i++) {
		outdir = expand_pattern(pattern, list[i].index);
		if (!listonly)
			make_path(outdir);
		extract(list[i].base, list[i].len, outdir);
		free(outdir);
	}

	free(list);
	elf_end(elf);
	close(fd);
	return (0);
}

/*
 * Asked for by -h, the usage goes to standard output and counts as
 * success; reached by way of a mistake, it goes to standard error
 * and exits EX_USAGE.
 */
static void
usage(int status)
{

	fprintf(status == 0 ? stdout : stderr,
	    "usage: unhare [-flv] [-o pattern] [-s section] [-x pattern] "
	    "file [pattern ...]\n"
	    "       unhare -t [-flv] [-o pattern] [-s section] [-x pattern] "
	    "[pattern ...]\n"
	    "       unhare -h\n");
	exit(status);
}

/*
 * Where this program lives, for -t.  argv[0] answers whenever we were
 * invoked by a path, which is the ordinary case; when it does not --
 * a bare name found along PATH, or a caller that made argv[0] up --
 * ask the kernel instead.
 */
static const char *
self_path(const char *argv0)
{
	static char buf[PATH_MAX];
#if defined(__FreeBSD__)
	size_t len;
	int mib[4];
#elif defined(__linux__)
	ssize_t n;
#elif defined(__APPLE__)
	uint32_t len;
#endif

	if (argv0 != NULL && strchr(argv0, '/') != NULL &&
	    access(argv0, R_OK) == 0)
		return (argv0);

#if defined(__FreeBSD__)
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PATHNAME;
	mib[3] = -1;
	len = sizeof(buf);
	if (sysctl(mib, 4, buf, &len, NULL, 0) == -1)
		err(EX_OSERR, "cannot find the path to this program");
#elif defined(__linux__)
	n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n == -1)
		err(EX_OSERR, "cannot find the path to this program");
	buf[n] = '\0';
#elif defined(__APPLE__)
	len = sizeof(buf);
	if (_NSGetExecutablePath(buf, &len) != 0)
		errx(EX_OSERR, "cannot find the path to this program");
#else
	errx(EX_USAGE, "-t needs to be invoked by a path on this "
	    "system");
#endif

	return (buf);
}

/*
 * Open the file, having satisfied ourselves it is worth reading.  The
 * descriptor stays open for libelf, which reads through it; the caller
 * closes it.
 */
static int
open_input(const char *file)
{
	struct stat sb;
	int fd;

	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(EX_NOINPUT, "%s", file);
	if (fstat(fd, &sb) == -1)
		err(EX_NOINPUT, "%s", file);
	if (!S_ISREG(sb.st_mode))
		errx(EX_NOINPUT, "%s: not a regular file", file);
	if (sb.st_size == 0)
		errx(EX_DATAERR, "%s: empty file", file);

	return (fd);
}

/*
 * Collect the ".bun" sections holding a bundle, in the order they
 * appear, passing over any the -s flags did not ask for -- there is no
 * sense reading a section, let alone searching it, only to put it
 * aside afterwards.  Returns how many were kept and fills *listp with
 * them; the caller frees the list.
 *
 * Section names are not unique in ELF -- nothing in the format forbids
 * two sections called ".bun", and objcopy(1) will cheerfully make a
 * second -- so all of them are gathered rather than only the first.
 *
 * The bytes come from libelf's own image of the file by way of
 * elf_rawdata(3), which points into it rather than copying, so there is
 * nothing here for us to map.  They stay good until elf_end(3), which
 * is why the descriptor outlives this call.
 *
 * elf_getshdrstrndx() is used deliberately in place of e_shstrndx: when
 * a binary has more than SHN_LORESERVE sections the real index is
 * stashed in the sh_link of section 0, and reading the header field
 * directly yields SHN_XINDEX instead of a usable index.
 */
static size_t
find_bundles(Elf *elf, const char *file, struct bundle **listp)
{
	GElf_Shdr shdr;
	struct bundle *list;
	Elf_Data *data;
	Elf_Scn *scn;
	const char *name;
	size_t n, shstrndx;

	if (elf_getshdrstrndx(elf, &shstrndx) != 0) {
		if (verbose > 0)
			warnx("%s: elf_getshdrstrndx: %s", file,
			    elf_errmsg(-1));
		return (0);
	}

	list = NULL;
	n = 0;
	scn = NULL;
	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		if (gelf_getshdr(scn, &shdr) != &shdr)
			errx(EX_DATAERR, "%s: gelf_getshdr: %s", file,
			    elf_errmsg(-1));
		if (shdr.sh_type == SHT_NOBITS)
			continue;
		name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (name == NULL || strcmp(name, BUN_SECTION) != 0)
			continue;
		if (!wants(elf_ndxscn(scn)))
			continue;

		data = elf_rawdata(scn, NULL);
		if (data == NULL || data->d_buf == NULL || data->d_size == 0) {
			if (verbose > 0)
				warnx("%s: %s section %zu is empty", file,
				    BUN_SECTION, elf_ndxscn(scn));
			continue;
		}
		if (find_trailer(data->d_buf, data->d_size) == NULL) {
			if (verbose > 0)
				warnx("%s: %s section %zu holds no trailer",
				    file, BUN_SECTION, elf_ndxscn(scn));
			continue;
		}

		list = reallocarray(list, n + 1, sizeof(*list));
		if (list == NULL)
			err(EX_OSERR, "reallocarray");
		list[n].base = data->d_buf;
		list[n].len = data->d_size;
		list[n].index = elf_ndxscn(scn);
		if (verbose > 0)
			warnx("%s: found %s section %zu at offset %ju, "
			    "%zu bytes", file, BUN_SECTION, list[n].index,
			    (uintmax_t)shdr.sh_offset, list[n].len);
		n++;
	}

	*listp = list;
	return (n);
}

/* Add a module-name pattern to one of the selection lists. */
static void
add_pattern(const char *pattern, const char ***patterns, size_t *npatterns)
{
	const char **p;

	p = reallocarray(*patterns, *npatterns + 1, sizeof(*p));
	if (p == NULL)
		err(EX_OSERR, "reallocarray");
	p[(*npatterns)++] = pattern;
	*patterns = p;
}

/*
 * Whether a module name is covered by any pattern in a list.
 *
 * fnmatch(3) is asked for no FNM_PATHNAME, so that a "*" spans the
 * separators and "*.node" reaches an entry however deeply it is buried,
 * and each pattern is tried again at every component boundary, so that
 * one leading with a directory name catches a nested tree as readily
 * as a top-level one.  Both are how tar(1) reads --exclude, and a
 * pattern written for that one is expected to mean the same here.
 */
static bool
matches(const char * const *patterns, size_t npatterns, const char *name)
{
	const char *p;
	size_t i;

	for (i = 0; i < npatterns; i++) {
		p = name;
		for (;;) {
			if (fnmatch(patterns[i], p, 0) == 0)
				return (true);
			p = strchr(p, '/');
			if (p == NULL)
				break;
			p++;
		}
	}
	return (false);
}

/* Whether a module name is covered by any -x pattern. */
static bool
excluded(const char *name)
{

	return (matches(excludes, nexcludes, name));
}

/* With no operand patterns every module is included. */
static bool
included(const char *name)
{

	return (nincludes == 0 || matches(includes, nincludes, name));
}

/* Note a section asked for by -s. */
static void
select_section(const char *arg)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(arg, &end, 0);
	if (errno != 0 || end == arg || *end != '\0')
		errx(EX_USAGE, "-s: not a section number: %s", arg);

	wanted = reallocarray(wanted, nwanted + 1, sizeof(*wanted));
	if (wanted == NULL)
		err(EX_OSERR, "reallocarray");
	wanted[nwanted++] = (size_t)v;
}

/* Whether a section index was asked for; with no -s, all of them are. */
static bool
wants(size_t index)
{
	size_t i;

	if (nwanted == 0)
		return (true);
	for (i = 0; i < nwanted; i++)
		if (wanted[i] == index)
			return (true);
	return (false);
}

/*
 * Count the conversions in a -o pattern, having satisfied ourselves
 * that each is one an unsigned argument can answer.  A pattern reaches
 * printf(3) from the command line, so anything it might do beyond
 * spelling a number has to be refused rather than passed along.
 */
static int
pattern_conversions(const char *pattern)
{
	const char *p;
	int n;

	n = 0;
	for (p = pattern; *p != '\0'; p++) {
		if (*p != '%')
			continue;
		p++;
		if (*p == '%')
			continue;
		while (*p == '0' || *p == '-' || *p == '+' || *p == ' ' ||
		    *p == '#')
			p++;
		while (isdigit((unsigned char)*p))
			p++;
		if (*p == '\0')
			errx(EX_USAGE, "-o: pattern ends in a bare %%: %s",
			    pattern);
		if (strchr("diouxX", *p) == NULL)
			errx(EX_USAGE, "-o: %%%c is not a number: %s", *p,
			    pattern);
		n++;
	}
	return (n);
}

/* Spell out a -o pattern for one section. */
static char *
expand_pattern(const char *pattern, size_t index)
{
	char *out;
	int len;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
	len = snprintf(NULL, 0, pattern, (unsigned int)index);
	if (len < 0)
		err(EX_OSERR, "snprintf");
	out = malloc((size_t)len + 1);
	if (out == NULL)
		err(EX_OSERR, "malloc");
	snprintf(out, (size_t)len + 1, pattern, (unsigned int)index);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

	return (out);
}

/*
 * Find the last occurrence of the Bun trailer within a region.  Unlike a
 * fixed-size tail probe this copes with a payload buried anywhere in the
 * file.
 */
static const uint8_t *
find_trailer(const uint8_t *base, size_t len)
{
	const uint8_t *p;
	size_t n;

	if (len < BUN_TRAILER_LEN)
		return (NULL);

	/*
	 * A trailer can begin anywhere in [0, len - BUN_TRAILER_LEN];
	 * n is the number of leading bytes still holding a candidate.
	 */
	n = len - BUN_TRAILER_LEN + 1;
	while (n > 0) {
		p = lastbyte(base, BUN_TRAILER[0], n);
		if (p == NULL)
			break;
		if (memcmp(p, BUN_TRAILER, BUN_TRAILER_LEN) == 0)
			return (p);
		n = (size_t)(p - base);
	}
	return (NULL);
}

static void
decode_sliceptr(const uint8_t *p, struct sliceptr *sp)
{

	sp->offset = getle32(p);
	sp->len = getle32(p + 4);
}

static void
decode_offsets(const uint8_t *p, struct offsets *o)
{

	o->byte_count = getle64(p);
	decode_sliceptr(p + 8, &o->modules);
	o->entry_id = getle32(p + 16);
	decode_sliceptr(p + 20, &o->argv);
	o->flags = getle32(p + 28);
}

static void
decode_module(const uint8_t *p, struct module *m)
{

	decode_sliceptr(p + 0, &m->name);
	decode_sliceptr(p + 8, &m->content);
	decode_sliceptr(p + 16, &m->source_map);
	decode_sliceptr(p + 24, &m->bytecode);
	decode_sliceptr(p + 32, &m->module_info);
	decode_sliceptr(p + 40, &m->bytecode_origin);
	m->encoding = p[48];
	m->loader = p[49];
	m->format = p[50];
	m->side = p[51];
}

/* Reject a descriptor pointing outside the blob before we dereference it. */
static void
slice_check(const struct sliceptr *sp, uint64_t bound, const char *what)
{

	if (sp->len == 0)
		return;
	if ((uint64_t)sp->offset + sp->len > bound)
		errx(EX_DATAERR, "%s: offset %u plus length %u exceeds blob "
		    "size %ju",
		    what, sp->offset, sp->len, (uintmax_t)bound);
}

static const char *
loader_name(uint8_t v)
{
	static const char *const names[] = {
		"jsx", "js", "ts", "tsx", "css", "file", "json", "jsonc",
		"toml", "wasm", "napi", "base64", "dataurl", "text", "bunsh",
		"sqlite", "sqlite_embedded", "html", "yaml", "json5", "md"
	};

	return (v < nitems(names) ? names[v] : "?");
}

static const char *
format_name(uint8_t v)
{
	static const char *const names[] = { "none", "esm", "cjs" };

	return (v < nitems(names) ? names[v] : "?");
}

static const char *
encoding_name(uint8_t v)
{
	static const char *const names[] = { "binary", "latin1", "utf16" };

	return (v < nitems(names) ? names[v] : "?");
}

static const char *
side_name(uint8_t v)
{
	static const char *const names[] = { "server", "client" };

	return (v < nitems(names) ? names[v] : "?");
}

/*
 * Strip the virtual filesystem prefix Bun gives every bundled module and
 * translate the result into a host-style relative path.
 */
static char *
clean_name(const char *name)
{
	static const char *const prefixes[] = { "/$bunfs/", "B:\\~BUN\\" };
	char *out, *p;
	size_t i, plen;

	for (i = 0; i < nitems(prefixes); i++) {
		plen = strlen(prefixes[i]);
		if (strncmp(name, prefixes[i], plen) == 0) {
			name += plen;
			break;
		}
	}
	name += strspn(name, "/\\");

	/* A leading DOS drive letter, but not a "node:" style scheme. */
	if (isalpha((unsigned char)name[0]) && name[1] == ':') {
		name += 2;
		name += strspn(name, "/\\");
	}

	out = strdup(name);
	if (out == NULL)
		err(EX_OSERR, "strdup");
	for (p = out; *p != '\0'; p++)
		if (*p == '\\')
			*p = '/';
	return (out);
}

/*
 * Render a string with its control characters escaped.  Module names
 * come out of the bundle and are not to be trusted with a terminal:
 * an escape sequence among them can rewrite what the user is shown,
 * and a newline can forge an entry in a listing meant to be read a
 * line at a time.  Bytes at or above 0x80 pass through, so that UTF-8
 * names stay legible; none of them can begin an escape sequence.
 */
static char *
escaped(const char *s)
{
	const unsigned char *p;
	char *out, *q;

	/* Worst case is four characters out for every one in. */
	out = malloc(strlen(s) * 4 + 1);
	if (out == NULL)
		err(EX_OSERR, "malloc");

	q = out;
	for (p = (const unsigned char *)s; *p != '\0'; p++) {
		if (*p >= 0x20 && *p != 0x7f)
			*q++ = (char)*p;
		else {
			snprintf(q, 5, "\\%03o", *p);
			q += 4;
		}
	}
	*q = '\0';
	return (out);
}

/*
 * Join an untrusted module name onto the output directory, resolving "."
 * and ".." as we go.  Returns NULL if the name would escape the output
 * directory or names nothing at all.
 */
static char *
safe_path(const char *outdir, const char *name)
{
	char **comp;
	char *copy, *p, *path, *seg, *state;
	size_t depth, i, len, n;
	bool slash;

	copy = strdup(name);
	if (copy == NULL)
		err(EX_OSERR, "strdup");
	comp = calloc(strlen(copy) + 1, sizeof(*comp));
	if (comp == NULL)
		err(EX_OSERR, "calloc");

	depth = 0;
	state = copy;
	while ((seg = strsep(&state, "/")) != NULL) {
		if (*seg == '\0' || strcmp(seg, ".") == 0)
			continue;
		if (strcmp(seg, "..") == 0) {
			if (depth == 0) {
				free(comp);
				free(copy);
				return (NULL);
			}
			depth--;
			continue;
		}
		comp[depth++] = seg;
	}
	if (depth == 0) {
		free(comp);
		free(copy);
		return (NULL);
	}

	len = strlen(outdir) + 1;
	for (i = 0; i < depth; i++)
		len += strlen(comp[i]) + 1;
	path = malloc(len);
	if (path == NULL)
		err(EX_OSERR, "malloc");

	/*
	 * Assembled by hand rather than with strlcat(3), which glibc
	 * only came by lately.  The length is known exactly, so there
	 * is nothing here for a bounded copy to guard against.
	 */
	n = strlen(outdir);
	memcpy(path, outdir, n);
	p = path + n;

	/* A directory spelt with a trailing slash needs no second one. */
	slash = n != 0 && outdir[n - 1] == '/' ? false : true;
	for (i = 0; i < depth; i++) {
		if (slash)
			*p++ = '/';
		slash = true;
		n = strlen(comp[i]);
		memcpy(p, comp[i], n);
		p += n;
	}
	*p = '\0';

	free(comp);
	free(copy);
	return (path);
}

/* Create a directory and every missing component leading up to it. */
static void
make_path(const char *path)
{
	char *copy, *p;

	if (*path == '\0')
		return;
	copy = strdup(path);
	if (copy == NULL)
		err(EX_OSERR, "strdup");
	for (p = copy + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(copy, 0777) == -1 && errno != EEXIST)
			err(EX_CANTCREAT, "mkdir: %s", copy);
		*p = '/';
	}
	if (mkdir(copy, 0777) == -1 && errno != EEXIST)
		err(EX_CANTCREAT, "mkdir: %s", copy);
	free(copy);
}

/*
 * Create the directories holding a file, below outdir.
 *
 * Every component the bundle asked for has to be a directory in its
 * own right.  Were one of them a symbolic link we would descend
 * through it and lay the file down wherever it pointed, which the
 * refusal of ".." in the name was supposed to rule out.  The
 * directory the user named is theirs and is left alone; only what we
 * create or find beneath it is held to this.
 */
static void
make_parents(const char *outdir, const char *path)
{
	struct stat sb;
	char *copy, *p;
	size_t skip;

	skip = strlen(outdir);
	if (strlen(path) <= skip + 1)
		return;

	copy = strdup(path);
	if (copy == NULL)
		err(EX_OSERR, "strdup");

	for (p = copy + skip + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(copy, 0777) == -1) {
			if (errno != EEXIST)
				err(EX_CANTCREAT, "mkdir: %s", copy);
			if (lstat(copy, &sb) == -1)
				err(EX_CANTCREAT, "%s", copy);
			if (!S_ISDIR(sb.st_mode))
				errx(EX_CANTCREAT,
				    "%s: not a directory", copy);
		}
		*p = '/';
	}
	free(copy);
}

static size_t
write_file(const char *path, const uint8_t *data, size_t len)
{
	ssize_t n;
	size_t done;
	int fd, flags;

	/*
	 * O_NOFOLLOW because the name came out of a bundle we do not
	 * trust: a symbolic link left in the output tree would other-
	 * wise carry the write outside it, which is precisely what
	 * refusing ".." in the name was meant to prevent.
	 */
	flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW;
	flags |= force ? O_TRUNC : O_EXCL;
	fd = open(path, flags, 0666);
	if (fd == -1) {
		if (errno == EEXIST)
			errx(EX_CANTCREAT,
			    "%s: already exists (-f overwrites)", path);
		if (errno == EMLINK || errno == ELOOP)
			errx(EX_CANTCREAT, "%s: is a symbolic link", path);
		err(EX_CANTCREAT, "%s", path);
	}
	for (done = 0; done < len; done += (size_t)n) {
		n = write(fd, data + done, len - done);
		if (n == -1)
			err(EX_IOERR, "%s", path);
		if (n == 0)
			errx(EX_IOERR, "%s: write wrote nothing", path);
	}
	if (close(fd) == -1)
		err(EX_IOERR, "%s", path);

	return (len);
}

/*
 * Decode UTF-16LE content into UTF-8 and write it out.  A module whose
 * text does not fit in Latin-1 is bundled as 16-bit code units, so
 * copying those bytes verbatim would leave a file nothing else can
 * read; decoding restores the source as it was before bundling.
 */
static uint8_t *
utf16_decode(const char *path, const uint8_t *data, size_t len,
    size_t *lenp)
{
	uint8_t *buf;
	size_t i, n, o;
	uint32_t cp, lo;

	if (len % 2 != 0)
		warnx("%s: odd UTF-16 length %zu, ignoring trailing byte",
		    path, len);
	n = len / 2;

	/* No code unit expands past three bytes; a pair yields four. */
	if (n > (SIZE_MAX - 1) / 3)
		errx(EX_DATAERR, "%s: UTF-16 content too large to decode",
		    path);
	buf = malloc(n * 3 + 1);
	if (buf == NULL)
		err(EX_OSERR, "malloc");

	o = 0;
	for (i = 0; i < n; i++) {
		cp = getle16(data + i * 2);
		if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n) {
			lo = getle16(data + (i + 1) * 2);
			if (lo >= 0xdc00 && lo <= 0xdfff) {
				cp = 0x10000 + ((cp - 0xd800) << 10) +
				    (lo - 0xdc00);
				i++;
			}
		}
		if (cp >= 0xd800 && cp <= 0xdfff)
			cp = 0xfffd;	/* unpaired surrogate */

		if (cp < 0x80)
			buf[o++] = (uint8_t)cp;
		else if (cp < 0x800) {
			buf[o++] = (uint8_t)(0xc0 | (cp >> 6));
			buf[o++] = (uint8_t)(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			buf[o++] = (uint8_t)(0xe0 | (cp >> 12));
			buf[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
			buf[o++] = (uint8_t)(0x80 | (cp & 0x3f));
		} else {
			buf[o++] = (uint8_t)(0xf0 | (cp >> 18));
			buf[o++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
			buf[o++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
			buf[o++] = (uint8_t)(0x80 | (cp & 0x3f));
		}
	}

	*lenp = o;
	return (buf);
}

/* Decode UTF-16LE content and write it out as UTF-8. */
static size_t
write_utf16(const char *path, const uint8_t *data, size_t len)
{
	uint8_t *buf;
	size_t o;

	buf = utf16_decode(path, data, len, &o);
	write_file(path, buf, o);
	free(buf);
	return (o);
}


/*
 * The name a module unpacks under: what the bundle records, with Bun's
 * virtual filesystem prefix stripped and the separators turned the
 * host's way.  This is the name selection patterns are matched against,
 * and the one safe_path() joins to the output directory.  The caller
 * frees the result.
 */
static char *
module_name(const uint8_t *blob, const struct module *m)
{
	char *clean, *raw;

	raw = strndup((const char *)blob + m->name.offset, m->name.len);
	if (raw == NULL)
		err(EX_OSERR, "strndup");

	clean = clean_name(raw);
	free(raw);
	return (clean);
}

/*
 * Walk the module table and write every bundled file below outdir.
 */
static void
extract(const uint8_t *region, size_t rlen, const char *outdir)
{
	struct module m;
	struct offsets o;
	const uint8_t *blob, *entry, *table, *trailer;
	uint8_t *decoded;
	char *clean, *name, *path, *shown;
	size_t nwrote;
	uintmax_t total;
	uint32_t i, nmodules, written;

	trailer = find_trailer(region, rlen);
	if (trailer == NULL)
		errx(EX_DATAERR,
		    "no Bun trailer found: not a Bun standalone binary");
	if ((size_t)(trailer - region) < OFFSETS_SIZE)
		errx(EX_DATAERR,
		    "truncated payload: no room for the offsets header");

	entry = trailer - OFFSETS_SIZE;
	decode_offsets(entry, &o);

	if (o.byte_count > (uint64_t)(entry - region))
		errx(EX_DATAERR,
		    "blob size %ju exceeds the %ju bytes before the header",
		    (uintmax_t)o.byte_count, (uintmax_t)(entry - region));
	blob = entry - o.byte_count;

	if (o.modules.len % MODULE_SIZE != 0)
		errx(EX_DATAERR, "module table length %u is not a multiple "
		    "of %d",
		    o.modules.len, MODULE_SIZE);
	slice_check(&o.modules, o.byte_count, "module table");
	slice_check(&o.argv, o.byte_count, "argv");

	nmodules = o.modules.len / MODULE_SIZE;
	if (nmodules != 0 && o.entry_id >= nmodules)
		errx(EX_DATAERR, "entry point index %u is beyond the %u "
		    "modules",
		    o.entry_id, nmodules);

	if (verbose > 1)
		warnx("blob %ju bytes, %u modules, entry %u, flags %#x",
		    (uintmax_t)o.byte_count, nmodules, o.entry_id, o.flags);

	table = blob + o.modules.offset;

	/*
	 * Look for anything already in the way before writing a single
	 * file, so that a run which would clobber something leaves the
	 * directory as it found it rather than stopping halfway with the
	 * job half done.  A module the selection patterns skip is passed
	 * over here as well: a file in the way of something we were told
	 * not to write is in the way of nothing.
	 */
	if (!force && !listonly)
		for (i = 0; i < nmodules; i++) {
			decode_module(table + (size_t)i * MODULE_SIZE, &m);
			slice_check(&m.name, o.byte_count, "module name");

			clean = module_name(blob, &m);
			if (!included(clean) || excluded(clean)) {
				free(clean);
				continue;
			}
			path = safe_path(outdir, clean);
			free(clean);
			if (path == NULL)
				continue;
			if (access(path, F_OK) == 0)
				errx(EX_CANTCREAT,
				    "%s: already exists (-f overwrites)", path);
			free(path);
		}

	total = 0;
	written = 0;
	for (i = 0; i < nmodules; i++) {
		decode_module(table + (size_t)i * MODULE_SIZE, &m);

		slice_check(&m.name, o.byte_count, "module name");
		slice_check(&m.content, o.byte_count, "module content");
		slice_check(&m.source_map, o.byte_count, "module source map");
		slice_check(&m.bytecode, o.byte_count, "module bytecode");
		slice_check(&m.module_info, o.byte_count, "module info");
		slice_check(&m.bytecode_origin, o.byte_count,
		    "module bytecode origin");

		clean = module_name(blob, &m);
		if (!included(clean)) {
			if (verbose > 1) {
				shown = escaped(clean);
				warnx("skipping module %u: \"%s\" is not "
				    "selected", i, shown);
				free(shown);
			}
			free(clean);
			continue;
		}
		if (excluded(clean)) {
			if (verbose > 1) {
				shown = escaped(clean);
				warnx("skipping module %u: \"%s\" is "
				    "excluded", i, shown);
				free(shown);
			}
			free(clean);
			continue;
		}
		path = safe_path(outdir, clean);
		free(clean);
		if (path == NULL) {
			name = strndup((const char *)blob + m.name.offset,
			    m.name.len);
			if (name == NULL)
				err(EX_OSERR, "strndup");
			shown = escaped(name);
			warnx("skipping module %u: unsafe name \"%s\"", i,
			    shown);
			free(shown);
			free(name);
			continue;
		}

		if (listonly) {
			/*
			 * Decode all the same, and throw the result away:
			 * the size worth reporting is the size the file
			 * would have, which for a UTF-16 module is not the
			 * size it occupies in the bundle.
			 */
			if (m.encoding == ENC_UTF16) {
				decoded = utf16_decode(path,
				    blob + m.content.offset, m.content.len,
				    &nwrote);
				free(decoded);
			} else
				nwrote = m.content.len;
		} else {
			make_parents(outdir, path);
			if (m.encoding == ENC_UTF16)
				nwrote = write_utf16(path,
				    blob + m.content.offset, m.content.len);
			else
				nwrote = write_file(path,
				    blob + m.content.offset, m.content.len);
		}
		total += nwrote;
		written++;

		/*
		 * One name to a line, so that the listing can be piped
		 * onward; the particulars wait for a second -v.  The size
		 * reported is what landed on disk, which for a UTF-16
		 * module is not what it occupied in the bundle.
		 */
		if (listonly || verbose > 0) {
			shown = escaped(path);
			if (verbose > (listonly ? 0 : 1))
				printf("%s (%zu bytes, %s/%s/%s/%s)%s%s\n",
				    shown, nwrote, loader_name(m.loader),
				    format_name(m.format),
				    encoding_name(m.encoding),
				    side_name(m.side),
				    i == o.entry_id ? " [entry]" : "",
				    m.source_map.len != 0 ?
				    " [has source map]" : "");
			else
				printf("%s\n", shown);
			free(shown);
		}

		free(path);
	}

	fflush(stdout);
	if (verbose > 0)
		warnx("%s %u of %u modules, %ju bytes into %s",
		    listonly ? "would extract" : "extracted", written,
		    nmodules, total, outdir);
}

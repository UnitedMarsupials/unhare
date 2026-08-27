/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mikhail Teterin
 */

/*
 * mkbunfs -- build the Bun bundle that unhare embeds in itself.
 *
 * This is a build tool, not part of the installed program.  It walks a
 * directory -- tests/ -- and emits either the C source holding that
 * directory as a bundle (-c), or a file with the same bundle buried in
 * filler and no ELF structure around it (-f).  The former is linked into
 * unhare so that "unhare -t" can unpack it again; the latter is the only
 * way to reach the fallback scan, because once the section is found the
 * search never looks at the rest of the file.
 *
 * Contents are bundled under whichever Bun encoding the enclosing
 * directory is named for, so that a single tree exercises all three:
 *
 *	US-ASCII	latin1, stored one byte per character
 *	UTF-16		utf16, converted to UTF-16LE going in and back out
 *			again by unhare
 *	anything else	binary, stored verbatim -- the right choice for a
 *			legacy eight-bit encoding such as KOI8-U, whose
 *			bytes mean nothing to Bun
 *
 * The files under UTF-16 are themselves UTF-8 on disk: the conversion
 * happens on the way into the bundle, and unhare undoes it on the way
 * out, so the round trip returns them unchanged.  A file already in
 * UTF-16 could not round-trip, unhare's business being to hand back
 * decoded UTF-8.
 */

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	BUN_TRAILER	"\n---- Bun! ----\n"
#define	BUN_TRAILER_LEN	16
#define	OFFSETS_SIZE	32
#define	MODULE_SIZE	52

#define	BUNFS_PREFIX	"/$bunfs/"

#define	ENC_BINARY	0
#define	ENC_LATIN1	1
#define	ENC_UTF16	2

#define	LOADER_JS	1
#define	LOADER_FILE	5
#define	LOADER_TEXT	13

#define	FORMAT_NONE	0
#define	FORMAT_CJS	2

/* One bundled file, with its contents already encoded. */
struct bfile {
	char		*name;		/* name as bundled */
	uint8_t		*body;
	size_t		 blen;
	uint8_t		 encoding;
	uint8_t		 loader;
	uint8_t		 format;
};

static struct bfile	*files;
static size_t		 nfiles;

static void	 add_file(const char *, const char *, const char *);
static void	 emit_buried(const char *);
static void	 emit_source(const char *, const uint8_t *, size_t);
static uint8_t	 encoding_for(const char *);
static void	 loader_for(const char *, uint8_t *, uint8_t *);
static uint8_t	*make_payload(size_t *);
static uint8_t	*read_file(const char *, size_t *);
static void	 usage(void);
static uint32_t	 utf8_next(const char **);
static uint8_t	*utf16le(const uint8_t *, size_t, size_t *);
static void	 walk(const char *, const char *, const char *);

int
main(int argc, char *argv[])
{
	const char *buried, *csrc, *dir, *root;
	uint8_t *payload;
	size_t len;
	int ch;

	buried = csrc = NULL;
	while ((ch = getopt(argc, argv, "c:f:")) != -1) {
		switch (ch) {
		case 'c':
			csrc = optarg;
			break;
		case 'f':
			buried = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1 || (csrc == NULL) == (buried == NULL))
		usage();
	dir = argv[0];

	/*
	 * The directory's own name leads every bundled path, so that
	 * unpacking recreates tests/ rather than its contents alone.
	 */
	root = strrchr(dir, '/');
	root = root != NULL ? root + 1 : dir;
	if (*root == '\0')
		errx(1, "%s: cannot take a name from this path", dir);

	walk(dir, "", root);
	if (nfiles == 0)
		errx(1, "%s: no files to bundle", dir);

	if (buried != NULL) {
		emit_buried(buried);
		return (0);
	}

	payload = make_payload(&len);
	emit_source(csrc, payload, len);
	free(payload);
	return (0);
}

static void
usage(void)
{

	fprintf(stderr, "usage: mkbunfs -c source.c directory\n"
	    "       mkbunfs -f file directory\n");
	exit(1);
}

/*
 * The Bun encoding to bundle a file under, taken from the first path
 * component -- the directory named for an encoding.
 */
static uint8_t
encoding_for(const char *rel)
{
	size_t n;

	n = strcspn(rel, "/");
	if (n == strlen("US-ASCII") && strncmp(rel, "US-ASCII", n) == 0)
		return (ENC_LATIN1);
	if (n == strlen("UTF-16") && strncmp(rel, "UTF-16", n) == 0)
		return (ENC_UTF16);
	return (ENC_BINARY);
}

/* A plausible loader and module format, going by the file's suffix. */
static void
loader_for(const char *rel, uint8_t *loader, uint8_t *format)
{
	const char *dot;

	dot = strrchr(rel, '.');
	if (dot != NULL && strcmp(dot, ".js") == 0) {
		*loader = LOADER_JS;
		*format = FORMAT_CJS;
	} else if (dot != NULL && (strcmp(dot, ".txt") == 0 ||
	    strcmp(dot, ".md") == 0)) {
		*loader = LOADER_TEXT;
		*format = FORMAT_NONE;
	} else {
		*loader = LOADER_FILE;
		*format = FORMAT_NONE;
	}
}

static uint8_t *
read_file(const char *path, size_t *lenp)
{
	struct stat sb;
	uint8_t *buf;
	ssize_t n;
	size_t done;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "%s", path);
	if (fstat(fd, &sb) == -1)
		err(1, "%s", path);

	buf = malloc((size_t)sb.st_size + 1);
	if (buf == NULL)
		err(1, "malloc");
	for (done = 0; done < (size_t)sb.st_size; done += (size_t)n) {
		n = read(fd, buf + done, (size_t)sb.st_size - done);
		if (n == -1)
			err(1, "%s", path);
		if (n == 0)
			break;
	}
	close(fd);

	*lenp = done;
	return (buf);
}

/* Bundle one file, encoding its contents as its directory calls for. */
static void
add_file(const char *path, const char *rel, const char *root)
{
	struct bfile *f;
	uint8_t *raw;
	size_t rlen;

	files = reallocarray(files, nfiles + 1, sizeof(*files));
	if (files == NULL)
		err(1, "reallocarray");
	f = &files[nfiles++];
	memset(f, 0, sizeof(*f));

	if (asprintf(&f->name, "%s%s/%s", BUNFS_PREFIX, root, rel) == -1)
		err(1, "asprintf");

	f->encoding = encoding_for(rel);
	loader_for(rel, &f->loader, &f->format);

	raw = read_file(path, &rlen);
	if (f->encoding == ENC_UTF16) {
		f->body = utf16le(raw, rlen, &f->blen);
		free(raw);
	} else {
		f->body = raw;
		f->blen = rlen;
	}
}

/* Collect a directory, in a fixed order, so the build is reproducible. */
static void
walk(const char *dir, const char *rel, const char *root)
{
	struct dirent **list;
	struct stat sb;
	char *path, *sub;
	int i, n;

	if (rel[0] == '\0')
		path = strdup(dir);
	else if (asprintf(&path, "%s/%s", dir, rel) == -1)
		err(1, "asprintf");
	if (path == NULL)
		err(1, "strdup");

	n = scandir(path, &list, NULL, alphasort);
	if (n < 0)
		err(1, "%s", path);

	for (i = 0; i < n; i++) {
		char *entry;

		if (strcmp(list[i]->d_name, ".") == 0 ||
		    strcmp(list[i]->d_name, "..") == 0) {
			free(list[i]);
			continue;
		}
		if (asprintf(&entry, "%s/%s", path, list[i]->d_name) == -1)
			err(1, "asprintf");
		if (rel[0] == '\0')
			sub = strdup(list[i]->d_name);
		else if (asprintf(&sub, "%s/%s", rel, list[i]->d_name) == -1)
			err(1, "asprintf");
		if (sub == NULL)
			err(1, "strdup");

		if (lstat(entry, &sb) == -1)
			err(1, "%s", entry);
		if (S_ISDIR(sb.st_mode))
			walk(dir, sub, root);
		else if (S_ISREG(sb.st_mode))
			add_file(entry, sub, root);
		else
			warnx("%s: not a regular file, skipped", entry);

		free(sub);
		free(entry);
		free(list[i]);
	}

	free(list);
	free(path);
}

/* Decode one UTF-8 character, advancing the cursor past it. */
static uint32_t
utf8_next(const char **sp)
{
	const uint8_t *s;
	uint32_t cp;
	int n, i;

	s = (const uint8_t *)*sp;
	if (s[0] < 0x80) {
		cp = s[0];
		n = 1;
	} else if ((s[0] & 0xe0) == 0xc0) {
		cp = s[0] & 0x1f;
		n = 2;
	} else if ((s[0] & 0xf0) == 0xe0) {
		cp = s[0] & 0x0f;
		n = 3;
	} else if ((s[0] & 0xf8) == 0xf0) {
		cp = s[0] & 0x07;
		n = 4;
	} else
		errx(1, "malformed UTF-8 lead byte %#x", s[0]);

	for (i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			errx(1, "malformed UTF-8 continuation byte %#x", s[i]);
		cp = (cp << 6) | (s[i] & 0x3f);
	}

	*sp += n;
	return (cp);
}

/* Convert UTF-8 to the UTF-16LE a bundle stores for such a module. */
static uint8_t *
utf16le(const uint8_t *data, size_t len, size_t *lenp)
{
	const char *s, *end;
	uint8_t *buf;
	size_t o;
	uint32_t cp;

	/* No character needs more than two code units. */
	buf = malloc(len * 4 + 2);
	if (buf == NULL)
		err(1, "malloc");

	s = (const char *)data;
	end = s + len;
	o = 0;
	while (s < end) {
		cp = utf8_next(&s);
		if (s > end)
			errx(1, "truncated UTF-8 sequence");
		if (cp < 0x10000) {
			le16enc(buf + o, (uint16_t)cp);
			o += 2;
		} else {
			cp -= 0x10000;
			le16enc(buf + o, (uint16_t)(0xd800 + (cp >> 10)));
			le16enc(buf + o + 2,
			    (uint16_t)(0xdc00 + (cp & 0x3ff)));
			o += 4;
		}
	}

	*lenp = o;
	return (buf);
}

/*
 * Assemble the bundle: the blob, then the module table, then the
 * offsets header and the trailer.
 */
static uint8_t *
make_payload(size_t *lenp)
{
	struct {
		uint32_t	name_off, name_len;
		uint32_t	body_off, body_len;
	} *ent;
	uint8_t *buf, *p;
	size_t blob, i, len, table, total;

	ent = calloc(nfiles, sizeof(*ent));
	if (ent == NULL)
		err(1, "calloc");

	blob = 0;
	for (i = 0; i < nfiles; i++)
		blob += strlen(files[i].name) + files[i].blen;
	blob = roundup2(blob, 4);
	table = blob;
	blob += nfiles * MODULE_SIZE;

	total = blob + OFFSETS_SIZE + BUN_TRAILER_LEN;
	buf = calloc(1, total);
	if (buf == NULL)
		err(1, "calloc");

	/* Names and contents. */
	len = 0;
	for (i = 0; i < nfiles; i++) {
		ent[i].name_len = (uint32_t)strlen(files[i].name);
		ent[i].name_off = (uint32_t)len;
		memcpy(buf + len, files[i].name, ent[i].name_len);
		len += ent[i].name_len;

		ent[i].body_len = (uint32_t)files[i].blen;
		ent[i].body_off = (uint32_t)len;
		memcpy(buf + len, files[i].body, files[i].blen);
		len += files[i].blen;
	}

	/* Module table: six slice pointers, then the four enums. */
	for (i = 0; i < nfiles; i++) {
		p = buf + table + i * MODULE_SIZE;
		le32enc(p + 0, ent[i].name_off);
		le32enc(p + 4, ent[i].name_len);
		le32enc(p + 8, ent[i].body_off);
		le32enc(p + 12, ent[i].body_len);
		/* source_map, bytecode, module_info, origin stay zero. */
		p[48] = files[i].encoding;
		p[49] = files[i].loader;
		p[50] = files[i].format;
		p[51] = 0;			/* side: server */
	}

	/* Offsets header, then the trailer. */
	p = buf + blob;
	le64enc(p, (uint64_t)blob);
	le32enc(p + 8, (uint32_t)table);
	le32enc(p + 12, (uint32_t)(nfiles * MODULE_SIZE));
	le32enc(p + 16, 0);			/* entry point */
	le32enc(p + 20, 0);			/* argv offset */
	le32enc(p + 24, 0);			/* argv length */
	le32enc(p + 28, 0);			/* flags */
	memcpy(buf + blob + OFFSETS_SIZE, BUN_TRAILER, BUN_TRAILER_LEN);

	free(ent);
	*lenp = total;
	return (buf);
}

/*
 * Write the bundle as a C array placed in its own ".bun" section, which
 * is all it takes for the linker to give us a section unhare can find.
 */
static void
emit_source(const char *path, const uint8_t *data, size_t len)
{
	FILE *f;
	size_t i;

	f = fopen(path, "w");
	if (f == NULL)
		err(1, "%s", path);

	fprintf(f, "/* Generated by mkbunfs.  Do not edit. */\n\n");
	fprintf(f, "__attribute__((__section__(\".bun\"), __used__))\n");
	fprintf(f, "static const unsigned char unhare_bunfs[] = {");
	for (i = 0; i < len; i++)
		fprintf(f, "%s0x%02x,", i % 12 == 0 ? "\n\t" : " ", data[i]);
	fprintf(f, "\n};\n");

	if (ferror(f) != 0 || fclose(f) != 0)
		err(1, "%s", path);
}

/*
 * Write the bundle buried in filler, with no ELF structure around it, so
 * that unhare has to fall back to scanning the whole file.  The filler
 * is stuffed with newlines and with near misses of the trailer, both to
 * push the payload well past any fixed-size tail probe and to make the
 * backward search reject a great many candidates before finding the real
 * one.
 */
#define	FILLER_BYTES	40000
#define	FILLER_PATTERN	"\n---- Bun! ---\n"	/* one dash short */
#define	GAP_BYTES	64			/* holding no newline */

static void
emit_buried(const char *path)
{
	FILE *f;
	uint8_t *payload;
	size_t i, len;

	payload = make_payload(&len);

	f = fopen(path, "w");
	if (f == NULL)
		err(1, "%s", path);

	for (i = 0; i < FILLER_BYTES; i += sizeof(FILLER_PATTERN) - 1)
		fputs(FILLER_PATTERN, f);
	if (fwrite(payload, 1, len, f) != len)
		err(1, "%s", path);

	/*
	 * Leave a stretch without a newline just past the payload.  The
	 * backward search then has to step from the newline that ends the
	 * trailer onto the one that begins it, sixteen bytes below: an
	 * off-by-one in the search bound steps over the second newline
	 * and loses the trailer altogether.
	 */
	for (i = 0; i < GAP_BYTES; i++)
		putc('.', f);

	for (i = 0; i < FILLER_BYTES; i += sizeof(FILLER_PATTERN) - 1)
		fputs(FILLER_PATTERN, f);

	if (ferror(f) != 0 || fclose(f) != 0)
		err(1, "%s", path);
	free(payload);
}

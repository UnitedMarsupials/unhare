/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mikhail Teterin
 */

/*
 * What the BSDs keep in their own headers, for systems having no such
 * headers.
 *
 * The little-endian accessors are spelled out here rather than taken
 * from <sys/endian.h>, that header being a BSD invention.  Written byte
 * by byte they need no header at all, and no notion of the host's own
 * byte order either.
 */

#ifndef UNHARE_COMPAT_H_
#define	UNHARE_COMPAT_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef nitems
#define	nitems(a)	(sizeof(a) / sizeof((a)[0]))
#endif

#ifndef roundup2
#define	roundup2(x, m)	(((x) + ((m) - 1)) & ~((m) - 1))
#endif

#ifndef __dead2
#if defined(__GNUC__)
#define	__dead2		__attribute__((__noreturn__))
#else
#define	__dead2
#endif
#endif

/*
 * memrchr(3) is a GNU extension the BSDs picked up; where it is absent
 * the loop below stands in for it.
 */
#if defined(__GLIBC__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define	HAVE_MEMRCHR	1
#endif

static inline uint16_t
getle16(const void *buf)
{
	const uint8_t *p = buf;

	return ((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t
getle32(const void *buf)
{
	const uint8_t *p = buf;

	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static inline uint64_t
getle64(const void *buf)
{
	const uint8_t *p = buf;

	return ((uint64_t)getle32(p) | (uint64_t)getle32(p + 4) << 32);
}

static inline void
putle16(void *buf, uint16_t v)
{
	uint8_t *p = buf;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
putle32(void *buf, uint32_t v)
{
	uint8_t *p = buf;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
putle64(void *buf, uint64_t v)
{
	uint8_t *p = buf;

	putle32(p, (uint32_t)v);
	putle32(p + 4, (uint32_t)(v >> 32));
}

/* The last byte equal to c among the first n, or NULL if there is none. */
static inline const uint8_t *
lastbyte(const uint8_t *base, uint8_t c, size_t n)
{
#ifdef HAVE_MEMRCHR
	return (memrchr(base, c, n));
#else
	while (n > 0)
		if (base[--n] == c)
			return (base + n);
	return (NULL);
#endif
}

#endif /* UNHARE_COMPAT_H_ */

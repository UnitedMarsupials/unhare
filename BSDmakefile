PROG=	unhare
SRCS=	unhare.c bunfs.c
MAN=	unhare.1

DPADD=	${LIBELF}
LDADD=	-lelf

WARNS?=	6

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin
MANDIR?=	${PREFIX}/share/man/man

#
# The tests/ tree is bundled into the program itself, so that "unhare -t"
# can unpack it again and the result be compared against the original.
#
# Nothing here injects the section, because nothing needs to: mkbunfs
# writes bunfs.c holding the bundle as a byte array tagged
# __attribute__((section(".bun"))), bunfs.c is named in SRCS, and the
# linker makes the section from it like any other input.  No objcopy
# pass, no post-processing of the linked program; the bundle stays
# inside the normal dependency graph, and being SHF_ALLOC it survives
# the strip that "make install" performs.
#
# mkbunfs comes from the implicit rule; it runs on the build host, so a
# cross build needs a host compiler for it.
#
TESTDIR=	${.CURDIR}/tests
TESTFILES!=	find ${TESTDIR} -type f

CLEANFILES+=	bunfs.c mkbunfs

bunfs.c: mkbunfs ${TESTFILES}
	./mkbunfs -c ${.TARGET} ${TESTDIR}

beforedepend: bunfs.c

#
# bsd.prog.mk installs into directories it expects to find already
# there, which is not so for a staged install.
#
beforeinstall:
	${INSTALL} -d ${DESTDIR}${BINDIR}
	${INSTALL} -d ${DESTDIR}${MANDIR}${MAN:E}

test: ${PROG} mkbunfs
	@sh ${.CURDIR}/selftest.sh ./${PROG} ./mkbunfs ${TESTDIR}

.include <bsd.prog.mk>

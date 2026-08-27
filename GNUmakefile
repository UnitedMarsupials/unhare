# Build with GNU make, for systems that have no bsd.prog.mk.
#
# BSD make reads BSDmakefile in preference to this, and GNU make reads
# this in preference to anything else, so the two live side by side
# without either noticing the other.

PROG =		unhare
SRCS =		unhare.c bunfs.c
OBJS =		$(SRCS:.c=.o)
MAN =		unhare.1

PREFIX ?=	/usr/local
BINDIR ?=	$(PREFIX)/bin
MANDIR ?=	$(PREFIX)/share/man/man1

CC ?=		cc
CFLAGS ?=	-O2 -g
CFLAGS +=	-Wall -Wextra
LDLIBS +=	-lelf
INSTALL ?=	install

# libelf may live outside the compiler's default search path.  This is
# normally the case on macOS, which does not provide the library itself.
ifneq ($(LIBELF_PREFIX),)
CPPFLAGS +=	-I$(LIBELF_PREFIX)/include \
		-I$(LIBELF_PREFIX)/include/libelf
LDFLAGS +=	-L$(LIBELF_PREFIX)/lib
endif

# The tests/ tree is bundled into the program itself; see BSDmakefile.
TESTDIR =	tests
TESTFILES :=	$(shell find $(TESTDIR) -type f)

.PHONY: all clean install test

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# mkbunfs runs on the build host, so a cross build needs a host compiler.
mkbunfs: mkbunfs.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

bunfs.c: mkbunfs $(TESTFILES)
	./mkbunfs -c $@ $(TESTDIR)

bunfs.o: bunfs.c
unhare.o mkbunfs: compat.h

test: $(PROG) mkbunfs
	@sh ./selftest.sh ./$(PROG) ./mkbunfs $(TESTDIR)

install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 555 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	$(INSTALL) -d $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 444 $(MAN) $(DESTDIR)$(MANDIR)/$(MAN)

clean:
	rm -f $(PROG) $(OBJS) mkbunfs bunfs.c
	rm -rf $(PROG).dSYM mkbunfs.dSYM

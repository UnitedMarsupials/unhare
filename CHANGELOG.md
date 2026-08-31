# Changelog

## 1.1.1 (2026-08-31)

- Accept shell-glob patterns after the input binary, extracting only files
  that match at least one.  With no patterns, every file is extracted as
  before, and `-x` exclusions still take precedence.

## 1.1 (2026-08-28)

- Add `-x pattern`, which skips the files whose names match a shell
  glob, the way `tar --exclude` does.  It may be given more than once,
  and a file in the way of a module it skips no longer stops the run.

## 1.0.1 (2026-08-27)

- Add macOS support using the system GNU make and a separately installed
  libelf selected with `LIBELF_PREFIX`.
- Test real Bun-produced Mach-O and ELF executables, and exchange native
  builds among macOS, Linux and FreeBSD for cross-platform extraction tests.

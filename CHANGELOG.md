# Changelog

## 1.0.1 (2026-08-27)

- Add macOS support using the system GNU make and a separately installed
  libelf selected with `LIBELF_PREFIX`.
- Test real Bun-produced Mach-O and ELF executables, and exchange native
  builds among macOS, Linux and FreeBSD for cross-platform extraction tests.

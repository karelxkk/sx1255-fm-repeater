# Repository Guidelines

This repository integrates the Semtech SX1255 transceiver with SvxLink on
Raspberry Pi hardware. Contributions should preserve the generated-package
workflow described in `doc/README.md`.

## Project Structure & Module Organization

- `src/` — C code: the `sx1255_repeater` daemon and SPI frequency helpers.
- `scripts/` — `svx_sx1255_ptt_bridge.py`, the SvxLink PTT-to-TCP bridge.
- `systemd/` — service unit templates and the SvxLink ordering drop-in.
- `config/` — ALSA loopback, service defaults, and the SvxLink template.
- `debian/` — package metadata and maintainer scripts.
- `doc/` — end-user installation and operation documentation.

Template files use `.in` and are rendered by `./configure` into `build/`.
Generated `Makefile`, `config.mk`, and `build/` are intentionally not committed.

## Build, Test, and Development Commands

- `./configure` — render `config.mk`, `Makefile`, and generated templates. Use
  `--no-dialog` plus callsign/reflector options for non-interactive runs.
- `make` — compile `sx1255_repeater`, `sx1255-spi`, and `sx1255-spi-fixed`.
- `make deb` — create an installable Debian package under `build/deb/`.
- `sudo make install` — install binaries, services, and configuration.
- `make clean` / `make distclean` — remove build artifacts or generated files.

Always run `./configure && make` after changing C, template, or `*.in` files.

## Coding Style & Naming Conventions

C code targets C11 and must build cleanly with `-Wall -Wextra`. New code uses
four-space indentation, `snake_case` functions, and `_t` typedefs. Preserve the
existing style in legacy SPI helpers instead of reformatting them.

Python code follows PEP 8, uses four-space indentation and type hints, and keeps
the `snake_case` naming used in the bridge script. Shell code is POSIX `sh` and
uses `set -e` or `set -eu`.

No formatter or linter is configured; match the surrounding code.

## Testing Guidelines

There is no automated test suite. Validate changes by running `./configure`,
`make`, and `python3 -m py_compile scripts/svx_sx1255_ptt_bridge.py`. For
behavioral changes, install the generated package and smoke-test the systemd
units on target hardware, checking `journalctl` output for the affected
services.

## Commit & Pull Request Guidelines

Use imperative, present-tense one-line summaries such as
`Document SvxLink tone and talk group settings`. Keep the subject under 72
characters and add a body only when context or rationale is needed.

Pull requests should state what changed, why it is needed, and how it was
verified. Include hardware, SPI, audio, or systemd impact when relevant. Do not
commit real reflector passwords or generated build files.

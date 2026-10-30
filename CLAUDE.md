# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Voltio is a C11 serial driver for BK Precision 1900B-Series bench power supplies (1685B, 1687B, 1688B, 1900B, 1901B, 1902B). It exposes a platform-agnostic C API, a CLI that outputs JSON, a Windows DLL for CANoe/CAPL integration, and a CAPL script (`voltio.cin`).

## Build commands

```sh
# Configure (first time or after CMakeLists changes)
cmake -B build

# Build (macOS: CLI only)
cmake --build build

# Output: build/bin/voltio

# Cross-compile for Windows (requires: brew install mingw-w64)
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake
cmake --build build-win
# Output: build-win/bin/voltio.exe  and  build-win/bin/voltio.dll
```

There are no automated tests. Manual verification requires a physical PSU on a serial port.

## Architecture

The codebase has three layers:

**Serial HAL** (`serial.h` interface, `serial_posix.c` / `serial_win32.c` implementations)  
Thin platform abstraction: open at 9600 8N1, write a buffer, read one byte, flush RX. The correct impl is selected at configure time based on `WIN32`. On macOS, use `/dev/cu.usbserial-XXXX` (not `tty.*`) to avoid blocking on carrier detect.

**Protocol driver** (`voltio.c`, `voltio.h`)  
Owns an opaque `Voltio_Context` (serial handle + decimal-place config + last-error string). All PSU commands are CR-terminated ASCII; all responses end with `OK\r\n`. `send_command()` in `voltio.c` is the single I/O path — it flushes RX, writes, reads lines until `OK`, accumulates any data lines into a caller buffer.

Value encoding is internal: 3-digit ASCII × scale factor. Voltage is always scale 10 (1 d.p.). Current is scale 10 or 100 depending on `dp` passed to `voltio_open` — `2` for the 1685B (2 d.p. current, per the BK programming manual), `1` for every other model. `GETD` (actual display readings) always returns 4-digit values decoded at fixed 100 scale. Out-of-range values return `VOLTIO_ERR_RANGE` instead of being clamped.

**Consumers** (`voltio_cli.c`, `voltio_dll.c`)  
`voltio_cli.c` — standalone executable; all output is JSON to stdout (errors to stderr). Global options `--port` and `--dp` come before the command.  
`voltio_dll.c` — Windows-only DLL exporting a flat C API (`Voltio_Open`, `Voltio_SetVoltage`, etc.) for use from CANoe CAPL via `loadDll`/`callDll`. The DLL holds a single global context. `Voltio_OpenDp(port, dp)` selects current decimal places (`Voltio_Open` = dp 1). `Voltio_ReadDisplay()` takes one `GETD` sample that the `Voltio_Display*()` getters then return without further I/O.

## Compiler flags

`-Wall -Wextra -Werror -Wpedantic` on GCC/Clang; `/W4 /WX` on MSVC. All warnings are errors — fix them, don't suppress them.

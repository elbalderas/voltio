# Voltio

C11 serial driver, CLI, and Vector CANoe integration for **BK Precision 1900B-series** bench power supplies.

Supported models: 1685B, 1687B, 1688B, 1900B, 1901B, 1902B — any unit speaking the 1900B-series ASCII protocol (9600 8N1 over USB-serial).

## What you get

| Deliverable | Platform | Purpose |
|---|---|---|
| `voltio` (CLI) | macOS, Linux, Windows | Control the PSU from the shell; all output is JSON |
| `voltio.dll` | Windows | Flat C API for CANoe CAPL via `loadDll`/`callDll` |
| `voltio.cin` | CANoe | CAPL wrappers + settling check + example test case |
| `libvoltio` core | all | Platform-agnostic C API (`voltio.h`) to embed in your own tools |

## Build

```sh
cmake -B build
cmake --build build
# → build/bin/voltio
```

Cross-compile the Windows CLI and DLL from macOS/Linux (needs `brew install mingw-w64` or equivalent):

```sh
cmake -G Ninja -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake
cmake --build build-win
# → build-win/bin/voltio.exe  and  build-win/bin/voltio.dll
```

Binaries are statically linked — no runtime DLLs needed.

## CLI usage

```sh
# macOS: use /dev/cu.* (NOT /dev/tty.* — it blocks on carrier detect)
voltio --port /dev/cu.usbserial-0001 status
voltio --port COM4 status                      # Windows

voltio --port <port> set --voltage 12.0 --current 2.5
voltio --port <port> output on|off
voltio --port <port> preset list
voltio --port <port> preset recall 2
voltio --port <port> preset write 1 12.0 2.5
voltio --port <port> limits [--ovp 15.0] [--ocp 5.0]
voltio --port <port> max
```

Example output:

```json
{
  "status": "ok",
  "voltage_set_V": 12.800,
  "current_set_A": 5.000,
  "voltage_actual_V": 12.820,
  "current_actual_A": 0.080,
  "power_actual_W": 1.026,
  "mode": "CV"
}
```

Errors go to stderr as JSON with `"status": "error"`; exit code is non-zero.

## CANoe integration

1. Copy `build-win/bin/voltio.dll` next to your CANoe configuration.
2. Include `voltio.cin` in your test node.
3. Open/close the connection around the measurement:

```c
on start          { Voltio_Open("COM4"); }
on stopMeasurement { Voltio_Close(); }
```

All DLL exports are wrapped as plain CAPL functions (`Voltio_SetVoltage`,
`Voltio_OutputOn`, `Voltio_GetVoltageActual`, …). For test sequences, use
`Voltio_WaitSettled(v_expected, tol_V, timeout_ms)` after changing the
output — the PSU has no ready flag and its readings lag output changes by
up to ~1 s, so a fixed `wait()` is not reliable. `TC_Voltio_12V_2A` in
`voltio.cin` shows the full pattern.

## Protocol notes

- Commands are CR-terminated ASCII; every response ends with `OK\r\n`.
- Values are 3 ASCII digits × a scale factor. Most models use one decimal
  place (`"125"` = 12.5 V, pass `dp=1` to `voltio_open`); two-decimal models
  use `dp=2`. Actual display readings (`GETD`) are always 4 digits at two
  decimals.
- Preset responses (`GETM`) may arrive as one line per slot or all three
  slots concatenated on a single line; the driver handles both.
- Output enable is inverted on the wire: `SOUT0` = on, `SOUT1` = off.

## Verification status

Tested end-to-end against a physical 1901B on macOS: every CLI command,
including output switching, presets, and protection limits. The Windows
serial backend and DLL are cross-compiled and export-verified but have not
yet been exercised on real Windows/CANoe hardware. There are no automated
tests — verification requires a physical PSU.

## License

[MIT](LICENSE)

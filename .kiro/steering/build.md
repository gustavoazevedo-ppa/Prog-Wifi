---
inclusion: always
---

# Building this firmware (ESP-IDF)

ESP-IDF **v5.4.4** project for the ESP32 (target: `modulo_wifi`). Host OS is Windows; use PowerShell syntax.

## Rules

- Always build through the repo scripts (`scripts\build.ps1` or `scripts\idf_env.ps1`).
- Never use the stock `export.ps1`. It can auto-detect the wrong Python (the shell's Python or a Windows Store stub) and leave `idf.py` unavailable. The repo scripts pin the exact venv Python.
- Run a build after any change to `main\*.c`, `components\`, or `CMakeLists.txt` files before reporting the work as done.
- Treat the IDF version as fixed at v5.4.4. Do not point tooling at the old v5.4.1 tree (`C:\Users\gusta\esp\v5.4.1`); it is kept on disk but unused.

## Commands

```powershell
# Build (activates IDF env, then runs idf.py build):
.\scripts\build.ps1

# Any idf.py subcommand works too:
.\scripts\build.ps1 flash monitor
.\scripts\build.ps1 fullclean

# Or activate the env in the current shell and use idf.py directly:
. .\scripts\idf_env.ps1
idf.py build
```

`flash monitor` is interactive and long-running. Do not run it in a blocking shell; ask the user to run it.

## Verifying a change compiles

A successful build ends with `Project build complete.` and produces `build\prog_wifi.bin`. Confirm both before claiming success.

## Known-good environment

- IDF framework: `C:\esp\v5.4.4\esp-idf` (installed by the ESP-IDF Installation Manager, EIM)
- IDF tools + venvs: `C:\Espressif` (`IDF_TOOLS_PATH`)
- Venv Python: `C:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe` (Python **3.12**, not 3.11)
- `scripts\idf_env.ps1` runs `idf_tools.py export --format key-value` with that venv Python and imports the emitted `KEY=VALUE` lines.
- An "unsupported cmake 4.1.1 in PATH" warning on stderr is harmless. IDF prepends its own cmake 3.30.2.

If paths change (IDF reinstalled, new venv), update the three variables at the top of `scripts\idf_env.ps1`.

## History

The project was on v5.4.1 until Kiro's Espressif extension (2.1.0 → 2.3.0) switched to EIM, which installed v5.4.4 at `C:\esp\v5.4.4` and could no longer cleanly select the hand-managed 5.4.1 tree. v5.4.4 is the same v5.4 bugfix line, with no API changes.

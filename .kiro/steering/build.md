# Building this firmware (ESP-IDF)

This is an ESP-IDF **v5.4.4** project for the ESP32 (target: `modulo_wifi`).

> History: the project was previously on v5.4.1. When Kiro's bundled Espressif
> extension (2.1.0 → 2.3.0) switched to the ESP-IDF Installation Manager (EIM),
> it installed v5.4.4 at `C:\esp\v5.4.4` and could no longer cleanly select the
> hand-managed 5.4.1 tree. We moved to 5.4.4 (same v5.4 bugfix line — no API
> changes). The 5.4.1 tree at `C:\Users\gusta\esp\v5.4.1` is left in place but
> unused.

## How to build

Do NOT use the stock `export.ps1` — it can fail on this machine by auto-detecting
the shell's Python and/or a Windows Store Python stub, leaving `idf.py`
unavailable. The repo scripts pin the exact venv python instead.

Use the repo scripts instead:

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

## Known-good environment (as of this writing)

- IDF framework: `C:\esp\v5.4.4\esp-idf` (installed by EIM)
- IDF tools + venvs: `C:\Espressif` (set `IDF_TOOLS_PATH` to this)
- Working venv python: `C:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe`
  (note: the 5.4.4 EIM install uses **Python 3.12**, not 3.11)
- Activation trick: run `idf_tools.py export --format key-value` with the venv
  python above and import the emitted `KEY=VALUE` lines. `scripts\idf_env.ps1`
  does exactly this. A harmless "unsupported cmake 4.1.1 in PATH" warning on
  stderr can be ignored — IDF prepends its own cmake 3.30.2.

If paths change (IDF reinstalled, new venv), update the three variables at the
top of `scripts\idf_env.ps1`.

## Verifying a change compiles

A successful `idf.py build` ends with `Project build complete.` and produces
`build\modulo_wifi.bin`. The build compiles `main\*.c` including
`monip_client.c` (the MONIP/DC-09 receiver client).

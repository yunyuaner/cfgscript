**Project**: cfgscript
- **Description:** Lightweight C library and CLI to parse, preprocess and dump configuration files with simple macro/loop features. The repo contains the library (`cfgscript.c`, `cfgscript.h`), a CLI wrapper (`cfgscript_cli.c`), docs in `doc/`, and unit tests in `unit_tests/`.

**Requirements:**
- **C compiler:** `gcc` (or compatible)
- **Platform:** Tested on Windows (MSYS2/mingw) and Unix-like systems

**Build:**
- Build the CLI directly with `gcc` (example):

```bash
gcc -g cfgscript_cli.c cfgscript.c -o cfgscript_cli.exe
```

- Or use the provided VS Code task `build gcc` (available in the workspace).

**Usage:**
- General help:

```bash
./cfgscript_cli --help
```

- Load and display a config file:

```bash
./cfgscript_cli load myconfig.cfg
# or using the option
./cfgscript_cli load --file myconfig.cfg
```

- Dump preprocessed config:

```bash
./cfgscript_cli dump input.cfg output.cfg
# or with options
./cfgscript_cli dump --in input.cfg --out output.cfg
```

- Global option: `--verbose` (or `-v`) enables extra messages.

**Tests:**
- Unit tests are under `unit_tests/`. See `unit_tests/README_TESTS.md` for test instructions.

**Docs:**
- Design and language spec are in `doc/` (`cfgscript_design_spec.md`, `cfgscript_language_spec.md`).

**Repository:**
- Important files: `cfgscript.c`, `cfgscript.h`, `cfgscript_cli.c`, `Makefile`, `unit_tests/`, `configs/`.
- A `.gitignore` has been added to ignore build artifacts, editors, and OS files.

**Contributing:**
- Fork, add tests for new features, and submit PRs. Keep changes small and well-documented.

**License:**
- Add your preferred license file (e.g., `LICENSE`) if desired.

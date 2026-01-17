# cfgscript unit tests

## Files

- `test_cfgscript.c` : unit test runner (no external deps)
- `configs/` : representative configuration files for testing

## Build

Assuming you have:
- `cfgscript.c`
- `cfgscript.h`
- `test_cfgscript.c`

Compile:

```bash
gcc -O2 -std=c11 -Wall -Wextra -I. test_cfgscript.c cfgscript.c -o test_cfgscript
```

Run:

```bash
./test_cfgscript ./configs
```

Expected output includes `OK` for each test.

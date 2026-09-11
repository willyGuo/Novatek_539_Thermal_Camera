# DDE Y8

Display order: VideoProc YUV420 -> optional Mirror -> CLAHE -> DDE -> Pseudo Color -> output.

The core preserves the supplied Domain Transform filtering and detail formula:
base + gain * (original - base). It now reads/writes uint8_t pixels directly,
uses a 256-entry difference LUT, and interprets line_stride in bytes.
Only Y is modified; row padding and UV are untouched. Output is clipped to
0..255 to match the existing CLAHE Y8 display path. Internal filtering remains
floating point. Percentile mapping remains disabled by default.

CFG Setting -> 9. DDE setting:

- On/off (default off).
- Standard: 3 iterations.
- Fast: 1 iteration.
- Sigma S: integer 1..256 pixels (default 8).
- Sigma R: integer 1..255 Y8 DN (default 16).
- Gain: integer 0..1000 percent (default 150; 100 preserves original).
- Iterations: 1..8 (default 3).

Presets only change iterations and preserve enable and all other parameters.
Defaults are initial tuning values, not board-validated image-quality presets.
Each menu change saves /mnt/sd/tiffany_thermal.cfg before publishing settings.
Failed saves retain the prior settings. The display worker snapshots settings
under a mutex on each frame. Direct edits to .cfg require reload/restart.
Keys: dde_enable, dde_sigma_s, dde_sigma_r, dde_gain_percent, dde_num_iters.

The display worker lazily allocates and reuses its workspace; shutdown releases
it after the worker has joined. No intermediate uint16_t image is allocated.
SDK build and board cache/FPS testing are still required.

Host core test (GCC):

    gcc -std=c99 -O2 -Wall -Wextra -Werror DDE/tests/test_dde.c DDE/dde_dt_opt.c DDE/dde.c -o test_dde -lm -lpthread
    ./test_dde

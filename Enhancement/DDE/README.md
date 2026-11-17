# DDE Y8

Display order: VideoProc YUV420 -> optional Mirror -> CLAHE -> DDE -> Pseudo Color -> output.

The core preserves the supplied Domain Transform filtering and detail formula:
base + gain * (original - base). It now reads/writes uint8_t pixels directly,
uses a 256-entry difference LUT, and interprets line_stride in bytes.
Only Y is modified; row padding and UV are untouched. Output is clipped to
0..255 to match the existing CLAHE Y8 display path. Internal filtering remains
floating point. Percentile mapping remains disabled by default.

CFG Setting -> 10. DDE setting:

- On/off (default off).
- Standard: Sigma S 21, Sigma R 51, gain 200%, 2 directional passes.
- Fast: 1 iteration.
- Sigma S: integer 1..256 pixels (default 8).
- Sigma R: integer 1..255 Y8 DN (default 16).
- Gain: integer 0..1000 percent (default 150; 100 preserves original).
- Directional passes: 1..8 (default 2; forward then reverse).

Standard/Fast set Sigma S 21, Sigma R 51, gain 200%, and 2/1 passes;
they preserve enable. Simple display Standard and the local CFG use 2 passes.
Defaults are initial tuning values, not board-validated image-quality presets.
Each menu change saves /mnt/sd/tiffany_thermal.cfg before publishing settings.
Failed saves retain the prior settings. The display worker snapshots settings
under a mutex on each frame. Direct edits to .cfg require reload/restart.
Keys: dde_enable, dde_sigma_s, dde_sigma_r, dde_gain_percent, dde_num_iters, dde_base_scale.

The display worker lazily allocates and reuses its workspace; shutdown releases
it after the worker has joined. No intermediate uint16_t image is allocated.
SDK build and board cache/FPS testing are still required.

The default recurrence is now DDE_RECUR_LERP=1: s + a * (previous - s).
Both passes still process the full Y8 image. Strip fusion and paired-sigma LUT
reuse remain enabled. No fast-math, frame skipping or downsampling is added.
Floating-point rounding can differ from the original formula; current host
tests found at most 1 Gray difference across 2,805,980 pixel comparisons.
Build with -DDDE_RECUR_LERP=0 for an original-formula A/B comparison.
Optional host timing: python tests/test_dde_1106.py --benchmark --report <path>.
Host core timing excludes HDAL mapping/cache and encoder/UVC; it is not board FPS.

DDE_SIMD4 auto-enables on ARM NEON/AArch64 builds. Explicit GNU four-float
vectors advance four independent row/column recurrences and recombine/clip
four output pixels; scalar tails cover all dimensions. Define DDE_SIMD4=0
for a target A/B baseline. The forced SIMD host tests exactly match scalar
outputs, but x86 timing did not improve; ARM speed/FPS is not yet validated.
GNU vector API reference: https://gcc.gnu.org/onlinedocs/gcc/Vector-Extensions.html

CFG -> 10. DDE setting shows the actual compiled backend (scalar/SIMD4).
Runtime timing collection and its reset option have been removed.
A 30 FPS result still requires board measurement.

Experimental half-base mode: option 8, Base resolution, selects 1 (full/default)
or 2 (half width/height). Original full mode remains available and unchanged.
The half guide uses rounded 2x2 averages, sigma_s/2, unchanged sigma_r/gain,
all requested passes, a floating-point base and center-aligned bilinear upsample.
Two expanded base rows are cached; recombination still uses the full Y8 input.
Odd dimensions, row padding and UV are supported. No temporal frame reuse occurs.
High-contrast edges may differ: compare halos, fine targets and detail before use.
Simple display Standard restores scale 1; DDE Standard/Fast preserve chosen scale.
Runtime timing statistics remain removed. Test: python tests/test_dde_half_base.py.

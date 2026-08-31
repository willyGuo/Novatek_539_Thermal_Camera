Thermal RAW Toolkit v1.0.0
==========================

Run:
  pip install -r requirements_thermal_raw_toolkit.txt
  python thermal_raw_toolkit.py

Default RAW format:
  640 x 512
  RAW16 Little Endian (<u2)
  14-bit effective range: 0 ~ 16383
  Selectable bit depths: 8, 10, 12, 14, 16-bit
  RAW previews and preview PNGs use the selected Bit Depth's fixed scale.
  No content-dependent percentile/min-max stretching is applied implicitly.

Functions:
  1. RAW Viewer
     - Selecting a RAW file immediately shows pixel count, minimum, maximum, mean,
       median, population standard deviation (ddof=0), P1, and P99 in original DN
  2. Average RAW Folder
  3. NUC Calculate
     - Folder Average (Recommended)
     - Single RAW
     - Select Point 1 ~ 5 and generate Novatek pointN_K.bin / pointN_B.bin
     - K is uint16 Q8; B is signed int16 for output = K * (input - B) >> 8
     - HIGH input and corrected HIGH previews use the same fixed Bit Depth scale; no percentile stretching
  4. NUC Apply
     - Load Novatek pointN_K.bin and pointN_B.bin
     - Apply output = K * (input - B) >> 8
     - Save original.raw, original.png, corrected_nuc.raw, and corrected_nuc.png
     - PNG uses fixed Bit Depth scaling; no automatic contrast enhancement
  5. High / Low Analyzer
  6. Temporal Drift Analyzer
  7. BPR Bad Pixel Marking
     - Load a Novatek SIE-format dpc_table.bin or CSV/TXT x,y coordinate list
     - Optionally overlay red circles on a RAW fixed-scale preview
     - Without a RAW input, mark the coordinates on a white canvas
     - Duplicate physical coordinates are marked once; out-of-bounds coordinates are reported
     - Show table/unique/duplicate/marked counts and every x,y coordinate in the UI
     - The shared Font Size control adjusts both Information / Log and Bad Pixel Information
     - Save the marked PNG, base PNG, normalized coordinate CSV, and summary TXT
  8. RAW Algorithm Compare
     - Load one RAW file and compare its fixed Bit Depth preview with a selected algorithm
     - Algorithms match UVC Algorithm Compare: Auto Contrast, Fieldscale Firmware Core,
       Histogram Equalization, CLAHE, Gamma, Bilateral Denoise, Unsharp Mask,
       Auto Contrast + CLAHE, and P1/P99 Percentile Preview (Original Values)
     - Fieldscale uses original RAW DN with the firmware 8x8 grid, 400 RAW14-DN
       max/min difference, seven message passes, and no Gamma/CLAHE
     - P1/P99 Percentile Preview matches raw_nuc_apply_percentile.py's preview stage
       without NUC: calculate P1/P99 on original RAW16 values, then map to 8-bit
     - Save original_fixed.png and result_<algorithm>.png
  9. UVC Algorithm Compare
     - Live Original / selected algorithm comparison
     - Optional Novatek K/B NUC preprocessing before the selected image algorithm
     - K/B dimensions must match Resolution; 8-bit UVC is linearly mapped to Bit Depth DN before NUC
     - Right-side algorithms: Auto Contrast, Fieldscale Firmware Core,
       Histogram Equalization, CLAHE, Gamma, Bilateral Denoise, Unsharp Mask,
       and Auto Contrast + CLAHE
     - UVC Fieldscale runs in the available grayscale domain with an equivalent
       RAW14 threshold and current-10% / previous-90% temporal field smoothing
     - Percentile stretching runs only when Auto Contrast or P1/P99 Percentile Preview is selected
     - Reset Algorithm Defaults restores the selected algorithm's parameters
     - CAMERA_INDEX switching (Camera 0 ~ 9)
     - Requested FPS options: 30 FPS or 60 FPS
     - Only the selected algorithm's parameter rows are shown
     - Capture original and processed PNG pair
     - Background capture keeps the UI responsive during FFC pauses
     - Keeps the last frame and automatically reconnects after 5 seconds without frames

Result saving:
  Results are grouped by function under Thermal_RAW_Results.
  Every run creates a new timestamped subfolder inside that function folder.
  Example: Thermal_RAW_Results/nuc_calculate/20260818_183000_000/
  Existing results are never overwritten.

Recommended NUC calibration workflow:
  LOW temperature folder -> pixel-by-pixel average -> LOW average RAW
  HIGH temperature folder -> pixel-by-pixel average -> HIGH average RAW
  LOW/HIGH average RAW -> calculate per-pixel gain/offset

Use Single RAW mode only for quick experiments/debugging. Multi-frame averaging reduces temporal noise in calibration coefficients.

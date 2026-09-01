Thermal RAW Toolkit v1.1.0
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
  8. Dual UVC Compare
     - Open Camera A and Camera B in two independent capture threads
     - Each camera selector shows the Windows camera friendly name and current
       OpenCV index, plus its own Image Name and Requested FPS
     - Camera devices are checked every second; hot-plug updates both selectors
       automatically, while Refresh Cameras provides a manual refresh
     - Selection is preserved by Windows device ID instead of unstable index;
       a removed camera is marked Disconnected and its worker waits for replug
     - Capture uses internal Auto Detect with no mode selector: try Novatek MJPG
       first, then fall back to the camera's native format automatically when
       opening, format negotiation, or initial frame reading fails
     - Status and saved metadata record the mode actually selected by Auto Detect
     - Image Name choices are PEGA, 天淵, and 方見; defaults are Camera A = PEGA
       and Camera B = 天淵
     - Camera indexes must differ; each camera keeps worker-owned capture release
     - Display the two latest UVC frames side by side; pairing is software-based
       latest-frame pairing and is not hardware-trigger synchronization
     - Each input uses a latest-frame mailbox so pending old frames are replaced
       instead of accumulating GUI latency
     - The selected names appear in preview captions, overview/recording labels,
       capture metadata, and recording metadata
     - Capture Pair saves name-tagged files such as camera_a_PEGA.png and
       camera_b_TianYuan.png, plus
       00_dual_uvc_overview.png, capture.json, and capture_summary.txt
     - Start Recording / Stop Recording saves dual_uvc_comparison.avi (MJPG),
       00_dual_uvc_overview.png, recording.json, and recording_summary.txt
     - Recording FPS uses the lower valid reported FPS of Camera A and Camera B
     - Camera stop, page change, setting change, and application exit safely
       finish recording and stop both capture workers
  9. RAW Algorithm Compare
     - Build an ordered pipeline with any number of enabled steps
     - Add, remove, repeat, enable/disable, move up, and move down independently
     - Available steps: Auto Contrast, Fieldscale Firmware Core, Histogram
       Equalization, CLAHE, Gamma, Bilateral Denoise, and Unsharp Mask
     - Every repeated step owns independent parameters
     - Input Mapping is fixed before the pipeline: Fixed Bit Depth Scale, or
       P1/P99 calculated from original RAW DN
     - Compatibility rule: when Fixed Bit Depth is selected and Fieldscale is
       the first enabled step, Fieldscale receives original RAW DN; later steps
       receive its 8-bit output
     - Fieldscale retains the firmware 8x8 grid, 400 RAW14-DN max/min difference,
       seven message passes, and no built-in Gamma/CLAHE
     - Right Preview can inspect the final output or any enabled step
     - Save original, mapped/native input, final, optional step PNGs,
       pipeline.json, and pipeline_summary.txt in a timestamped result folder
     - Every Run also saves 00_pipeline_overview.png: Original, Pipeline Input,
       and every enabled step arranged in order with numbered algorithm labels
  10. UVC Algorithm Compare
     - Live Original / ordered algorithm pipeline comparison
     - Capture uses internal Auto Detect with no mode selector: try Novatek MJPG
       first, then automatically fall back to the camera's native format
     - Status and saved metadata record the mode actually selected by Auto Detect
     - Add, remove, repeat, enable/disable, and reorder the same steps as RAW
     - Each Fieldscale step has independent current-10% / previous-90% temporal state
     - Right Preview can show the final output or any enabled step
     - CAMERA_INDEX switching (Camera 0 ~ 9)
     - The selector shows Windows friendly names, updates every second for
       hot-plug, and also provides a manual Refresh Cameras button
     - Selection is preserved by Windows device ID; saved metadata includes the
       friendly name, device ID, current index, and actual selected capture mode
     - Requested FPS options: 30 FPS or 60 FPS
     - Status displays output FPS, total pipeline time, per-step time, and frame budget
     - Camera capture and pipeline processing use separate workers; pending old
       frames are replaced by the newest frame instead of accumulating latency
     - Capture original, pipeline input, final, combined comparison, optional
       step PNGs, pipeline.json, and pipeline_summary.txt
     - Every Capture Pair also saves 00_pipeline_overview.png with Original UVC,
       Pipeline Input, and every enabled step in numbered execution order
     - Start Recording / Stop Recording saves synchronized Original UVC and
       Final Pipeline Output side by side as comparison_pipeline.avi (MJPG)
     - Video encoding runs in a separate worker. Timestamp-based duplicate/drop
       handling preserves constant-FPS playback when processing or FFC pauses
     - Each recording folder also saves 00_pipeline_overview.png, pipeline.json,
       and recording_summary.txt with duration, frame, drop, and file-size data
     - Camera stop/switch, pipeline changes, processing errors, and application
       exit safely finish the active AVI before the UVC workers are released
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

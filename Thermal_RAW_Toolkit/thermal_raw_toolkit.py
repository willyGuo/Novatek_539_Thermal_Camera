import os
import sys
import csv
import glob
import re
import time
import threading
from datetime import datetime
from pathlib import Path

import numpy as np

try:
    import cv2
except ImportError:
    cv2 = None

try:
    from PyQt5.QtCore import Qt, QThread, QTimer, pyqtSignal
    from PyQt5.QtGui import QImage, QPixmap
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QFileDialog, QMessageBox,
        QVBoxLayout, QHBoxLayout, QGridLayout, QFormLayout, QLabel,
        QPushButton, QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit,
        QTextEdit, QGroupBox, QStackedWidget, QSplitter, QCheckBox
    )
except ImportError as exc:
    raise SystemExit("PyQt5 is required. Install with: pip install PyQt5") from exc


APP_VERSION = "v1.0.0"
APP_TITLE = f"Thermal RAW Toolkit {APP_VERSION}"
DEFAULT_WIDTH = 640
DEFAULT_HEIGHT = 512
RESOLUTION_OPTIONS = [
    (256, 192),
    (320, 240),
    (384, 288),
    (640, 512),
    (1024, 768),
]
RAW_DTYPE = "<u2"       # RAW16 Little Endian
DEFAULT_BIT_DEPTH = 14
BIT_DEPTH_OPTIONS = [8, 10, 12, 14, 16]
NOVATEK_NUC_GAIN_SHIFT = 8
NOVATEK_NUC_GAIN_ONE = 1 << NOVATEK_NUC_GAIN_SHIFT
DEFAULT_CAMERA_INDEX = 1
CAMERA_INDEX_OPTIONS = list(range(10))
DEFAULT_UVC_FPS = 30
UVC_CAPTURE_FOURCC = "MJPG"
UVC_FRAME_WAIT_SECONDS = 0.5
UVC_RECONNECT_SECONDS = 5.0
UVC_CLOSE_TIMEOUT_MS = 3000
BPR_MARK_RADIUS = 5
IMAGE_ALGORITHM_OPTIONS = [
    ("Percentile Auto Contrast", "auto_contrast"),
    ("Fieldscale Local Adaptive AGC (Firmware Core)", "fieldscale"),
    ("Histogram Equalization", "hist_equalization"),
    ("CLAHE", "clahe"),
    ("Gamma Correction", "gamma"),
    ("Bilateral Denoise", "bilateral"),
    ("Unsharp Mask", "unsharp"),
    ("Auto Contrast + CLAHE", "auto_contrast_clahe"),
    ("P1/P99 Percentile Preview (Original Values)", "raw_percentile_preview"),
]
IMAGE_ALGORITHM_DESCRIPTIONS = {
    "auto_contrast": (
        "在 8-bit 工作影像上計算 P1/P99，將 P1 映射為黑色、P99 映射為白色，"
        "範圍外像素裁切；不包含 CLAHE。"
    ),
    "fieldscale": (
        "Firmware-matching Fieldscale core: builds 8x8 local Min/Max fields, "
        "runs seven message-passing iterations, and locally maps the input range "
        "to the full display range. Gamma and CLAHE are not applied. RAW Compare "
        "uses original DN; UVC Compare uses the available UVC grayscale domain."
    ),
    "hist_equalization": (
        "對整張 8-bit 影像執行全域直方圖均衡化，重新分配灰階以提升整體對比。"
    ),
    "clahe": (
        "將 8-bit 影像分區做限制對比的局部直方圖均衡化；Clip Limit 限制放大量，"
        "Grid Size 控制分區數量。"
    ),
    "gamma": (
        "以 Gamma 曲線重新映射 8-bit 灰階；目前公式中 Gamma 小於 1 會提亮中間灰階，"
        "大於 1 會壓暗。"
    ),
    "bilateral": (
        "在 8-bit 影像上做保邊雙邊濾波，降低雜訊並盡量保留物體邊緣。"
    ),
    "unsharp": (
        "以原圖減去模糊影像取得高頻細節，再依 Amount 加回原圖以增強邊緣。"
    ),
    "auto_contrast_clahe": (
        "先在 8-bit 工作影像做 P1/P99 Auto Contrast，再執行 CLAHE 局部對比增強。"
    ),
    "raw_percentile_preview": (
        "直接在輸入的原始數值上計算 P1/P99。P1 是排除最低 1% 像素後的灰階下限，"
        "P99 是排除最高 1% 像素前的灰階上限；將 P1 映射為黑色 0、P99 映射為白色 255，"
        "範圍外的像素裁切，最後輸出成 8-bit 顯示影像。"
    ),
}


def timestamp():
    return datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]


def unique_result_dir(base_dir, function_name):
    root = Path(base_dir).expanduser().resolve() if base_dir else Path.cwd() / "Thermal_RAW_Results"
    function_root = root / function_name
    function_root.mkdir(parents=True, exist_ok=True)
    out = function_root / timestamp()
    counter = 1
    original = out
    while out.exists():
        out = Path(f"{original}_{counter:02d}")
        counter += 1
    out.mkdir(parents=True, exist_ok=False)
    return out


def load_raw(path, width, height):
    expected_pixels = width * height
    expected_bytes = expected_pixels * 2
    size = os.path.getsize(path)
    if size != expected_bytes:
        raise ValueError(
            f"RAW size error:\n{path}\nActual: {size:,} bytes\nExpected: {expected_bytes:,} bytes "
            f"({width}x{height}x2)"
        )
    raw = np.fromfile(path, dtype=RAW_DTYPE)
    return raw.reshape((height, width))


def find_raw_files(folder):
    files = []
    for ext in ("*.raw", "*.RAW", "*.bin", "*.BIN"):
        files.extend(glob.glob(os.path.join(folder, ext)))
    return sorted(set(files))


def average_raw_folder(folder, width, height):
    files = find_raw_files(folder)
    if not files:
        raise FileNotFoundError(f"No RAW/BIN files found:\n{folder}")
    total = np.zeros((height, width), dtype=np.float64)
    valid = []
    skipped = []
    for path in files:
        try:
            total += load_raw(path, width, height)
            valid.append(path)
        except Exception as exc:
            skipped.append((path, str(exc)))
    if not valid:
        raise ValueError("No valid RAW files in selected folder.")
    avg = np.rint(total / len(valid)).astype(np.uint16)
    return avg, valid, skipped


def raw_to_display_u8(image, raw_max):
    scaled = np.clip(image, 0, raw_max).astype(np.float64) * 255.0 / raw_max
    return np.rint(scaled).astype(np.uint8)


def signed_delta_to_display_u8(image, raw_max):
    scaled = (
        np.clip(image, -raw_max, raw_max).astype(np.float64) + raw_max
    ) * 255.0 / (2.0 * raw_max)
    return np.rint(scaled).astype(np.uint8)


def percentile_stretch_gray(gray, low_p=1.0, high_p=99.0):
    gray_f = gray.astype(np.float32)
    vmin = float(np.percentile(gray_f, low_p))
    vmax = float(np.percentile(gray_f, high_p))
    if vmax <= vmin:
        return gray.copy(), vmin, vmax
    stretched = (gray_f - vmin) * 255.0 / (vmax - vmin)
    return np.clip(stretched, 0, 255).astype(np.uint8), vmin, vmax


def percentile_preview_u8(image, p_low=1.0, p_high=99.0):
    arr = image.astype(np.float64)
    vmin = float(np.percentile(arr, p_low))
    vmax = float(np.percentile(arr, p_high))
    if vmax <= vmin:
        vmin = float(arr.min())
        vmax = float(arr.max())
    if vmax <= vmin:
        return np.zeros(arr.shape, dtype=np.uint8), vmin, vmax
    stretched = (arr - vmin) * 255.0 / (vmax - vmin)
    return np.clip(stretched, 0, 255).astype(np.uint8), vmin, vmax


FIELDSCALE_GRID_SIZE = 8
FIELDSCALE_RAW14_MAX = 16383
FIELDSCALE_DIFF_RAW14 = 400
FIELDSCALE_ITERATIONS = 7
FIELDSCALE_Q8_ONE = 256
FIELDSCALE_INTERP_ONE = 65536


def _fieldscale_suppress_extrema(grid, threshold_q8, suppress_maximum):
    result = grid.copy()
    height, width = grid.shape
    for y in range(height):
        for x in range(width):
            neighbors = []
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    if dx == 0 and dy == 0:
                        continue
                    nx = x + dx
                    ny = y + dy
                    if 0 <= nx < width and 0 <= ny < height:
                        neighbors.append(int(grid[ny, nx]))
            if not neighbors:
                continue
            current = int(grid[y, x])
            mean_value = sum(neighbors) // len(neighbors)
            if suppress_maximum:
                if (
                    current >= max(neighbors)
                    and current > mean_value
                    and current - mean_value > threshold_q8
                ):
                    result[y, x] = mean_value + threshold_q8
            elif (
                current <= min(neighbors)
                and mean_value > current
                and mean_value - current > threshold_q8
            ):
                result[y, x] = mean_value - threshold_q8
    return result


def _fieldscale_message_pass(grid, increase):
    result = grid.copy()
    height, width = grid.shape
    for y in range(height):
        for x in range(width):
            values = []
            for dy in range(-1, 2):
                for dx in range(-1, 2):
                    nx = x + dx
                    ny = y + dy
                    if 0 <= nx < width and 0 <= ny < height:
                        values.append(int(grid[ny, nx]))
            current = int(grid[y, x])
            mean_value = sum(values) // len(values)
            result[y, x] = (
                max(mean_value, current) if increase else min(mean_value, current)
            )
    return result


def _fieldscale_resize_q8(grid, output_width, output_height):
    grid_height, grid_width = grid.shape
    x_start_q16 = grid_width * 32768 // output_width - 32768
    x_step_q16 = grid_width * FIELDSCALE_INTERP_ONE // output_width
    y_start_q16 = grid_height * 32768 // output_height - 32768
    y_step_q16 = grid_height * FIELDSCALE_INTERP_ONE // output_height
    source_x = x_start_q16 + np.arange(output_width, dtype=np.int64) * x_step_q16
    source_y = y_start_q16 + np.arange(output_height, dtype=np.int64) * y_step_q16
    source_x = np.clip(source_x, 0, (grid_width - 1) * FIELDSCALE_INTERP_ONE)
    source_y = np.clip(source_y, 0, (grid_height - 1) * FIELDSCALE_INTERP_ONE)
    x0 = (source_x >> 16).astype(np.intp)
    y0 = (source_y >> 16).astype(np.intp)
    x1 = np.minimum(x0 + 1, grid_width - 1)
    y1 = np.minimum(y0 + 1, grid_height - 1)
    fx = source_x & 0xFFFF
    fy = source_y & 0xFFFF

    horizontal = (
        grid[:, x0] * (FIELDSCALE_INTERP_ONE - fx)[None, :]
        + grid[:, x1] * fx[None, :]
        + FIELDSCALE_INTERP_ONE // 2
    ) // FIELDSCALE_INTERP_ONE
    return (
        horizontal[y0, :] * (FIELDSCALE_INTERP_ONE - fy)[:, None]
        + horizontal[y1, :] * fy[:, None]
        + FIELDSCALE_INTERP_ONE // 2
    ) // FIELDSCALE_INTERP_ONE


def fieldscale_firmware_preview_u8(image, input_max, state=None, temporal=False):
    if image.ndim != 2:
        raise ValueError("Fieldscale input must be a 2D grayscale image.")
    height, width = image.shape
    if (
        width < FIELDSCALE_GRID_SIZE
        or height < FIELDSCALE_GRID_SIZE
        or width % FIELDSCALE_GRID_SIZE
        or height % FIELDSCALE_GRID_SIZE
    ):
        raise ValueError("Fieldscale requires width and height divisible by 8.")
    if input_max <= 0:
        raise ValueError("Fieldscale input maximum must be positive.")

    source = np.clip(image, 0, input_max).astype(np.int64)
    patch_height = height // FIELDSCALE_GRID_SIZE
    patch_width = width // FIELDSCALE_GRID_SIZE
    patches = source.reshape(
        FIELDSCALE_GRID_SIZE,
        patch_height,
        FIELDSCALE_GRID_SIZE,
        patch_width,
    )
    min_grid = patches.min(axis=(1, 3)) * FIELDSCALE_Q8_ONE
    max_grid = patches.max(axis=(1, 3)) * FIELDSCALE_Q8_ONE
    threshold_q8 = max(
        1,
        int(round(
            FIELDSCALE_DIFF_RAW14 * input_max * FIELDSCALE_Q8_ONE
            / FIELDSCALE_RAW14_MAX
        )),
    )
    max_grid = _fieldscale_suppress_extrema(max_grid, threshold_q8, True)
    max_grid = _fieldscale_suppress_extrema(max_grid, threshold_q8, False)
    for _ in range(FIELDSCALE_ITERATIONS):
        min_grid = _fieldscale_message_pass(min_grid, False)
        max_grid = _fieldscale_message_pass(max_grid, True)

    temporal_applied = False
    if temporal and state is not None:
        previous_min = state.get("min_grid")
        previous_max = state.get("max_grid")
        previous_input_max = state.get("input_max")
        if (
            previous_min is not None
            and previous_max is not None
            and previous_min.shape == min_grid.shape
            and previous_max.shape == max_grid.shape
            and previous_input_max == input_max
        ):
            min_grid = (min_grid + 9 * previous_min + 5) // 10
            max_grid = (max_grid + 9 * previous_max + 5) // 10
            temporal_applied = True
        state["min_grid"] = min_grid.copy()
        state["max_grid"] = max_grid.copy()
        state["input_max"] = input_max

    min_field = _fieldscale_resize_q8(min_grid, width, height)
    max_field = _fieldscale_resize_q8(max_grid, width, height)
    low = np.minimum(min_field, max_field)
    high = np.maximum(min_field, max_field)
    value_q8 = source * FIELDSCALE_Q8_ONE
    output = np.zeros(source.shape, dtype=np.int64)
    output[value_q8 >= high] = input_max
    valid = (value_q8 > low) & (value_q8 < high) & (high > low)
    output[valid] = (
        (value_q8[valid] - low[valid]) * input_max
        // (high[valid] - low[valid])
    )
    preview = np.rint(output.astype(np.float64) * 255.0 / input_max).astype(np.uint8)
    threshold_levels = threshold_q8 / FIELDSCALE_Q8_ONE
    details = [
        f"Fieldscale firmware core: grid=8x8, iterations={FIELDSCALE_ITERATIONS}",
        f"max/min difference={FIELDSCALE_DIFF_RAW14} RAW14 DN "
        f"({threshold_levels:.2f} input levels)",
        "Gamma=off, CLAHE=off",
    ]
    if temporal:
        details.append(
            "temporal=current 10% / previous 90%"
            if temporal_applied
            else "temporal=first frame"
        )
    return preview, details


def apply_image_algorithm(
    gray,
    algorithm_key,
    low_percentile=1.0,
    high_percentile=99.0,
    clahe_clip=2.0,
    clahe_grid=8,
    gamma=0.7,
    bilateral_strength=2,
    unsharp_amount=1.0,
    clahe=None,
    input_max=255,
    fieldscale_state=None,
    fieldscale_temporal=False,
):
    if cv2 is None:
        raise RuntimeError("OpenCV not installed. pip install opencv-python")
    if gray.ndim != 2:
        raise ValueError("Image algorithm input must be a 2D grayscale image.")

    details = []

    if algorithm_key == "raw_percentile_preview":
        processed, vmin, vmax = percentile_preview_u8(gray)
        details.append(f"Original-value P1.0={vmin:.1f}, P99.0={vmax:.1f}")
        return processed, details

    if algorithm_key == "fieldscale":
        return fieldscale_firmware_preview_u8(
            gray,
            input_max,
            state=fieldscale_state,
            temporal=fieldscale_temporal,
        )

    processed = np.ascontiguousarray(np.clip(gray, 0, 255).astype(np.uint8))

    if algorithm_key in ("auto_contrast", "auto_contrast_clahe"):
        processed, vmin, vmax = percentile_stretch_gray(
            processed, low_percentile, high_percentile
        )
        details.append(
            f"P{low_percentile:.1f}={vmin:.1f}, P{high_percentile:.1f}={vmax:.1f}"
        )

    if algorithm_key in ("clahe", "auto_contrast_clahe"):
        if clahe is None:
            clahe = cv2.createCLAHE(
                clipLimit=clahe_clip,
                tileGridSize=(clahe_grid, clahe_grid),
            )
        processed = clahe.apply(processed)
        details.append(f"CLAHE clip={clahe_clip:.1f}, grid={clahe_grid}x{clahe_grid}")

    if algorithm_key == "hist_equalization":
        processed = cv2.equalizeHist(processed)
        details.append("Global histogram equalization")
    elif algorithm_key == "gamma":
        lut = np.array(
            [((value / 255.0) ** gamma) * 255.0 for value in range(256)],
            dtype=np.uint8,
        )
        processed = cv2.LUT(processed, lut)
        details.append(f"Gamma={gamma:.1f}")
    elif algorithm_key == "bilateral":
        diameter = bilateral_strength * 2 + 1
        sigma = bilateral_strength * 20
        processed = cv2.bilateralFilter(processed, diameter, sigma, sigma)
        details.append(f"Bilateral strength={bilateral_strength}")
    elif algorithm_key == "unsharp":
        blurred = cv2.GaussianBlur(processed, (0, 0), 1.0)
        processed = cv2.addWeighted(
            processed, 1.0 + unsharp_amount, blurred, -unsharp_amount, 0
        )
        details.append(f"Unsharp amount={unsharp_amount:.1f}")

    if not details:
        raise ValueError(f"Unknown image algorithm: {algorithm_key}")
    return processed, details


def uvc_to_gray(frame):
    if frame.ndim == 2:
        return frame
    if frame.shape[2] == 1:
        return frame[:, :, 0]
    if frame.shape[2] == 4:
        return cv2.cvtColor(frame, cv2.COLOR_BGRA2GRAY)
    return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)


def uvc_gray_to_nuc_dn(gray, raw_max):
    """Convert a UVC grayscale frame into the DN domain used by the NUC table."""
    if gray.ndim != 2 or not np.issubdtype(gray.dtype, np.integer):
        raise ValueError("UVC NUC requires a 2D integer grayscale frame.")
    if gray.dtype.itemsize == 1:
        scaled = gray.astype(np.float64) * raw_max / 255.0
        return np.rint(scaled).astype(np.uint16), f"8-bit UVC mapped to 0..{raw_max} DN"
    return np.clip(gray, 0, raw_max).astype(np.uint16), f"{gray.dtype} UVC DN"


def image_stats(image):
    arr = np.asarray(image, dtype=np.float64)
    return {
        "count": int(arr.size),
        "min": float(arr.min()),
        "max": float(arr.max()),
        "mean": float(arr.mean()),
        "median": float(np.median(arr)),
        "std": float(arr.std()),
        "p1": float(np.percentile(arr, 1.0)),
        "p99": float(np.percentile(arr, 99.0)),
    }


def calculate_nuc(low, high, min_delta):
    low_f = low.astype(np.float64)
    high_f = high.astype(np.float64)
    low_avg = low_f.mean()
    high_avg = high_f.mean()
    target_delta = high_avg - low_avg
    pixel_delta = high_f - low_f

    valid = pixel_delta > min_delta
    gain = np.ones_like(low_f, dtype=np.float64)
    offset = np.zeros_like(low_f, dtype=np.float64)
    gain[valid] = target_delta / pixel_delta[valid]
    offset[valid] = low_avg - gain[valid] * low_f[valid]
    return gain, offset, valid, pixel_delta, low_avg, high_avg


def convert_additive_b_to_gfx_b(old_b, stored_k):
    old_b_i64 = np.asarray(old_b, dtype=np.int64)
    stored_k_i64 = np.asarray(stored_k, dtype=np.int64)
    gfx_b = np.zeros(old_b_i64.shape, dtype=np.int64)
    nonzero = stored_k_i64 != 0
    numerator = -(old_b_i64[nonzero] * NOVATEK_NUC_GAIN_ONE)
    denominator = stored_k_i64[nonzero]
    rounded = (np.abs(numerator) + denominator // 2) // denominator
    gfx_b[nonzero] = np.where(numerator < 0, -rounded, rounded)
    return np.clip(gfx_b, -32768, 32767).astype(np.int16)


def build_novatek_nuc_tables(low, high, min_delta):
    low_i64 = np.asarray(low, dtype=np.int64)
    high_i64 = np.asarray(high, dtype=np.int64)
    pixel_count = low_i64.size
    low_avg = int(low_i64.sum() // pixel_count)
    high_avg = int(high_i64.sum() // pixel_count)
    target_delta = high_avg - low_avg
    if target_delta <= 0:
        raise ValueError(
            f"Novatek NUC requires HIGH average > LOW average; got LOW={low_avg}, HIGH={high_avg}."
        )

    pixel_delta = high_i64 - low_i64
    valid = pixel_delta > min_delta
    stored_k_i64 = np.full(low_i64.shape, NOVATEK_NUC_GAIN_ONE, dtype=np.int64)
    raw_k = np.zeros(low_i64.shape, dtype=np.int64)
    raw_k[valid] = (target_delta << NOVATEK_NUC_GAIN_SHIFT) // pixel_delta[valid]
    table_valid = valid & (raw_k > 0)
    stored_k_i64[table_valid] = np.clip(raw_k[table_valid], 1, 65535)

    old_b = np.zeros(low_i64.shape, dtype=np.int64)
    old_b[table_valid] = low_avg - (
        (stored_k_i64[table_valid] * low_i64[table_valid]) >> NOVATEK_NUC_GAIN_SHIFT
    )
    gfx_b = np.zeros(low_i64.shape, dtype=np.int16)
    gfx_b[table_valid] = convert_additive_b_to_gfx_b(
        old_b[table_valid], stored_k_i64[table_valid]
    )
    stored_k = stored_k_i64.astype(np.uint16)
    return stored_k, gfx_b, table_valid, low_avg, high_avg


def apply_novatek_nuc(raw, stored_k, gfx_b, raw_max):
    corrected = (
        stored_k.astype(np.int64) * (raw.astype(np.int64) - gfx_b.astype(np.int64))
    ) >> NOVATEK_NUC_GAIN_SHIFT
    return np.clip(corrected, 0, raw_max).astype(np.uint16)


def save_csv_matrix(path, data, fmt):
    np.savetxt(path, data, delimiter=",", fmt=fmt)


def load_novatek_nuc_bin(path, width, height, dtype, table_name):
    if not path:
        raise ValueError(f"Select the Novatek {table_name} BIN file.")
    expected_bytes = width * height * np.dtype(dtype).itemsize
    actual_bytes = os.path.getsize(path)
    if actual_bytes != expected_bytes:
        raise ValueError(
            f"{table_name} BIN size error:\n{path}\nActual: {actual_bytes:,} bytes\n"
            f"Expected: {expected_bytes:,} bytes ({width}x{height}x2)"
        )
    return np.fromfile(path, dtype=dtype).reshape((height, width))


def deduplicate_coordinates(coordinates):
    unique = []
    seen = set()
    for x, y in coordinates:
        coordinate = (int(x), int(y))
        if coordinate in seen:
            continue
        seen.add(coordinate)
        unique.append(coordinate)
    return unique


def load_bpr_sie_coordinates(path):
    if not path:
        raise ValueError("Select a BPR dpc_table.bin file.")
    actual_bytes = os.path.getsize(path)
    if actual_bytes < 4 or actual_bytes % 2 != 0:
        raise ValueError(
            f"Invalid BPR BIN size: {actual_bytes:,} bytes. Expected an even-sized SIE table."
        )

    words = np.fromfile(path, dtype="<u2")
    coordinates = []
    current_y = None
    terminator_word = None
    for word_index, raw_value in enumerate(words):
        value = int(raw_value)
        if value == 0xFFFF:
            terminator_word = word_index
            break
        if value & 0x8000:
            current_y = value & 0x7FFF
            continue
        if current_y is None:
            raise ValueError(
                f"Invalid BPR SIE table: X={value} appears before a Y row marker at word {word_index}."
            )
        coordinates.append((value, current_y))

    if terminator_word is None:
        raise ValueError("Invalid BPR SIE table: 0xFFFF terminator was not found.")
    if terminator_word + 1 >= words.size or int(words[terminator_word + 1]) != 0:
        raise ValueError("Invalid BPR SIE table: expected 0x0000 after the 0xFFFF terminator.")

    unique = deduplicate_coordinates(coordinates)
    return unique, {
        "format": "Novatek SIE dpc_table.bin",
        "entry_count": len(coordinates),
        "unique_count": len(unique),
        "duplicate_count": len(coordinates) - len(unique),
        "terminator_word": terminator_word,
    }


def _parse_coordinate_integer(token):
    token = token.strip().strip("()[]{}")
    if token.lower().startswith(("0x", "+0x", "-0x")):
        return int(token, 16)
    return int(token, 10)


def load_bpr_text_coordinates(path):
    if not path:
        raise ValueError("Select a BPR coordinate CSV/TXT file.")
    coordinates = []
    with open(path, "r", encoding="utf-8-sig") as source:
        for line_number, original_line in enumerate(source, 1):
            line = original_line.split("#", 1)[0].split("//", 1)[0].strip()
            if not line:
                continue
            fields = [field for field in re.split(r"[,;\s]+", line) if field]
            if len(fields) < 2:
                raise ValueError(
                    f"Invalid coordinate at line {line_number}: expected x,y."
                )
            try:
                x = _parse_coordinate_integer(fields[0])
                y = _parse_coordinate_integer(fields[1])
            except ValueError as exc:
                if (
                    not coordinates
                    and fields[0].lower() == "x"
                    and fields[1].lower() == "y"
                ):
                    continue
                raise ValueError(
                    f"Invalid coordinate at line {line_number}: {original_line.strip()}"
                ) from exc
            coordinates.append((x, y))

    if not coordinates:
        raise ValueError("The BPR coordinate file does not contain any x,y coordinates.")
    unique = deduplicate_coordinates(coordinates)
    return unique, {
        "format": "Text coordinates (x,y)",
        "entry_count": len(coordinates),
        "unique_count": len(unique),
        "duplicate_count": len(coordinates) - len(unique),
        "terminator_word": None,
    }


def load_bpr_coordinates(path):
    suffix = Path(path).suffix.lower() if path else ""
    if suffix == ".bin":
        return load_bpr_sie_coordinates(path)
    if suffix in (".csv", ".txt"):
        return load_bpr_text_coordinates(path)
    raise ValueError("BPR coordinates must be a Novatek .bin table or an x,y .csv/.txt file.")


def split_coordinates_by_bounds(coordinates, width, height):
    valid = []
    outside = []
    for coordinate in coordinates:
        x, y = coordinate
        if 0 <= x < width and 0 <= y < height:
            valid.append(coordinate)
        else:
            outside.append(coordinate)
    return valid, outside


def format_bpr_coordinate_report(table_info, coordinates, outside_coordinates, width, height):
    outside_set = set(outside_coordinates)
    marked_count = len(coordinates) - len(outside_coordinates)
    lines = [
        f"Table entries           : {table_info['entry_count']}",
        f"Unique bad pixels       : {table_info['unique_count']}",
        f"Duplicate entries       : {table_info['duplicate_count']}",
        f"Marked in {width}x{height}      : {marked_count}",
        f"Out-of-bounds           : {len(outside_coordinates)}",
        "",
        "All unique coordinates (top-left origin; OUT = not marked):",
    ]
    coordinate_tokens = []
    index_width = max(3, len(str(len(coordinates))))
    for index, (x, y) in enumerate(coordinates, 1):
        suffix = " [OUT]" if (x, y) in outside_set else ""
        coordinate_tokens.append(
            f"{index:0{index_width}d}:({x},{y}){suffix}"
        )
    for start in range(0, len(coordinate_tokens), 4):
        lines.append("    ".join(coordinate_tokens[start:start + 4]))
    return "\n".join(lines)


def draw_bpr_markers(base_gray, coordinates, radius=BPR_MARK_RADIUS):
    if cv2 is None:
        raise RuntimeError("OpenCV is required for BPR marking. Install: pip install opencv-python")
    if base_gray.ndim != 2:
        raise ValueError("BPR marker base image must be a 2D grayscale image.")
    marked_rgb = cv2.cvtColor(
        np.ascontiguousarray(np.clip(base_gray, 0, 255).astype(np.uint8)),
        cv2.COLOR_GRAY2RGB,
    )
    for x, y in coordinates:
        cv2.circle(
            marked_rgb,
            (int(x), int(y)),
            int(radius),
            (255, 0, 0),
            1,
            cv2.LINE_AA,
        )
    return marked_rgb


def save_png(path, image):
    if cv2 is None:
        raise RuntimeError("OpenCV is required to save PNG. Install: pip install opencv-python")
    ok = cv2.imwrite(str(path), image)
    if not ok:
        raise RuntimeError(f"Failed to save image: {path}")


def save_rgb_png(path, image_rgb):
    if cv2 is None:
        raise RuntimeError("OpenCV is required to save PNG. Install: pip install opencv-python")
    if image_rgb.ndim != 3 or image_rgb.shape[2] < 3:
        raise ValueError("RGB PNG output requires a three-channel image.")
    bgr = cv2.cvtColor(np.ascontiguousarray(image_rgb[:, :, :3]), cv2.COLOR_RGB2BGR)
    ok = cv2.imwrite(str(path), bgr)
    if not ok:
        raise RuntimeError(f"Failed to save image: {path}")


def save_uvc_comparison_png(path, original, processed, algorithm_title):
    if cv2 is None:
        raise RuntimeError("OpenCV is required to save PNG. Install: pip install opencv-python")
    if original.ndim != 2 or processed.ndim != 2:
        raise ValueError("UVC comparison PNG requires two grayscale images.")
    if original.shape != processed.shape:
        raise ValueError(
            f"UVC comparison image size mismatch: original={original.shape}, processed={processed.shape}"
        )

    original_u8 = np.ascontiguousarray(np.clip(original, 0, 255).astype(np.uint8))
    processed_u8 = np.ascontiguousarray(np.clip(processed, 0, 255).astype(np.uint8))
    height, width = original_u8.shape
    header_height = 42
    gap = 8
    canvas = np.full((height + header_height, width * 2 + gap), 24, dtype=np.uint8)
    canvas[header_height:, :width] = original_u8
    canvas[header_height:, width + gap:] = processed_u8

    labels = ("Original UVC", f"Processed: {algorithm_title}")
    panel_x = (0, width + gap)
    for label, x_offset in zip(labels, panel_x):
        font_scale = min(0.7, max(0.42, width / max(420.0, len(label) * 18.0)))
        text_size, _ = cv2.getTextSize(
            label, cv2.FONT_HERSHEY_SIMPLEX, font_scale, 1
        )
        text_x = x_offset + max(6, (width - text_size[0]) // 2)
        cv2.putText(
            canvas,
            label,
            (text_x, 28),
            cv2.FONT_HERSHEY_SIMPLEX,
            font_scale,
            255,
            1,
            cv2.LINE_AA,
        )

    save_png(path, canvas)


class PreviewLabel(QLabel):
    def __init__(self, title):
        super().__init__()
        self.title = title
        self._pixmap = None
        self.setAlignment(Qt.AlignCenter)
        self.setMinimumSize(360, 200)
        self.setStyleSheet("background:#121417; color:#aeb7c2; border:1px solid #424850; border-radius:8px; font-size:15px; font-weight:600;")
        self.setText("Waiting for image...")

    def set_title(self, title):
        self.title = title
        if self._pixmap is None:
            self.setText("Waiting for image...")

    def set_array(self, array):
        if array is None:
            self._pixmap = None
            self.setText("Waiting for image...")
            return
        if array.ndim == 2:
            img8 = np.ascontiguousarray(np.clip(array, 0, 255).astype(np.uint8))
            qimg = QImage(img8.data, img8.shape[1], img8.shape[0], img8.strides[0], QImage.Format_Grayscale8).copy()
        else:
            rgb = np.ascontiguousarray(array[:, :, :3])
            qimg = QImage(rgb.data, rgb.shape[1], rgb.shape[0], rgb.strides[0], QImage.Format_RGB888).copy()
        self._pixmap = QPixmap.fromImage(qimg)
        self._rescale()

    def _rescale(self):
        if self._pixmap:
            self.setPixmap(self._pixmap.scaled(self.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation))

    def resizeEvent(self, event):
        self._rescale()
        super().resizeEvent(event)


class PathRow(QWidget):
    path_selected = pyqtSignal(str)

    def __init__(self, mode="file", filter_text="All Files (*)", dialog_title=None):
        super().__init__()
        self.mode = mode
        self.filter_text = filter_text
        self.dialog_title = dialog_title or ("Select Folder" if mode == "folder" else "Select File")
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        self.edit = QLineEdit()
        self.edit.setPlaceholderText(self.dialog_title)
        self.btn = QPushButton("Browse")
        self.btn.setProperty("buttonRole", "browse")
        self.btn.setToolTip(self.dialog_title)
        self.btn.clicked.connect(self.browse)
        lay.addWidget(self.edit, 1)
        lay.addWidget(self.btn)

    def browse(self):
        if self.mode == "folder":
            p = QFileDialog.getExistingDirectory(self, self.dialog_title)
        else:
            p, _ = QFileDialog.getOpenFileName(self, self.dialog_title, "", self.filter_text)
        if p:
            self.edit.setText(p)
            self.path_selected.emit(p)

    def text(self):
        return self.edit.text().strip()

    def setText(self, text):
        self.edit.setText(text)


class UvcCaptureThread(QThread):
    frame_ready = pyqtSignal(object)
    camera_opened = pyqtSignal(int, int, int, float)
    status_changed = pyqtSignal(str)

    def __init__(self, camera_index, width, height, fps, reconnect_seconds, parent=None):
        super().__init__(parent)
        self.camera_index = camera_index
        self.width = width
        self.height = height
        self.fps = fps
        self.reconnect_seconds = reconnect_seconds
        self._stop_event = threading.Event()
        self._reconnect_event = threading.Event()
        self._capture_lock = threading.Lock()
        self._capture = None

    def stop(self):
        self._stop_event.set()

    def force_reconnect(self):
        self._reconnect_event.set()

    def _set_capture(self, capture):
        with self._capture_lock:
            self._capture = capture

    def _clear_capture(self, capture):
        with self._capture_lock:
            if self._capture is capture:
                self._capture = None
        try:
            capture.release()
        except Exception:
            pass

    def _open_capture(self):
        if sys.platform.startswith("win"):
            mjpg_fourcc = cv2.VideoWriter_fourcc(*UVC_CAPTURE_FOURCC)
            open_params = [
                cv2.CAP_PROP_FRAME_WIDTH,
                self.width,
                cv2.CAP_PROP_FRAME_HEIGHT,
                self.height,
                cv2.CAP_PROP_FOURCC,
                mjpg_fourcc,
            ]
            try:
                # DirectShow applies these values before starting the device.
                # Setting CAP_PROP_FPS afterward can reopen it without FOURCC
                # and make the Novatek host request format=0 instead of MJPEG.
                capture = cv2.VideoCapture(
                    self.camera_index,
                    cv2.CAP_DSHOW,
                    open_params,
                )
            except (TypeError, cv2.error):
                capture = cv2.VideoCapture(self.camera_index, cv2.CAP_DSHOW)

            if capture.isOpened():
                return capture

            capture.release()
            capture = cv2.VideoCapture(self.camera_index, cv2.CAP_DSHOW)
            if capture.isOpened():
                capture.set(cv2.CAP_PROP_FOURCC, mjpg_fourcc)
                capture.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
                capture.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
                capture.set(cv2.CAP_PROP_FOURCC, mjpg_fourcc)
            return capture

        capture = cv2.VideoCapture(self.camera_index)
        if capture.isOpened():
            capture.set(
                cv2.CAP_PROP_FOURCC,
                cv2.VideoWriter_fourcc(*UVC_CAPTURE_FOURCC),
            )
            capture.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
            capture.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
            capture.set(cv2.CAP_PROP_FPS, self.fps)
        return capture

    @staticmethod
    def _fourcc_text(value):
        fourcc = int(value)
        return "".join(chr((fourcc >> (8 * index)) & 0xFF) for index in range(4))

    def run(self):
        capture = None
        try:
            while not self._stop_event.is_set():
                self._reconnect_event.clear()
                self.status_changed.emit(f"Opening camera {self.camera_index}...")
                try:
                    capture = self._open_capture()
                except Exception as exc:
                    capture = None
                    self.status_changed.emit(f"Camera open error: {exc}; retrying...")

                if capture is None or not capture.isOpened():
                    if capture is not None:
                        self._clear_capture(capture)
                        capture = None
                    self.status_changed.emit(f"Cannot open camera {self.camera_index}; retrying...")
                    if self._stop_event.wait(1.0):
                        break
                    continue

                self._set_capture(capture)
                if not sys.platform.startswith("win") and hasattr(cv2, "CAP_PROP_BUFFERSIZE"):
                    capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
                if not sys.platform.startswith("win") and hasattr(cv2, "CAP_PROP_READ_TIMEOUT_MSEC"):
                    capture.set(cv2.CAP_PROP_READ_TIMEOUT_MSEC, 1000)
                actual_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
                actual_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
                actual_fps = float(capture.get(cv2.CAP_PROP_FPS))
                actual_fourcc_value = int(capture.get(cv2.CAP_PROP_FOURCC))
                actual_fourcc = self._fourcc_text(actual_fourcc_value)
                if (
                    sys.platform.startswith("win")
                    and actual_fourcc != UVC_CAPTURE_FOURCC
                ):
                    self.status_changed.emit(
                        f"Camera {self.camera_index} negotiated {actual_fourcc!r}, "
                        f"expected {UVC_CAPTURE_FOURCC}; retrying..."
                    )
                    self._clear_capture(capture)
                    capture = None
                    if self._stop_event.wait(1.0):
                        break
                    continue
                self.status_changed.emit(
                    f"Camera {self.camera_index}: {actual_fourcc} "
                    f"{actual_width}x{actual_height} {actual_fps:.2f} FPS"
                )
                self.camera_opened.emit(self.camera_index, actual_width, actual_height, actual_fps)

                failed_since = None
                while not self._stop_event.is_set():
                    try:
                        ok, frame = capture.read()
                    except Exception:
                        ok, frame = False, None

                    if self._stop_event.is_set():
                        break
                    if self._reconnect_event.is_set():
                        self.status_changed.emit(f"Reconnecting camera {self.camera_index}...")
                        break
                    if ok and frame is not None and frame.size:
                        failed_since = None
                        self.frame_ready.emit(frame)
                        continue

                    now = time.perf_counter()
                    if failed_since is None:
                        failed_since = now
                    if now - failed_since >= self.reconnect_seconds:
                        self.status_changed.emit(
                            f"No frame for {self.reconnect_seconds:.1f}s; reconnecting camera {self.camera_index}..."
                        )
                        break
                    if self._stop_event.wait(0.03):
                        break

                self._clear_capture(capture)
                capture = None
                if not self._stop_event.is_set():
                    self._stop_event.wait(0.25)
        finally:
            if capture is not None:
                self._clear_capture(capture)


class ThermalRawToolkit(QMainWindow):
    FUNCTIONS = [
        "RAW Viewer",
        "Average RAW Folder",
        "NUC Calculate",
        "NUC Apply",
        "High / Low Analyzer",
        "Temporal Drift Analyzer",
        "BPR Bad Pixel Marking",
        "RAW Algorithm Compare",
        "UVC Algorithm Compare",
    ]
    FUNCTION_DESCRIPTIONS = [
        "選擇 RAW 檔後立即讀取並顯示影像，不需要按 Run。",
        "將資料夾內的 RAW 影像逐像素平均，並依 LOW／HIGH 後綴儲存平均結果。",
        "使用 LOW 與 HIGH RAW 計算 NUC 參數，產生 Novatek pointN_K.bin 與 pointN_B.bin；右側以固定 Bit Depth 比例驗證校正後 HIGH。",
        "讀取 RAW、pointN_K.bin 與 pointN_B.bin 執行 NUC 計算，顯示並儲存原始與 NUC 後的 RAW 和 PNG。",
        "逐像素比較 LOW 與 HIGH RAW，影像區顯示 HIGH - LOW Difference，並分析 DN 差異及異常像素。",
        "分析資料夾內多張 RAW 的時間變化，計算標準差與 Peak-to-Peak。",
        "讀取單張 RAW，以固定 Bit Depth 映射後套用所選影像演算法，並比較原始與處理結果。",
        "開啟 UVC Camera，可選擇先套用 Novatek K/B NUC table，再即時比較原始影像與影像演算法結果。",
    ]
    PREVIEW_LAYOUTS = [
        (False, "", "RAW Preview (Fixed Scale)"),
        (False, "", "Average RAW (Fixed Scale)"),
        (True, "HIGH Input (Fixed Scale)", "NUC Corrected HIGH (Fixed Scale)"),
        (True, "Original RAW (Fixed Scale)", "NUC Corrected RAW (Fixed Scale)"),
        (False, "", "HIGH - LOW Difference (Fixed Scale)"),
        (False, "", "Temporal Standard Deviation (Fixed Scale)"),
        (True, "RAW Fixed-Scale / White Canvas", "BPR Bad Pixels Marked"),
        (True, "Original RAW (Fixed Scale)", "Percentile Auto Contrast"),
        (True, "Original UVC", "Percentile Auto Contrast"),
    ]
    FUNCTION_DESCRIPTIONS.insert(
        6,
        "Load a Novatek BPR dpc_table.bin or x,y coordinate file. Optionally overlay bad-pixel circles on a RAW image; otherwise use a white canvas.",
    )

    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(1450, 900)
        self.current_original = None
        self.current_result = None
        self.last_output_dir = None
        self.uvc_worker = None
        self.uvc_original_source = None
        self.uvc_original_frame = None
        self.uvc_processed_frame = None
        self.uvc_nuc_k_table = None
        self.uvc_nuc_b_table = None
        self.uvc_nuc_cache_key = None
        self.uvc_last_processing_error = None
        self.uvc_frame_count = 0
        self.uvc_fps_display = 0.0
        self.uvc_last_time = 0.0
        self.uvc_last_frame_time = 0.0
        self.uvc_waiting_for_frame = False
        self.uvc_reconnect_requested = False
        self.uvc_restart_when_stopped = False
        self.uvc_close_pending = False
        self.uvc_clahe = None
        self.uvc_clahe_settings = None
        self.uvc_fieldscale_state = {}
        self.uvc_watchdog_timer = QTimer(self)
        self.uvc_watchdog_timer.setInterval(200)
        self.uvc_watchdog_timer.timeout.connect(self.check_uvc_stream)
        self._build_ui()
        self._apply_ui_style()

    def _apply_ui_style(self):
        self.setStyleSheet("""
            QMainWindow {
                background-color: #181a1e;
            }
            QWidget#centralWidget {
                background-color: #181a1e;
                color: #e7e9ed;
                font-family: "Segoe UI";
                font-size: 13px;
            }
            QWidget#controlPanel, QWidget#previewPanel {
                background-color: #22252a;
                border: 1px solid #3a3f47;
                border-radius: 10px;
            }
            QWidget#infoPanel {
                background-color: #202328;
                border: 1px solid #424850;
                border-radius: 8px;
            }
            QGroupBox#settingsCard {
                background-color: #22252a;
                border: 1px solid #3a3f47;
                border-radius: 10px;
            }
            QGroupBox#viewerStatsCard {
                background-color: #20252b;
                border: 1px solid #46515c;
                border-radius: 8px;
                margin-top: 10px;
                padding: 10px 8px 8px 8px;
                font-weight: 700;
                color: #c9e2f2;
            }
            QGroupBox#viewerStatsCard::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px;
            }
            QLabel[viewerStatValue="true"] {
                color: #ffffff;
                background-color: #191c21;
                border: 1px solid #3f4852;
                border-radius: 5px;
                padding: 5px 7px;
                font-family: "Cascadia Mono", "Consolas";
                font-weight: 700;
            }
            QLabel {
                color: #e0e3e8;
                background: transparent;
            }
            QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QTextEdit {
                background-color: #191c21;
                color: #f0f1f3;
                border: 1px solid #454b54;
                border-radius: 6px;
                padding: 6px 8px;
                selection-background-color: #4f8fbd;
            }
            QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QTextEdit:focus {
                border: 1px solid #86bce2;
            }
            QComboBox::drop-down {
                border: none;
                width: 24px;
            }
            QComboBox QAbstractItemView {
                background-color: #25292f;
                color: #f0f1f3;
                border: 1px solid #4a515b;
                selection-background-color: #4f8fbd;
                padding: 4px;
            }
            QPushButton {
                background-color: #3a4048;
                color: #f5f6f7;
                border: 1px solid #555d68;
                border-radius: 7px;
                padding: 7px 14px;
                font-weight: 600;
            }
            QPushButton:hover {
                background-color: #4a515b;
                border-color: #69727e;
            }
            QPushButton:pressed {
                background-color: #2d3239;
            }
            QPushButton:disabled {
                background-color: #292d33;
                color: #737a84;
                border-color: #383d44;
            }
            QPushButton#primaryButton {
                background-color: #3f7eae;
                border-color: #69a5d0;
            }
            QPushButton#primaryButton:hover {
                background-color: #4d90c1;
            }
            QPushButton#startButton, QPushButton#saveButton {
                background-color: #397851;
                border-color: #5ca476;
            }
            QPushButton#startButton:hover, QPushButton#saveButton:hover {
                background-color: #458c60;
            }
            QPushButton#stopButton {
                background-color: #984646;
                border-color: #c66a6a;
            }
            QPushButton#stopButton:hover {
                background-color: #ad5050;
            }
            QPushButton#captureButton {
                background-color: #70518c;
                border-color: #9874b5;
            }
            QPushButton#captureButton:hover {
                background-color: #80609d;
            }
            QPushButton#resetButton {
                background-color: #a96f2e;
                border-color: #d09a58;
                color: #fff8ef;
            }
            QPushButton#resetButton:hover {
                background-color: #bc7e35;
            }
            QPushButton#startButton:disabled, QPushButton#stopButton:disabled,
            QPushButton#captureButton:disabled, QPushButton#resetButton:disabled,
            QPushButton#saveButton:disabled {
                background-color: #292d33;
                color: #737a84;
                border-color: #383d44;
            }
            QPushButton[buttonRole="browse"] {
                background-color: #376f8d;
                border-color: #65a4c5;
                min-width: 66px;
            }
            QPushButton[buttonRole="browse"]:hover {
                background-color: #43819f;
            }
            QCheckBox {
                color: #e7e9ed;
                spacing: 7px;
            }
            QCheckBox::indicator {
                width: 17px;
                height: 17px;
            }
            QCheckBox::indicator:unchecked {
                background-color: #191c21;
                border: 1px solid #626a74;
                border-radius: 4px;
            }
            QCheckBox::indicator:checked {
                background-color: #4f8fbd;
                border: 1px solid #86bce2;
                border-radius: 4px;
            }
            QTextEdit {
                font-family: "Cascadia Mono", "Consolas";
                font-size: 17px;
            }
            QSplitter::handle {
                background: transparent;
            }
            QSplitter::handle:horizontal {
                width: 6px;
            }
            QSplitter::handle:vertical {
                height: 6px;
            }
            QToolTip {
                background-color: #30343a;
                color: #f5f6f7;
                border: 1px solid #555d68;
                padding: 5px;
            }
        """)

    def _build_ui(self):
        central = QWidget()
        central.setObjectName("centralWidget")
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(18, 16, 18, 18)
        root.setSpacing(12)

        top = QHBoxLayout()
        self.function_title = QLabel(self.FUNCTIONS[0])
        self.function_title.setStyleSheet("font-size:26px; font-weight:700; color:#86bce2;")
        top.addWidget(self.function_title)
        top.addStretch()
        top.addWidget(QLabel("Function:"))
        self.function_combo = QComboBox()
        self.function_combo.addItems(self.FUNCTIONS)
        self.function_combo.currentIndexChanged.connect(self.change_page)
        self.function_combo.setMinimumWidth(240)
        top.addWidget(self.function_combo)
        root.addLayout(top)

        settings = QGroupBox()
        settings.setObjectName("settingsCard")
        sg = QVBoxLayout(settings)
        sg.setContentsMargins(14, 14, 14, 14)
        sg.setSpacing(10)
        self.function_description = QLabel()
        self.function_description.setWordWrap(True)
        self.function_description.setStyleSheet("font-size:18px; font-weight:600; color:#e6edf2; background:#2b323a; border-left:4px solid #79afd3; border-radius:6px; padding:10px 12px;")
        sg.addWidget(self.function_description)
        settings_row = QHBoxLayout()
        self.resolution_combo = QComboBox()
        for width, height in RESOLUTION_OPTIONS:
            self.resolution_combo.addItem(f"{width} × {height}", (width, height))
        default_resolution = (DEFAULT_WIDTH, DEFAULT_HEIGHT)
        default_index = RESOLUTION_OPTIONS.index(default_resolution) if default_resolution in RESOLUTION_OPTIONS else 0
        self.resolution_combo.setCurrentIndex(max(default_index, 0))
        self.bit_depth_combo = QComboBox()
        for bits in BIT_DEPTH_OPTIONS:
            self.bit_depth_combo.addItem(f"{bits}-bit (Max {(1 << bits) - 1})", bits)
        default_bit_index = BIT_DEPTH_OPTIONS.index(DEFAULT_BIT_DEPTH) if DEFAULT_BIT_DEPTH in BIT_DEPTH_OPTIONS else 0
        self.bit_depth_combo.setCurrentIndex(default_bit_index)
        self.output_root = PathRow("folder", dialog_title="Select Output Root Folder")
        self.output_root.setText(str(Path.cwd() / "Thermal_RAW_Results"))
        settings_row.addWidget(QLabel("Resolution")); settings_row.addWidget(self.resolution_combo)
        settings_row.addWidget(QLabel("Bit Depth")); settings_row.addWidget(self.bit_depth_combo)
        settings_row.addWidget(QLabel("Output Root")); settings_row.addWidget(self.output_root, 1)
        sg.addLayout(settings_row)
        root.addWidget(settings)

        self.main_splitter = QSplitter(Qt.Horizontal)
        self.main_splitter.setHandleWidth(6)
        splitter = self.main_splitter
        root.addWidget(splitter, 1)

        left = QWidget(); left.setObjectName("controlPanel"); left_lay = QVBoxLayout(left)
        self.stack = QStackedWidget()
        left_lay.addWidget(self.stack)
        self.run_btn = QPushButton("Run / Preview")
        self.run_btn.setObjectName("primaryButton")
        self.run_btn.setMinimumHeight(44)
        self.run_btn.clicked.connect(self.run_current)
        left_lay.addWidget(self.run_btn)
        splitter.addWidget(left)

        right = QWidget(); right.setObjectName("previewPanel"); right_lay = QVBoxLayout(right)
        self.right_splitter = QSplitter(Qt.Vertical)
        self.right_splitter.setHandleWidth(6)
        self.right_splitter.setChildrenCollapsible(False)
        preview_area = QWidget()
        previews = QHBoxLayout(preview_area)
        previews.setContentsMargins(0, 0, 0, 0)
        previews.setSpacing(8)
        self.preview_original = PreviewLabel("Original / Input Preview")
        self.preview_result = PreviewLabel("Result Preview")
        self.preview_original_container = QWidget()
        original_preview_layout = QVBoxLayout(self.preview_original_container)
        original_preview_layout.setContentsMargins(0, 0, 0, 0)
        original_preview_layout.setSpacing(6)
        self.preview_original_caption = QLabel("Original / Input Preview")
        self.preview_original_caption.setAlignment(Qt.AlignCenter)
        self.preview_original_caption.setStyleSheet("font-size:16px; font-weight:700; color:#c9e2f2; background:#2b333b; border-left:4px solid #79afd3; border-radius:6px; padding:7px;")
        original_preview_layout.addWidget(self.preview_original_caption)
        original_preview_layout.addWidget(self.preview_original, 1)
        self.preview_result_container = QWidget()
        result_preview_layout = QVBoxLayout(self.preview_result_container)
        result_preview_layout.setContentsMargins(0, 0, 0, 0)
        result_preview_layout.setSpacing(6)
        self.preview_result_caption = QLabel("Result Preview")
        self.preview_result_caption.setAlignment(Qt.AlignCenter)
        self.preview_result_caption.setStyleSheet("font-size:16px; font-weight:700; color:#d4eadc; background:#29372f; border-left:4px solid #6aaa7d; border-radius:6px; padding:7px;")
        result_preview_layout.addWidget(self.preview_result_caption)
        result_preview_layout.addWidget(self.preview_result, 1)
        previews.addWidget(self.preview_original_container, 1)
        previews.addWidget(self.preview_result_container, 1)
        self.right_splitter.addWidget(preview_area)

        self.info_area = QWidget()
        self.info_area.setObjectName("infoPanel")
        info_layout = QVBoxLayout(self.info_area)
        info_layout.setContentsMargins(10, 8, 10, 10)
        info_layout.setSpacing(6)
        info_header = QHBoxLayout()
        self.info_title = QLabel("Information / Log")
        self.info_title.setStyleSheet("font-size:16px; font-weight:700; color:#d7dde5; padding:2px 4px;")
        self.log_font_size = QSpinBox()
        self.log_font_size.setRange(10, 28)
        self.log_font_size.setValue(17)
        self.log_font_size.setSuffix(" px")
        self.log_font_size.setFixedWidth(88)
        info_header.addWidget(self.info_title)
        info_header.addStretch()
        info_header.addWidget(QLabel("Font Size"))
        info_header.addWidget(self.log_font_size)
        info_layout.addLayout(info_header)
        self.log = QTextEdit(); self.log.setReadOnly(True); self.log.setMinimumHeight(120)
        self.log_font_size.valueChanged.connect(self.update_log_font_size)
        self.update_log_font_size(self.log_font_size.value())
        info_layout.addWidget(self.log, 1)
        self.right_splitter.addWidget(self.info_area)
        self.right_splitter.setStretchFactor(0, 3)
        self.right_splitter.setStretchFactor(1, 1)
        self.right_splitter.setSizes([520, 220])
        self.right_splitter.handle(1).setToolTip("Drag to resize the image and Information / Log areas")
        right_lay.addWidget(self.right_splitter, 1)
        action_row = QHBoxLayout()
        self.save_current_btn = QPushButton("Save Current Preview PNG")
        self.save_current_btn.setObjectName("saveButton")
        self.save_current_btn.clicked.connect(self.save_current_preview)
        self.open_result_btn = QPushButton("Open Last Result Folder")
        self.open_result_btn.setObjectName("openButton")
        self.open_result_btn.clicked.connect(self.open_last_result)
        action_row.addWidget(self.save_current_btn)
        action_row.addWidget(self.open_result_btn)
        action_row.addStretch()
        right_lay.addLayout(action_row)
        splitter.addWidget(right)
        splitter.setSizes([520, 900])

        self._create_pages()
        self.change_page(0)

    def update_log_font_size(self, size):
        self.log.setStyleSheet(f"font-size:{size}px;")
        if hasattr(self, "bpr_coordinate_info"):
            self.bpr_coordinate_info.setStyleSheet(
                f'font-family:"Consolas"; font-size:{size}px;'
            )

    def _form_page(self):
        w = QWidget(); layout = QVBoxLayout(w); form = QFormLayout(); layout.addLayout(form); layout.addStretch(); return w, form

    def _create_pages(self):
        # Viewer
        p, f = self._form_page()
        self.viewer_file = PathRow(
            "file",
            "RAW Files (*.raw *.RAW *.bin *.BIN)",
            "Select RAW File to View",
        )
        self.viewer_file.path_selected.connect(self.auto_preview_raw)
        self.viewer_file.edit.returnPressed.connect(self.auto_preview_raw)
        f.addRow("RAW File", self.viewer_file)

        self.viewer_stats_group = QGroupBox("RAW Statistics (DN)")
        self.viewer_stats_group.setObjectName("viewerStatsCard")
        viewer_stats_layout = QGridLayout(self.viewer_stats_group)
        viewer_stats_layout.setHorizontalSpacing(10)
        viewer_stats_layout.setVerticalSpacing(8)
        self.viewer_stat_values = {}
        stat_fields = [
            ("Pixel Count", "count"),
            ("Minimum", "min"),
            ("Maximum", "max"),
            ("Mean", "mean"),
            ("Median", "median"),
            ("Std Dev", "std"),
            ("P1", "p1"),
            ("P99", "p99"),
        ]
        for index, (label_text, key) in enumerate(stat_fields):
            row = index // 2
            column = (index % 2) * 2
            name_label = QLabel(label_text)
            value_label = QLabel("--")
            value_label.setProperty("viewerStatValue", True)
            value_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            value_label.setMinimumWidth(92)
            viewer_stats_layout.addWidget(name_label, row, column)
            viewer_stats_layout.addWidget(value_label, row, column + 1)
            self.viewer_stat_values[key] = value_label

        self.viewer_stats_note = QLabel(
            "Statistics use every input pixel. Std Dev is population standard deviation "
            "(ddof=0); P1/P99 are percentiles of the original RAW DN values."
        )
        self.viewer_stats_note.setWordWrap(True)
        self.viewer_stats_note.setStyleSheet("color:#aeb8c2; font-weight:400; padding-top:4px;")
        viewer_stats_layout.addWidget(self.viewer_stats_note, 4, 0, 1, 4)
        f.addRow(self.viewer_stats_group)
        self.stack.addWidget(p)

        # Average
        p, f = self._form_page(); self.avg_folder = PathRow("folder", dialog_title="Select RAW Folder to Average"); self.avg_suffix_combo = QComboBox(); self.avg_suffix_combo.addItems(["LOW", "HIGH"]); f.addRow("RAW Folder", self.avg_folder); f.addRow("Filename Suffix", self.avg_suffix_combo); self.avg_save_png = QCheckBox("Save preview PNG"); self.avg_save_png.setChecked(True); f.addRow(self.avg_save_png); self.stack.addWidget(p)

        # NUC Calculate
        p, f = self._form_page()
        self.nuc_mode = QComboBox(); self.nuc_mode.addItems(["Folder Average (Recommended)", "Single RAW"]); self.nuc_mode.currentIndexChanged.connect(self.update_nuc_mode)
        self.nuc_point_combo = QComboBox()
        for point_number in range(1, 6):
            self.nuc_point_combo.addItem(f"Point {point_number}", point_number)
        self.low_single = PathRow("file", "RAW Files (*.raw *.RAW *.bin *.BIN)", "Select LOW RAW File for NUC")
        self.high_single = PathRow("file", "RAW Files (*.raw *.RAW *.bin *.BIN)", "Select HIGH RAW File for NUC")
        self.low_folder = PathRow("folder", dialog_title="Select LOW RAW Folder for NUC")
        self.high_folder = PathRow("folder", dialog_title="Select HIGH RAW Folder for NUC")
        self.min_delta = QSpinBox(); self.min_delta.setRange(0, 16383); self.min_delta.setValue(50)
        f.addRow("NUC Point", self.nuc_point_combo); f.addRow("NUC Source Mode", self.nuc_mode); f.addRow("LOW RAW", self.low_single); f.addRow("HIGH RAW", self.high_single); f.addRow("LOW Folder", self.low_folder); f.addRow("HIGH Folder", self.high_folder); f.addRow("Min Delta DN", self.min_delta)
        note = QLabel("Recommended: capture multiple frames at each stable blackbody temperature, average each folder pixel-by-pixel, then calculate NUC Gain/Offset and Novatek K/B tables.")
        note.setWordWrap(True); note.setStyleSheet("color:#c98;"); f.addRow(note)
        self.stack.addWidget(p); self.update_nuc_mode()

        # NUC Apply
        p, f = self._form_page(); self.apply_raw_file = PathRow("file", "RAW Files (*.raw *.RAW *.bin *.BIN)", "Select RAW File for NUC Apply"); self.k_bin_file = PathRow("file", "Novatek K Table (*.bin *.BIN)", "Select NUC pointN_K.bin"); self.b_bin_file = PathRow("file", "Novatek B Table (*.bin *.BIN)", "Select NUC pointN_B.bin"); self.k_bin_file.path_selected.connect(self.auto_pair_nuc_b_file); f.addRow("RAW File", self.apply_raw_file); f.addRow("pointN_K.bin", self.k_bin_file); f.addRow("pointN_B.bin", self.b_bin_file); self.stack.addWidget(p)

        # High Low Analyzer
        p, f = self._form_page(); self.an_low = PathRow("file", "RAW Files (*.raw *.RAW *.bin *.BIN)", "Select LOW RAW File for High / Low Analysis"); self.an_high = PathRow("file", "RAW Files (*.raw *.RAW *.bin *.BIN)", "Select HIGH RAW File for High / Low Analysis"); self.an_min_delta = QSpinBox(); self.an_min_delta.setRange(0, 16383); self.an_min_delta.setValue(50); f.addRow("LOW RAW", self.an_low); f.addRow("HIGH RAW", self.an_high); f.addRow("Min Delta DN", self.an_min_delta); self.stack.addWidget(p)

        # Temporal Drift
        p, f = self._form_page(); self.drift_folder = PathRow("folder", dialog_title="Select RAW Folder for Temporal Drift Analysis"); self.std_spec = QDoubleSpinBox(); self.std_spec.setRange(0, 100000); self.std_spec.setValue(50.0); self.p2p_spec = QDoubleSpinBox(); self.p2p_spec.setRange(0, 100000); self.p2p_spec.setValue(100.0); f.addRow("RAW Folder", self.drift_folder); f.addRow("Temporal Std Spec", self.std_spec); f.addRow("Peak-to-Peak Spec", self.p2p_spec)
        self.drift_help = QLabel(
            "Temporal Std：每個像素隨時間的整體波動程度。\n"
            "Peak-to-Peak：每個像素的最大 DN - 最小 DN；超過 Spec 會判定為異常。\n"
            "建議使用固定溫度、固定場景且沒有 FFC／Shutter 動作的 RAW。"
        )
        self.drift_help.setWordWrap(True)
        self.drift_help.setStyleSheet("color:#c8d0d9; background:#292d33; border-left:3px solid #79afd3; border-radius:5px; padding:8px;")
        f.addRow(self.drift_help)
        self.stack.addWidget(p)

        # BPR Bad Pixel Marking
        p, f = self._form_page()
        self.bpr_form = f
        self.bpr_coordinate_file = PathRow(
            "file",
            "BPR Table / Coordinates (*.bin *.BIN *.csv *.CSV *.txt *.TXT)",
            "Select Novatek dpc_table.bin or x,y coordinate file",
        )
        self.bpr_use_raw = QCheckBox("Overlay circles on a RAW image")
        self.bpr_use_raw.setChecked(False)
        self.bpr_raw_file = PathRow(
            "file",
            "RAW Files (*.raw *.RAW *.bin *.BIN)",
            "Select optional RAW background",
        )
        self.bpr_circle_radius = QSpinBox()
        self.bpr_circle_radius.setRange(1, 20)
        self.bpr_circle_radius.setValue(BPR_MARK_RADIUS)
        self.bpr_note = QLabel()
        self.bpr_note.setWordWrap(True)
        self.bpr_note.setStyleSheet(
            "color:#c8d0d9; background:#292d33; border-left:3px solid #79afd3; "
            "border-radius:5px; padding:8px;"
        )
        self.bpr_coordinate_info = QTextEdit()
        self.bpr_coordinate_info.setReadOnly(True)
        self.bpr_coordinate_info.setMinimumHeight(190)
        self.bpr_coordinate_info.setPlaceholderText(
            "Run / Preview to show bad-pixel counts and all coordinates."
        )
        self.update_log_font_size(self.log_font_size.value())
        self.bpr_use_raw.toggled.connect(self.update_bpr_marking_ui)
        self.bpr_coordinate_file.edit.textChanged.connect(self.clear_bpr_coordinate_info)
        self.resolution_combo.currentIndexChanged.connect(self.clear_bpr_coordinate_info)
        self.resolution_combo.currentIndexChanged.connect(self.update_bpr_coordinate_note)
        f.addRow("BPR Table / Coordinates", self.bpr_coordinate_file)
        f.addRow("Background", self.bpr_use_raw)
        f.addRow("RAW File", self.bpr_raw_file)
        f.addRow("Circle Radius", self.bpr_circle_radius)
        f.addRow(self.bpr_note)
        f.addRow("Bad Pixel Information", self.bpr_coordinate_info)
        self.stack.addWidget(p)
        self.update_bpr_marking_ui()
        self.update_bpr_coordinate_note()

        # Single RAW Algorithm Compare
        p, f = self._form_page()
        self.raw_algorithm_form = f
        self.raw_algorithm_file = PathRow(
            "file",
            "RAW Files (*.raw *.RAW *.bin *.BIN)",
            "Select RAW File for Algorithm Compare",
        )
        self.raw_algorithm_combo = QComboBox()
        for algorithm_name, algorithm_key in IMAGE_ALGORITHM_OPTIONS:
            self.raw_algorithm_combo.addItem(algorithm_name, algorithm_key)
        self.raw_algorithm_description = QLabel()
        self.raw_algorithm_description.setWordWrap(True)
        self.raw_algorithm_description.setStyleSheet(
            "color:#c8d0d9; background:#292d33; border-left:3px solid #79afd3; "
            "border-radius:5px; padding:8px;"
        )
        self.raw_low_percentile = QDoubleSpinBox(); self.raw_low_percentile.setRange(0.0, 99.5); self.raw_low_percentile.setDecimals(1); self.raw_low_percentile.setSingleStep(0.5); self.raw_low_percentile.setValue(1.0)
        self.raw_high_percentile = QDoubleSpinBox(); self.raw_high_percentile.setRange(0.5, 100.0); self.raw_high_percentile.setDecimals(1); self.raw_high_percentile.setSingleStep(0.5); self.raw_high_percentile.setValue(99.0)
        self.raw_clahe_clip = QDoubleSpinBox(); self.raw_clahe_clip.setRange(0.1, 100.0); self.raw_clahe_clip.setDecimals(1); self.raw_clahe_clip.setSingleStep(0.1); self.raw_clahe_clip.setValue(2.0)
        self.raw_clahe_grid = QSpinBox(); self.raw_clahe_grid.setRange(1, 64); self.raw_clahe_grid.setValue(8)
        self.raw_gamma = QDoubleSpinBox(); self.raw_gamma.setRange(0.1, 3.0); self.raw_gamma.setDecimals(1); self.raw_gamma.setSingleStep(0.1); self.raw_gamma.setValue(0.7)
        self.raw_bilateral_strength = QSpinBox(); self.raw_bilateral_strength.setRange(1, 7); self.raw_bilateral_strength.setValue(2)
        self.raw_unsharp_amount = QDoubleSpinBox(); self.raw_unsharp_amount.setRange(0.1, 3.0); self.raw_unsharp_amount.setDecimals(1); self.raw_unsharp_amount.setSingleStep(0.1); self.raw_unsharp_amount.setValue(1.0)
        self.raw_algorithm_reset_btn = QPushButton("Reset Algorithm Defaults")
        self.raw_algorithm_reset_btn.setObjectName("resetButton")
        self.raw_algorithm_reset_btn.clicked.connect(self.reset_raw_algorithm_settings)
        self.raw_low_percentile.valueChanged.connect(self.update_raw_percentile_limits)
        self.raw_high_percentile.valueChanged.connect(self.update_raw_percentile_limits)
        f.addRow("RAW File", self.raw_algorithm_file)
        f.addRow("Right-side Algorithm", self.raw_algorithm_combo)
        f.addRow(self.raw_algorithm_description)
        f.addRow("Low Percentile", self.raw_low_percentile)
        f.addRow("High Percentile", self.raw_high_percentile)
        f.addRow("CLAHE Clip Limit", self.raw_clahe_clip)
        f.addRow("CLAHE Grid Size", self.raw_clahe_grid)
        f.addRow("Gamma", self.raw_gamma)
        f.addRow("Bilateral Strength", self.raw_bilateral_strength)
        f.addRow("Unsharp Amount", self.raw_unsharp_amount)
        f.addRow(self.raw_algorithm_reset_btn)
        self.stack.addWidget(p)
        self.raw_algorithm_combo.currentIndexChanged.connect(self.update_raw_algorithm_ui)
        self.update_raw_percentile_limits()
        self.update_raw_algorithm_ui()

        # UVC Algorithm Compare
        p, f = self._form_page()
        self.uvc_camera_combo = QComboBox()
        for camera_index in CAMERA_INDEX_OPTIONS:
            self.uvc_camera_combo.addItem(f"Camera {camera_index}", camera_index)
        default_camera_index = CAMERA_INDEX_OPTIONS.index(DEFAULT_CAMERA_INDEX) if DEFAULT_CAMERA_INDEX in CAMERA_INDEX_OPTIONS else 0
        self.uvc_camera_combo.setCurrentIndex(default_camera_index)
        self.uvc_form = f
        self.uvc_algorithm_combo = QComboBox()
        for algorithm_name, algorithm_key in IMAGE_ALGORITHM_OPTIONS:
            self.uvc_algorithm_combo.addItem(algorithm_name, algorithm_key)
        self.uvc_algorithm_description = QLabel()
        self.uvc_algorithm_description.setWordWrap(True)
        self.uvc_algorithm_description.setStyleSheet(
            "color:#c8d0d9; background:#292d33; border-left:3px solid #79afd3; "
            "border-radius:5px; padding:8px;"
        )
        self.uvc_apply_nuc = QCheckBox("Enable NUC before algorithm")
        self.uvc_nuc_k_file = PathRow("file", "Novatek K Table (*.bin *.BIN)", "Select UVC NUC pointN_K.bin")
        self.uvc_nuc_b_file = PathRow("file", "Novatek B Table (*.bin *.BIN)", "Select UVC NUC pointN_B.bin")
        self.uvc_nuc_note = QLabel("K/B table must match the selected Resolution and DN scale. An 8-bit UVC frame is linearly mapped to the selected Bit Depth before NUC.")
        self.uvc_nuc_note.setWordWrap(True)
        self.uvc_nuc_note.setStyleSheet("color:#c98;")
        self.uvc_fps_combo = QComboBox()
        for fps in (30, 60):
            self.uvc_fps_combo.addItem(f"{fps} FPS", fps)
        default_fps_index = self.uvc_fps_combo.findData(DEFAULT_UVC_FPS)
        self.uvc_fps_combo.setCurrentIndex(max(default_fps_index, 0))
        self.uvc_low_percentile = QDoubleSpinBox(); self.uvc_low_percentile.setRange(0.0, 99.5); self.uvc_low_percentile.setDecimals(1); self.uvc_low_percentile.setSingleStep(0.5); self.uvc_low_percentile.setValue(1.0)
        self.uvc_high_percentile = QDoubleSpinBox(); self.uvc_high_percentile.setRange(0.5, 100.0); self.uvc_high_percentile.setDecimals(1); self.uvc_high_percentile.setSingleStep(0.5); self.uvc_high_percentile.setValue(99.0)
        self.uvc_clahe_clip = QDoubleSpinBox(); self.uvc_clahe_clip.setRange(0.1, 100.0); self.uvc_clahe_clip.setDecimals(1); self.uvc_clahe_clip.setSingleStep(0.1); self.uvc_clahe_clip.setValue(2.0)
        self.uvc_clahe_grid = QSpinBox(); self.uvc_clahe_grid.setRange(1, 64); self.uvc_clahe_grid.setValue(8)
        self.uvc_gamma = QDoubleSpinBox(); self.uvc_gamma.setRange(0.1, 3.0); self.uvc_gamma.setDecimals(1); self.uvc_gamma.setSingleStep(0.1); self.uvc_gamma.setValue(0.7)
        self.uvc_bilateral_strength = QSpinBox(); self.uvc_bilateral_strength.setRange(1, 7); self.uvc_bilateral_strength.setValue(2)
        self.uvc_unsharp_amount = QDoubleSpinBox(); self.uvc_unsharp_amount.setRange(0.1, 3.0); self.uvc_unsharp_amount.setDecimals(1); self.uvc_unsharp_amount.setSingleStep(0.1); self.uvc_unsharp_amount.setValue(1.0)
        self.uvc_low_percentile.valueChanged.connect(self.update_uvc_percentile_limits)
        self.uvc_high_percentile.valueChanged.connect(self.update_uvc_percentile_limits)
        self.update_uvc_percentile_limits()
        self.uvc_camera_combo.currentIndexChanged.connect(self.switch_uvc_camera)
        self.uvc_apply_nuc.toggled.connect(self.update_uvc_nuc_ui)
        self.uvc_nuc_k_file.path_selected.connect(self.auto_pair_uvc_nuc_b_file)
        self.uvc_nuc_k_file.edit.textChanged.connect(self.invalidate_uvc_nuc_cache)
        self.uvc_nuc_b_file.edit.textChanged.connect(self.invalidate_uvc_nuc_cache)
        self.resolution_combo.currentIndexChanged.connect(self.invalidate_uvc_nuc_cache)
        controls = QWidget(); controls_layout = QHBoxLayout(controls); controls_layout.setContentsMargins(0, 0, 0, 0)
        self.uvc_start_btn = QPushButton("Start Camera"); self.uvc_start_btn.setObjectName("startButton"); self.uvc_start_btn.clicked.connect(self.start_uvc_camera)
        self.uvc_stop_btn = QPushButton("Stop Camera"); self.uvc_stop_btn.setObjectName("stopButton"); self.uvc_stop_btn.clicked.connect(self.stop_uvc_camera); self.uvc_stop_btn.setEnabled(False)
        self.uvc_capture_btn = QPushButton("Capture Pair"); self.uvc_capture_btn.setObjectName("captureButton"); self.uvc_capture_btn.clicked.connect(self.capture_uvc_pair); self.uvc_capture_btn.setEnabled(False)
        self.uvc_reset_btn = QPushButton("Reset Algorithm Defaults"); self.uvc_reset_btn.setObjectName("resetButton"); self.uvc_reset_btn.clicked.connect(self.reset_uvc_algorithm_settings)
        controls_layout.addWidget(self.uvc_start_btn); controls_layout.addWidget(self.uvc_stop_btn); controls_layout.addWidget(self.uvc_capture_btn); controls_layout.addWidget(self.uvc_reset_btn)
        self.uvc_status = QLabel("Camera stopped"); self.uvc_status.setWordWrap(True)
        f.addRow("CAMERA_INDEX", self.uvc_camera_combo)
        f.addRow("NUC Preprocess", self.uvc_apply_nuc)
        f.addRow("NUC pointN_K.bin", self.uvc_nuc_k_file)
        f.addRow("NUC pointN_B.bin", self.uvc_nuc_b_file)
        f.addRow(self.uvc_nuc_note)
        f.addRow("Right-side Algorithm", self.uvc_algorithm_combo)
        f.addRow(self.uvc_algorithm_description)
        f.addRow("Requested FPS", self.uvc_fps_combo)
        f.addRow("Low Percentile", self.uvc_low_percentile)
        f.addRow("High Percentile", self.uvc_high_percentile)
        f.addRow("CLAHE Clip Limit", self.uvc_clahe_clip)
        f.addRow("CLAHE Grid Size", self.uvc_clahe_grid)
        f.addRow("Gamma", self.uvc_gamma)
        f.addRow("Bilateral Strength", self.uvc_bilateral_strength)
        f.addRow("Unsharp Amount", self.uvc_unsharp_amount)
        f.addRow(controls)
        f.addRow("Status", self.uvc_status)
        self.stack.addWidget(p)
        self.uvc_algorithm_combo.currentIndexChanged.connect(self.update_uvc_algorithm_ui)
        self.update_uvc_nuc_ui()
        self.update_uvc_algorithm_ui()

    def update_nuc_mode(self):
        folder_mode = self.nuc_mode.currentIndex() == 0
        for w in (self.low_folder, self.high_folder): w.setEnabled(folder_mode)
        for w in (self.low_single, self.high_single): w.setEnabled(not folder_mode)

    def update_bpr_marking_ui(self, _checked=None):
        self.set_form_parameter_visible(
            self.bpr_form,
            self.bpr_raw_file,
            self.bpr_use_raw.isChecked(),
        )

    def update_bpr_coordinate_note(self, _index=None):
        self.bpr_note.setText(
            "座標方向：左上角為原點 (0,0)；X 由左往右增加，"
            f"有效範圍 0..{self.width - 1}；Y 由上往下增加，"
            f"有效範圍 0..{self.height - 1}。座標以 (x,y) 表示，"
            "對應程式陣列 RAW[y,x]。例如 (600,79) 是從左側第 601 欄、"
            "從上方第 80 列。Novatek BIN 使用 SIE row 格式；CSV/TXT 使用 x,y；"
            "重複座標只圈選一次。"
        )

    def clear_bpr_coordinate_info(self, _value=None):
        self.bpr_coordinate_info.clear()

    def set_form_parameter_visible(self, form, widget, visible):
        widget.setVisible(visible)
        label = form.labelForField(widget)
        if label is not None:
            label.setVisible(visible)

    def update_raw_percentile_limits(self, _value=None):
        self.raw_high_percentile.setMinimum(
            min(100.0, self.raw_low_percentile.value() + 0.5)
        )
        self.raw_low_percentile.setMaximum(
            max(0.0, self.raw_high_percentile.value() - 0.5)
        )

    def reset_raw_algorithm_settings(self):
        algorithm_key = self.raw_algorithm_combo.currentData()
        reset_items = []
        if algorithm_key in ("auto_contrast", "auto_contrast_clahe"):
            self.raw_high_percentile.setValue(99.0)
            self.raw_low_percentile.setValue(1.0)
            reset_items.append("Percentiles P1.0/P99.0")
        if algorithm_key in ("clahe", "auto_contrast_clahe"):
            self.raw_clahe_clip.setValue(2.0)
            self.raw_clahe_grid.setValue(8)
            reset_items.append("CLAHE clip=2.0, grid=8x8")
        if algorithm_key == "gamma":
            self.raw_gamma.setValue(0.7)
            reset_items.append("Gamma=0.7")
        elif algorithm_key == "bilateral":
            self.raw_bilateral_strength.setValue(2)
            reset_items.append("Bilateral strength=2")
        elif algorithm_key == "unsharp":
            self.raw_unsharp_amount.setValue(1.0)
            reset_items.append("Unsharp amount=1.0")
        if reset_items:
            self.log.append(
                f"Reset {self.raw_algorithm_combo.currentText()}: {', '.join(reset_items)}"
            )

    def update_raw_algorithm_ui(self, _combo_index=None):
        algorithm_key = self.raw_algorithm_combo.currentData()
        self.raw_algorithm_description.setText(
            IMAGE_ALGORITHM_DESCRIPTIONS.get(algorithm_key, "")
        )
        uses_auto_contrast = algorithm_key in ("auto_contrast", "auto_contrast_clahe")
        uses_clahe = algorithm_key in ("clahe", "auto_contrast_clahe")
        self.set_form_parameter_visible(
            self.raw_algorithm_form, self.raw_low_percentile, uses_auto_contrast
        )
        self.set_form_parameter_visible(
            self.raw_algorithm_form, self.raw_high_percentile, uses_auto_contrast
        )
        self.raw_algorithm_reset_btn.setEnabled(
            algorithm_key not in (
                "fieldscale",
                "hist_equalization",
                "raw_percentile_preview",
            )
        )
        self.set_form_parameter_visible(
            self.raw_algorithm_form, self.raw_clahe_clip, uses_clahe
        )
        self.set_form_parameter_visible(
            self.raw_algorithm_form, self.raw_clahe_grid, uses_clahe
        )
        self.set_form_parameter_visible(
            self.raw_algorithm_form, self.raw_gamma, algorithm_key == "gamma"
        )
        self.set_form_parameter_visible(
            self.raw_algorithm_form,
            self.raw_bilateral_strength,
            algorithm_key == "bilateral",
        )
        self.set_form_parameter_visible(
            self.raw_algorithm_form, self.raw_unsharp_amount, algorithm_key == "unsharp"
        )
        if self.function_combo.currentIndex() == len(self.FUNCTIONS) - 2:
            title = self.raw_algorithm_combo.currentText()
            self.preview_result.set_title(title)
            self.preview_result_caption.setText(title)

    def change_page(self, index):
        uvc_page_index = len(self.FUNCTIONS) - 1
        raw_algorithm_page_index = len(self.FUNCTIONS) - 2
        if index != uvc_page_index and self.uvc_worker is not None:
            self.uvc_restart_when_stopped = False
            self.stop_uvc_camera(log_message=False)
        self.stack.setCurrentIndex(index)
        self.function_title.setText(self.FUNCTIONS[index])
        self.function_description.setText(self.FUNCTION_DESCRIPTIONS[index])
        show_comparison, original_title, result_title = self.PREVIEW_LAYOUTS[index]
        if index == raw_algorithm_page_index:
            result_title = self.raw_algorithm_combo.currentText()
        elif index == uvc_page_index:
            result_title = self.uvc_result_title()
        self.preview_original_container.setVisible(show_comparison)
        self.preview_original.setVisible(show_comparison)
        self.preview_original.set_title(original_title)
        self.preview_original_caption.setText(original_title)
        self.preview_result.set_title(result_title)
        self.preview_result_caption.setText(result_title)
        self.preview_original.set_array(None)
        self.preview_result.set_array(None)
        self.run_btn.setVisible(index != uvc_page_index)
        self.save_current_btn.setVisible(index != 3)
        self.save_current_btn.setText(
            "Save Original + Processed PNG"
            if index == uvc_page_index
            else "Save Current Preview PNG"
        )
        self.log.clear()
        self.log.append(f"Selected: {self.FUNCTIONS[index]}")

    def auto_preview_raw(self, _path=None):
        if self.function_combo.currentIndex() == 0:
            self.run_current()

    def auto_pair_nuc_b_file(self, k_path):
        path = Path(k_path)
        if path.name.lower().endswith("_k.bin"):
            candidate = path.with_name(f"{path.name[:-6]}_B.bin")
            if candidate.is_file():
                self.b_bin_file.setText(str(candidate))

    @property
    def width(self): return self.resolution_combo.currentData()[0]
    @property
    def height(self): return self.resolution_combo.currentData()[1]
    @property
    def bit_depth(self): return self.bit_depth_combo.currentData()
    @property
    def raw_max(self): return (1 << self.bit_depth) - 1

    def log_stats(self, name, image, stats=None):
        s = stats if stats is not None else image_stats(image)
        self.log.append(f"\n[{name}]\nMin={s['min']:.3f}  Max={s['max']:.3f}  Mean={s['mean']:.3f}  Median={s['median']:.3f}  Std={s['std']:.3f}")

    def clear_viewer_stats(self):
        for value_label in self.viewer_stat_values.values():
            value_label.setText("--")

    def update_viewer_stats(self, image):
        stats = image_stats(image)
        self.viewer_stat_values["count"].setText(f"{stats['count']:,}")
        for key in ("min", "max", "mean", "median", "std", "p1", "p99"):
            self.viewer_stat_values[key].setText(f"{stats[key]:,.3f}")
        return stats

    def make_output(self, name):
        out = unique_result_dir(self.output_root.text(), name)
        self.last_output_dir = str(out)
        self.log.append(f"\nOutput: {out}")
        return out

    def run_current(self):
        QApplication.setOverrideCursor(Qt.WaitCursor)
        try:
            idx = self.function_combo.currentIndex()
            handlers = [
                self.run_viewer,
                self.run_average,
                self.run_nuc_calculate,
                self.run_nuc_apply,
                self.run_high_low,
                self.run_drift,
                self.run_bpr_marking,
                self.run_raw_algorithm,
                self.start_uvc_camera,
            ]
            handlers[idx]()
        except Exception as exc:
            self.log.append(f"\nERROR: {exc}")
            QMessageBox.critical(self, "Error", str(exc))
        finally:
            QApplication.restoreOverrideCursor()

    def run_viewer(self):
        self.current_original = None
        self.current_result = None
        self.preview_result.set_array(None)
        self.clear_viewer_stats()
        raw_path = self.viewer_file.text()
        raw = load_raw(raw_path, self.width, self.height)
        self.current_original = raw; self.current_result = raw
        self.preview_result.set_array(raw_to_display_u8(raw, self.raw_max))
        stats = self.update_viewer_stats(raw)
        self.log.append(f"\nRAW File: {raw_path}")
        self.log.append(
            f"Resolution: {self.width} x {self.height}; Pixels: {raw.size:,}; "
            f"Selected bit depth: {self.bit_depth}-bit"
        )
        self.log_stats("RAW", raw, stats)
        self.log.append(f"P1={stats['p1']:.3f}  P99={stats['p99']:.3f}")
        self.log.append(f"Preview scale: fixed 0..{self.raw_max} DN")

    def run_average(self):
        avg, valid, skipped = average_raw_folder(self.avg_folder.text(), self.width, self.height)
        out = self.make_output("average_raw")
        suffix = self.avg_suffix_combo.currentText()
        avg_preview = raw_to_display_u8(avg, self.raw_max)
        raw_path = out / f"average_{suffix}.raw"; avg.astype(RAW_DTYPE).tofile(raw_path)
        if self.avg_save_png.isChecked(): save_png(out / f"average_preview_{suffix}.png", avg_preview)
        with open(out / "source_files.txt", "w", encoding="utf-8") as f:
            f.write("\n".join(valid))
        self.current_original = load_raw(valid[0], self.width, self.height); self.current_result = avg
        self.preview_result.set_array(avg_preview)
        self.log.append(f"Valid frames: {len(valid)}; Skipped: {len(skipped)}")
        self.log.append(f"Filename suffix: {suffix}")
        self.log_stats("First Frame", self.current_original); self.log_stats("Average RAW", avg)
        self.log.append(f"Preview scale: fixed 0..{self.raw_max} DN")
        self.log.append(f"Saved: {raw_path}")

    def _get_nuc_inputs(self):
        if self.nuc_mode.currentIndex() == 0:
            low, low_files, low_skip = average_raw_folder(self.low_folder.text(), self.width, self.height)
            high, high_files, high_skip = average_raw_folder(self.high_folder.text(), self.width, self.height)
            return low, high, low_files, high_files, low_skip, high_skip
        low = load_raw(self.low_single.text(), self.width, self.height)
        high = load_raw(self.high_single.text(), self.width, self.height)
        return low, high, [self.low_single.text()], [self.high_single.text()], [], []

    def run_nuc_calculate(self):
        low, high, low_files, high_files, low_skip, high_skip = self._get_nuc_inputs()
        gain, offset, valid, delta, low_avg, high_avg = calculate_nuc(low, high, self.min_delta.value())
        stored_k, gfx_b, table_valid, novatek_low_avg, novatek_high_avg = build_novatek_nuc_tables(
            low, high, self.min_delta.value()
        )
        valid = table_valid
        point_number = self.nuc_point_combo.currentData()
        out = self.make_output("nuc_calculate")
        low.astype(RAW_DTYPE).tofile(out / "low_input_used.raw"); high.astype(RAW_DTYPE).tofile(out / "high_input_used.raw")
        np.save(out / "gain.npy", gain); np.save(out / "offset.npy", offset)
        save_csv_matrix(out / "gain.csv", gain, "%.8f"); save_csv_matrix(out / "offset.csv", offset, "%.3f")
        k_path = out / f"point{point_number}_K.bin"
        b_path = out / f"point{point_number}_B.bin"
        stored_k.astype("<u2").tofile(k_path)
        gfx_b.astype("<i2").tofile(b_path)
        yy, xx = np.where(~valid)
        with open(out / "invalid_pixels.csv", "w", newline="") as f:
            wr = csv.writer(f); wr.writerow(["x", "y", "low", "high", "delta"])
            for y, x in zip(yy, xx): wr.writerow([x, y, int(low[y,x]), int(high[y,x]), float(delta[y,x])])
        with open(out / "nuc_summary.txt", "w", encoding="utf-8") as f:
            f.write(f"Novatek NUC point: {point_number}\nMode: {self.nuc_mode.currentText()}\nLow frames: {len(low_files)}\nHigh frames: {len(high_files)}\n")
            f.write(f"Low mean: {low_avg:.6f}\nHigh mean: {high_avg:.6f}\nTarget delta: {high_avg-low_avg:.6f}\n")
            f.write(f"Min delta: {self.min_delta.value()}\nValid pixels: {valid.sum()} / {valid.size}\n")
            if valid.any(): f.write(f"Gain mean/std: {gain[valid].mean():.8f} / {gain[valid].std():.8f}\nOffset mean/std: {offset[valid].mean():.4f} / {offset[valid].std():.4f}\n")
            f.write(f"Novatek integer LOW/HIGH mean: {novatek_low_avg} / {novatek_high_avg}\n")
            f.write(f"K format: uint16 Q8; B format: signed int16\n")
            f.write(f"Runtime formula: output = (K * (input - B)) >> {NOVATEK_NUC_GAIN_SHIFT}\n")
            f.write(f"K file: {k_path.name} ({k_path.stat().st_size} bytes)\n")
            f.write(f"B file: {b_path.name} ({b_path.stat().st_size} bytes)\n")
        low_preview = raw_to_display_u8(low, self.raw_max)
        high_preview = raw_to_display_u8(high, self.raw_max)
        save_png(out / "low_preview.png", low_preview); save_png(out / "high_preview.png", high_preview)
        # Preview the same quantized K/B formula used by the Novatek GFX path.
        corrected_high = apply_novatek_nuc(high, stored_k, gfx_b, self.raw_max)
        corrected_high_preview = raw_to_display_u8(corrected_high, self.raw_max)
        corrected_high.astype(RAW_DTYPE).tofile(out / "high_nuc_corrected.raw")
        save_png(out / "high_nuc_corrected_preview.png", corrected_high_preview)
        self.current_original = high; self.current_result = corrected_high
        self.preview_original.set_array(high_preview)
        self.preview_result.set_array(corrected_high_preview)
        self.log.append(f"Novatek NUC point={point_number}: {k_path.name}, {b_path.name}")
        self.log.append(f"Mode: {self.nuc_mode.currentText()}")
        self.log.append(f"LOW frames={len(low_files)}, HIGH frames={len(high_files)}, invalid/skipped={len(low_skip)+len(high_skip)}")
        self.log.append(f"Low mean={low_avg:.3f}, High mean={high_avg:.3f}, Target delta={high_avg-low_avg:.3f}")
        self.log.append(f"Valid NUC pixels={valid.sum():,}/{valid.size:,} ({valid.mean()*100:.3f}%)")
        if valid.any(): self.log.append(f"Gain mean={gain[valid].mean():.6f}, std={gain[valid].std():.6f}; Offset mean={offset[valid].mean():.3f}, std={offset[valid].std():.3f}")
        if valid.any():
            corrected_valid = corrected_high[valid].astype(np.float64)
            self.log.append(
                f"Corrected HIGH valid pixels: mean={corrected_valid.mean():.3f}, "
                f"std={corrected_valid.std():.3f}, min={corrected_valid.min():.0f}, "
                f"max={corrected_valid.max():.0f} DN"
            )
        if not valid.all():
            self.log.append(f"Invalid pixels kept uncorrected: {(~valid).sum():,}")
        self.log.append("Preview scale: fixed 0..Bit Depth Max (no percentile stretching)")

    def run_nuc_apply(self):
        raw = load_raw(self.apply_raw_file.text(), self.width, self.height)
        stored_k = load_novatek_nuc_bin(self.k_bin_file.text(), self.width, self.height, "<u2", "K")
        gfx_b = load_novatek_nuc_bin(self.b_bin_file.text(), self.width, self.height, "<i2", "B")
        corrected = apply_novatek_nuc(raw, stored_k, gfx_b, self.raw_max)
        out = self.make_output("nuc_apply")
        original_path = out / "original.raw"
        original_png_path = out / "original.png"
        corrected_path = out / "corrected_nuc.raw"
        corrected_png_path = out / "corrected_nuc.png"
        raw.astype(RAW_DTYPE).tofile(original_path)
        save_png(original_png_path, raw_to_display_u8(raw, self.raw_max))
        corrected.astype(RAW_DTYPE).tofile(corrected_path)
        save_png(corrected_png_path, raw_to_display_u8(corrected, self.raw_max))
        self.current_original = raw; self.current_result = corrected
        self.preview_original.set_array(raw_to_display_u8(raw, self.raw_max))
        self.preview_result.set_array(raw_to_display_u8(corrected, self.raw_max))
        self.log.append(f"K BIN: {self.k_bin_file.text()}")
        self.log.append(f"B BIN: {self.b_bin_file.text()}")
        self.log.append(f"Saved original RAW: {original_path}")
        self.log.append(f"Saved original PNG: {original_png_path}")
        self.log.append(f"Saved NUC RAW: {corrected_path}")
        self.log.append(f"Saved NUC PNG: {corrected_png_path}")

    def run_high_low(self):
        low = load_raw(self.an_low.text(), self.width, self.height); high = load_raw(self.an_high.text(), self.width, self.height)
        diff = high.astype(np.int32) - low.astype(np.int32); threshold = self.an_min_delta.value()
        valid = diff > threshold
        out = self.make_output("high_low_analysis")
        np.save(out / "delta.npy", diff)
        yy, xx = np.where(~valid)
        with open(out / "abnormal_pixels.csv", "w", newline="") as f:
            wr = csv.writer(f); wr.writerow(["x","y","low","high","delta","high_le_low","small_delta","saturated"])
            for y,x in zip(yy,xx):
                d=int(diff[y,x]); wr.writerow([x,y,int(low[y,x]),int(high[y,x]),d,int(d<=0),int(0<d<=threshold),int(low[y,x]>=self.raw_max or high[y,x]>=self.raw_max)])
        # Signed delta uses the fixed Bit Depth range; zero maps to middle gray.
        delta8 = signed_delta_to_display_u8(diff, self.raw_max)
        save_png(out / "difference_preview.png", delta8)
        self.current_original = low; self.current_result = diff
        self.preview_original.set_array(raw_to_display_u8(low, self.raw_max)); self.preview_result.set_array(delta8)
        self.log_stats("LOW", low); self.log_stats("HIGH", high)
        self.log.append(f"High > Low: {(diff>0).sum():,} ({(diff>0).mean()*100:.3f}%)")
        self.log.append(f"High = Low: {(diff==0).sum():,}; High < Low: {(diff<0).sum():,}")
        self.log.append(f"Delta Mean={diff.mean():.3f}, Std={diff.std():.3f}, Min={diff.min()}, Max={diff.max()}")
        self.log.append(f"Delta > {threshold} valid: {valid.sum():,}/{valid.size:,}")
        self.log.append(f"Difference preview scale: fixed {-self.raw_max}..{self.raw_max} DN")

    def run_drift(self):
        files = find_raw_files(self.drift_folder.text())
        if not files: raise FileNotFoundError("No RAW files found")
        frames=[]; valid_files=[]
        for p in files:
            try: frames.append(load_raw(p,self.width,self.height)); valid_files.append(p)
            except Exception as exc: self.log.append(f"Skip {Path(p).name}: {exc}")
        if not frames: raise ValueError("No valid frames")
        stack=np.stack(frames).astype(np.float64)
        pm=stack.mean(axis=0); ps=stack.std(axis=0); pmin=stack.min(axis=0); pmax=stack.max(axis=0); p2p=pmax-pmin
        frame_mean=stack.mean(axis=(1,2)); frame_std=stack.std(axis=(1,2))
        out=self.make_output("temporal_drift")
        pm.astype(RAW_DTYPE).tofile(out/"temporal_mean.raw")
        np.save(out/"pixel_std.npy",ps); np.save(out/"pixel_peak_to_peak.npy",p2p)
        mean_preview=raw_to_display_u8(pm,self.raw_max); std_preview=raw_to_display_u8(ps,self.raw_max); p2p_preview=raw_to_display_u8(p2p,self.raw_max)
        save_png(out/"temporal_mean_preview.png",mean_preview); save_png(out/"temporal_std_preview.png",std_preview); save_png(out/"peak_to_peak_preview.png",p2p_preview)
        with open(out/"frame_statistics.csv","w",newline="") as f:
            wr=csv.writer(f); wr.writerow(["frame_index","filename","frame_mean_dn","frame_spatial_std_dn"])
            for i,p in enumerate(valid_files): wr.writerow([i,Path(p).name,f"{frame_mean[i]:.6f}",f"{frame_std[i]:.6f}"])
        std_fail=ps>self.std_spec.value(); p2p_fail=p2p>self.p2p_spec.value()
        self.current_original=frames[0]; self.current_result=ps
        self.preview_result.set_array(std_preview)
        self.log.append(f"Frames={len(frames)}")
        self.log.append(f"Frame mean: mean={frame_mean.mean():.3f}, P-P={np.ptp(frame_mean):.3f}, std={frame_mean.std():.3f}")
        self.log.append(f"Pixel temporal std: mean={ps.mean():.3f}, 95%={np.percentile(ps,95):.3f}, max={ps.max():.3f}")
        self.log.append(f"Std > {self.std_spec.value():.1f}: {std_fail.sum():,} ({std_fail.mean()*100:.3f}%)")
        self.log.append(f"P-P > {self.p2p_spec.value():.1f}: {p2p_fail.sum():,} ({p2p_fail.mean()*100:.3f}%)")
        self.log.append(f"Preview scale: fixed 0..{self.raw_max} DN")

    def run_bpr_marking(self):
        self.bpr_coordinate_info.clear()
        coordinate_path = self.bpr_coordinate_file.text()
        coordinates, table_info = load_bpr_coordinates(coordinate_path)
        valid_coordinates, outside_coordinates = split_coordinates_by_bounds(
            coordinates,
            self.width,
            self.height,
        )
        coordinate_report = format_bpr_coordinate_report(
            table_info,
            coordinates,
            outside_coordinates,
            self.width,
            self.height,
        )
        self.bpr_coordinate_info.setPlainText(coordinate_report)
        if not valid_coordinates:
            raise ValueError(
                f"No BPR coordinates are inside the selected {self.width}x{self.height} resolution."
            )

        use_raw = self.bpr_use_raw.isChecked()
        if use_raw:
            raw_path = self.bpr_raw_file.text()
            raw = load_raw(raw_path, self.width, self.height)
            base_gray = raw_to_display_u8(raw, self.raw_max)
            background_mode = f"RAW fixed scale 0..{self.raw_max} DN"
            base_filename = "raw_fixed.png"
        else:
            raw_path = ""
            raw = None
            base_gray = np.full((self.height, self.width), 255, dtype=np.uint8)
            background_mode = "White canvas"
            base_filename = "white_canvas.png"

        base_rgb = np.repeat(base_gray[:, :, np.newaxis], 3, axis=2)
        marked_rgb = draw_bpr_markers(
            base_gray,
            valid_coordinates,
            self.bpr_circle_radius.value(),
        )
        out = self.make_output("bpr_bad_pixel_marking")
        base_path = out / base_filename
        marked_path = out / "bad_pixels_marked.png"
        coordinate_output_path = out / "bad_pixel_coordinates.csv"
        summary_path = out / "bpr_marking_summary.txt"

        save_rgb_png(base_path, base_rgb)
        save_rgb_png(marked_path, marked_rgb)
        outside_set = set(outside_coordinates)
        with open(coordinate_output_path, "w", newline="", encoding="utf-8") as destination:
            writer = csv.writer(destination)
            writer.writerow(["x", "y", "in_bounds", "index"])
            for index, (x, y) in enumerate(coordinates, 1):
                writer.writerow([x, y, 0 if (x, y) in outside_set else 1, index])

        with open(summary_path, "w", encoding="utf-8") as summary:
            summary.write(f"BPR source: {coordinate_path}\n")
            summary.write(f"Source format: {table_info['format']}\n")
            summary.write(f"Resolution: {self.width}x{self.height}\n")
            summary.write(f"Background: {background_mode}\n")
            if raw_path:
                summary.write(f"RAW source: {raw_path}\n")
            summary.write(f"Circle radius: {self.bpr_circle_radius.value()} px\n")
            summary.write("\n")
            summary.write(coordinate_report)
            summary.write("\n")

        self.current_original = base_rgb
        self.current_result = marked_rgb
        self.preview_original.set_array(base_rgb)
        self.preview_result.set_array(marked_rgb)
        self.log.append(f"BPR source: {coordinate_path}")
        self.log.append(f"Format: {table_info['format']}")
        self.log.append(
            f"Coordinates: entries={table_info['entry_count']:,}, "
            f"unique={table_info['unique_count']:,}, "
            f"duplicates removed={table_info['duplicate_count']:,}"
        )
        self.log.append(
            f"Marked={len(valid_coordinates):,}; outside {self.width}x{self.height}="
            f"{len(outside_coordinates):,}"
        )
        self.log.append(f"Background: {background_mode}")
        self.log.append("All bad-pixel coordinates are shown in Bad Pixel Information.")
        self.log.append(f"Saved: {marked_path}")
        self.log.append(f"Coordinates: {coordinate_output_path}")

    def run_raw_algorithm(self):
        raw = load_raw(self.raw_algorithm_file.text(), self.width, self.height)
        raw8 = raw_to_display_u8(raw, self.raw_max)
        algorithm_key = self.raw_algorithm_combo.currentData()
        algorithm_input = (
            raw
            if algorithm_key in ("fieldscale", "raw_percentile_preview")
            else raw8
        )
        processed, details = apply_image_algorithm(
            algorithm_input,
            algorithm_key,
            low_percentile=self.raw_low_percentile.value(),
            high_percentile=self.raw_high_percentile.value(),
            clahe_clip=self.raw_clahe_clip.value(),
            clahe_grid=self.raw_clahe_grid.value(),
            gamma=self.raw_gamma.value(),
            bilateral_strength=self.raw_bilateral_strength.value(),
            unsharp_amount=self.raw_unsharp_amount.value(),
            input_max=self.raw_max if algorithm_key == "fieldscale" else 255,
        )
        out = self.make_output("raw_algorithm_compare")
        original_path = out / "original_fixed.png"
        result_path = out / f"result_{algorithm_key}.png"
        save_png(original_path, raw8)
        save_png(result_path, processed)
        self.current_original = raw8
        self.current_result = processed
        self.preview_original.set_array(raw8)
        self.preview_result.set_array(processed)
        self.log.append(f"Algorithm: {self.raw_algorithm_combo.currentText()}")
        self.log.append(f"Processing: {' | '.join(details)}")
        if algorithm_key == "fieldscale":
            self.log.append(
                f"Algorithm input: original RAW DN 0..{self.raw_max}; firmware-core parameters"
            )
        elif algorithm_key == "raw_percentile_preview":
            self.log.append("Algorithm input: original RAW DN with direct P1/P99 preview scaling")
        else:
            self.log.append(f"Algorithm input: fixed 0..{self.raw_max} DN -> 0..255")
        self.log_stats("Original 8-bit", raw8)
        self.log_stats("Processed 8-bit", processed)
        self.log.append(f"Saved: {original_path}")
        self.log.append(f"Saved: {result_path}")

    def uvc_result_title(self):
        prefix = "NUC + " if self.uvc_apply_nuc.isChecked() else ""
        return f"{prefix}{self.uvc_algorithm_combo.currentText()}"

    def invalidate_uvc_nuc_cache(self, _value=None):
        self.uvc_nuc_k_table = None
        self.uvc_nuc_b_table = None
        self.uvc_nuc_cache_key = None
        self.reset_uvc_fieldscale_state()

    def reset_uvc_fieldscale_state(self):
        self.uvc_fieldscale_state.clear()

    def auto_pair_uvc_nuc_b_file(self, k_path):
        path = Path(k_path)
        if path.name.lower().endswith("_k.bin"):
            candidate = path.with_name(f"{path.name[:-6]}_B.bin")
            if candidate.is_file():
                self.uvc_nuc_b_file.setText(str(candidate))

    def update_uvc_nuc_ui(self, _checked=None):
        enabled = self.uvc_apply_nuc.isChecked()
        self.uvc_nuc_k_file.setEnabled(enabled)
        self.uvc_nuc_b_file.setEnabled(enabled)
        self.uvc_nuc_note.setEnabled(enabled)
        self.invalidate_uvc_nuc_cache()
        self.preview_result.set_title(self.uvc_result_title())
        self.preview_result_caption.setText(self.uvc_result_title())
        if self.uvc_original_source is not None:
            if not enabled or (self.uvc_nuc_k_file.text() and self.uvc_nuc_b_file.text()):
                self.refresh_uvc_processed_preview()

    def load_uvc_nuc_tables(self):
        k_path = self.uvc_nuc_k_file.text()
        b_path = self.uvc_nuc_b_file.text()
        if not k_path or not b_path:
            raise ValueError("Enable UVC NUC requires both pointN_K.bin and pointN_B.bin.")
        cache_key = (str(Path(k_path).resolve()), str(Path(b_path).resolve()), self.width, self.height)
        if self.uvc_nuc_cache_key != cache_key:
            self.uvc_nuc_k_table = load_novatek_nuc_bin(k_path, self.width, self.height, "<u2", "K")
            self.uvc_nuc_b_table = load_novatek_nuc_bin(b_path, self.width, self.height, "<i2", "B")
            self.uvc_nuc_cache_key = cache_key
        return self.uvc_nuc_k_table, self.uvc_nuc_b_table

    def apply_uvc_nuc(self, original_gray):
        expected_shape = (self.height, self.width)
        if original_gray.shape != expected_shape:
            raise ValueError(
                f"UVC frame is {original_gray.shape[1]}x{original_gray.shape[0]}, but NUC table Resolution is "
                f"{self.width}x{self.height}. Select the matching Resolution."
            )
        stored_k, gfx_b = self.load_uvc_nuc_tables()
        nuc_input, conversion_details = uvc_gray_to_nuc_dn(original_gray, self.raw_max)
        corrected = apply_novatek_nuc(nuc_input, stored_k, gfx_b, self.raw_max)
        return raw_to_display_u8(corrected, self.raw_max), conversion_details

    def refresh_uvc_processed_preview(self):
        if self.uvc_original_source is None:
            return
        try:
            processed, details = self.process_uvc_frame(self.uvc_original_source)
        except Exception as exc:
            message = str(exc)
            if message != self.uvc_last_processing_error:
                self.log.append(f"UVC frame processing error: {message}")
            self.uvc_last_processing_error = message
            self.uvc_processed_frame = None
            self.current_result = None
            self.preview_result.set_array(None)
            self.uvc_capture_btn.setEnabled(False)
            self.uvc_status.setText(f"Frame processing error: {message}")
            return
        self.uvc_last_processing_error = None
        self.uvc_processed_frame = processed.copy()
        self.current_result = self.uvc_processed_frame
        self.preview_result.set_array(processed)
        self.uvc_capture_btn.setEnabled(True)
        self.uvc_status.setText(details)

    def update_uvc_percentile_limits(self, _value=None):
        self.uvc_high_percentile.setMinimum(min(100.0, self.uvc_low_percentile.value() + 0.5))
        self.uvc_low_percentile.setMaximum(max(0.0, self.uvc_high_percentile.value() - 0.5))

    def reset_uvc_algorithm_settings(self):
        algorithm_key = self.uvc_algorithm_combo.currentData()
        reset_items = []
        if algorithm_key in ("auto_contrast", "auto_contrast_clahe"):
            self.uvc_high_percentile.setValue(99.0)
            self.uvc_low_percentile.setValue(1.0)
            reset_items.append("Percentiles P1.0/P99.0")
        if algorithm_key in ("clahe", "auto_contrast_clahe"):
            self.uvc_clahe_clip.setValue(2.0)
            self.uvc_clahe_grid.setValue(8)
            reset_items.append("CLAHE clip=2.0, grid=8x8")
        if algorithm_key == "gamma":
            self.uvc_gamma.setValue(0.7)
            reset_items.append("Gamma=0.7")
        elif algorithm_key == "bilateral":
            self.uvc_bilateral_strength.setValue(2)
            reset_items.append("Bilateral strength=2")
        elif algorithm_key == "unsharp":
            self.uvc_unsharp_amount.setValue(1.0)
            reset_items.append("Unsharp amount=1.0")
        if reset_items:
            self.log.append(f"Reset {self.uvc_algorithm_combo.currentText()}: {', '.join(reset_items)}")
            self.refresh_uvc_processed_preview()

    def set_uvc_parameter_visible(self, widget, visible):
        self.set_form_parameter_visible(self.uvc_form, widget, visible)

    def update_uvc_algorithm_ui(self, _combo_index=None):
        algorithm_key = self.uvc_algorithm_combo.currentData()
        self.reset_uvc_fieldscale_state()
        self.uvc_algorithm_description.setText(
            IMAGE_ALGORITHM_DESCRIPTIONS.get(algorithm_key, "")
        )
        uses_auto_contrast = algorithm_key in ("auto_contrast", "auto_contrast_clahe")
        uses_clahe = algorithm_key in ("clahe", "auto_contrast_clahe")
        self.set_uvc_parameter_visible(self.uvc_low_percentile, uses_auto_contrast)
        self.set_uvc_parameter_visible(self.uvc_high_percentile, uses_auto_contrast)
        self.uvc_reset_btn.setEnabled(
            algorithm_key not in (
                "fieldscale",
                "hist_equalization",
                "raw_percentile_preview",
            )
        )
        self.set_uvc_parameter_visible(self.uvc_clahe_clip, uses_clahe)
        self.set_uvc_parameter_visible(self.uvc_clahe_grid, uses_clahe)
        self.set_uvc_parameter_visible(self.uvc_gamma, algorithm_key == "gamma")
        self.set_uvc_parameter_visible(self.uvc_bilateral_strength, algorithm_key == "bilateral")
        self.set_uvc_parameter_visible(self.uvc_unsharp_amount, algorithm_key == "unsharp")
        self.preview_result.set_title(self.uvc_result_title())
        self.preview_result_caption.setText(self.uvc_result_title())
        self.refresh_uvc_processed_preview()

    def process_uvc_frame(self, original_gray):
        algorithm_key = self.uvc_algorithm_combo.currentData()
        details = []

        if self.uvc_apply_nuc.isChecked():
            processed, conversion_details = self.apply_uvc_nuc(original_gray)
            details.append(f"NUC K/B ({conversion_details})")
        elif algorithm_key in ("fieldscale", "raw_percentile_preview"):
            processed = original_gray.copy()
        elif original_gray.dtype == np.uint8:
            processed = original_gray.copy()
        else:
            processed = raw_to_display_u8(original_gray, self.raw_max)

        clahe = None
        if algorithm_key in ("clahe", "auto_contrast_clahe"):
            clip_limit = self.uvc_clahe_clip.value()
            grid_size = self.uvc_clahe_grid.value()
            clahe_settings = (clip_limit, grid_size)
            if self.uvc_clahe is None or self.uvc_clahe_settings != clahe_settings:
                self.uvc_clahe = cv2.createCLAHE(clipLimit=clip_limit, tileGridSize=(grid_size, grid_size))
                self.uvc_clahe_settings = clahe_settings
            clahe = self.uvc_clahe

        processed, algorithm_details = apply_image_algorithm(
            processed,
            algorithm_key,
            low_percentile=self.uvc_low_percentile.value(),
            high_percentile=self.uvc_high_percentile.value(),
            clahe_clip=self.uvc_clahe_clip.value(),
            clahe_grid=self.uvc_clahe_grid.value(),
            gamma=self.uvc_gamma.value(),
            bilateral_strength=self.uvc_bilateral_strength.value(),
            unsharp_amount=self.uvc_unsharp_amount.value(),
            clahe=clahe,
            input_max=(
                self.raw_max
                if processed.dtype.itemsize > 1
                else 255
            ),
            fieldscale_state=self.uvc_fieldscale_state,
            fieldscale_temporal=algorithm_key == "fieldscale",
        )
        details.extend(algorithm_details)
        return processed, " | ".join(details)

    def start_uvc_camera(self, _checked=False):
        if cv2 is None:
            message = "OpenCV not installed. pip install opencv-python"
            self.log.append(f"\nERROR: {message}")
            QMessageBox.critical(self, "Camera Error", message)
            return
        if self.uvc_apply_nuc.isChecked():
            try:
                self.load_uvc_nuc_tables()
            except Exception as exc:
                message = str(exc)
                self.log.append(f"\nERROR: {message}")
                QMessageBox.critical(self, "UVC NUC Error", message)
                return

        if not self.stop_uvc_camera(log_message=False):
            self.uvc_restart_when_stopped = True
            self.log.append("Previous capture thread is still stopping; camera will start automatically afterward.")
            return
        self.uvc_restart_when_stopped = False
        camera_index = self.uvc_camera_combo.currentData()
        requested_fps = self.uvc_fps_combo.currentData()
        worker = UvcCaptureThread(
            camera_index,
            self.width,
            self.height,
            requested_fps,
            UVC_RECONNECT_SECONDS,
            self,
        )
        worker.frame_ready.connect(self.handle_uvc_frame)
        worker.camera_opened.connect(self.handle_uvc_camera_opened)
        worker.status_changed.connect(self.handle_uvc_worker_status)
        worker.finished.connect(self.handle_uvc_worker_finished)
        worker.finished.connect(worker.deleteLater)
        self.uvc_worker = worker
        self.reset_uvc_fieldscale_state()
        self.uvc_original_source = None
        self.uvc_original_frame = None
        self.uvc_processed_frame = None
        self.uvc_last_processing_error = None
        self.uvc_frame_count = 0
        self.uvc_fps_display = 0.0
        now = time.perf_counter()
        self.uvc_last_time = now
        self.uvc_last_frame_time = now
        self.uvc_waiting_for_frame = False
        self.uvc_reconnect_requested = False
        self.preview_original.set_array(None)
        self.preview_result.set_array(None)
        self.uvc_watchdog_timer.start()
        self.uvc_start_btn.setEnabled(False)
        self.uvc_stop_btn.setEnabled(True)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_status.setText(f"Opening camera {camera_index}...")
        self.log.append(f"Starting camera {camera_index} in background capture thread...")
        if self.uvc_apply_nuc.isChecked():
            self.log.append(
                f"UVC processing order: NUC ({Path(self.uvc_nuc_k_file.text()).name}, "
                f"{Path(self.uvc_nuc_b_file.text()).name}) -> {self.uvc_algorithm_combo.currentText()}"
            )
        worker.start()

    def stop_uvc_camera(self, _checked=False, log_message=True):
        self.uvc_restart_when_stopped = False
        self.reset_uvc_fieldscale_state()
        worker = self.uvc_worker
        was_running = worker is not None
        self.uvc_watchdog_timer.stop()
        if worker is not None:
            worker.stop()
            if not worker.wait(2000):
                self.uvc_start_btn.setEnabled(False)
                self.uvc_stop_btn.setEnabled(False)
                self.uvc_capture_btn.setEnabled(False)
                self.uvc_status.setText("Camera capture thread is stopping...")
                return False
            self.uvc_worker = None
        self.uvc_start_btn.setEnabled(True)
        self.uvc_stop_btn.setEnabled(False)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_status.setText("Camera stopped")
        if was_running and log_message:
            self.log.append("Camera stopped")
        return True

    def switch_uvc_camera(self, _combo_index):
        if self.uvc_worker is None:
            return
        self.start_uvc_camera()

    def handle_uvc_camera_opened(self, camera_index, actual_width, actual_height, actual_fps):
        if self.sender() is not self.uvc_worker:
            return
        self.uvc_last_frame_time = time.perf_counter()
        self.uvc_reconnect_requested = False
        self.uvc_status.setText(
            f"Camera {camera_index} opened: {actual_width}x{actual_height}, reported {actual_fps:.1f} FPS; waiting for frame..."
        )
        self.log.append(
            f"Camera {camera_index} opened: {actual_width}x{actual_height}, requested {self.uvc_fps_combo.currentData()} FPS, reported {actual_fps:.1f} FPS"
        )

    def handle_uvc_worker_status(self, message):
        if self.sender() is not self.uvc_worker:
            return
        self.uvc_status.setText(message)

    def handle_uvc_worker_finished(self):
        if self.sender() is not self.uvc_worker:
            return
        restart = self.uvc_restart_when_stopped
        close_pending = self.uvc_close_pending
        self.uvc_restart_when_stopped = False
        self.uvc_worker = None
        self.uvc_watchdog_timer.stop()
        self.uvc_start_btn.setEnabled(True)
        self.uvc_stop_btn.setEnabled(False)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_status.setText("Camera stopped")
        if close_pending:
            self.uvc_close_pending = False
            QTimer.singleShot(0, self.close)
        elif restart and self.function_combo.currentIndex() == len(self.FUNCTIONS) - 1:
            QTimer.singleShot(0, self.start_uvc_camera)

    def force_close_after_uvc_timeout(self):
        if not self.uvc_close_pending:
            return
        worker = self.uvc_worker
        if worker is not None and worker.isRunning():
            worker.terminate()
            if not worker.wait(500):
                QTimer.singleShot(500, self.force_close_after_uvc_timeout)
                return
        self.uvc_worker = None
        self.uvc_close_pending = False
        QTimer.singleShot(0, self.close)

    def check_uvc_stream(self):
        worker = self.uvc_worker
        if worker is None or not worker.isRunning():
            return
        elapsed = time.perf_counter() - self.uvc_last_frame_time
        if elapsed < UVC_FRAME_WAIT_SECONDS:
            return
        if not self.uvc_waiting_for_frame:
            self.uvc_waiting_for_frame = True
            self.log.append("UVC frame paused; keeping the last image (possible FFC).")
        self.uvc_status.setText(
            f"Waiting for UVC frame {elapsed:.1f}s (possible FFC); last image retained."
        )
        if elapsed >= UVC_RECONNECT_SECONDS and not self.uvc_reconnect_requested:
            self.uvc_reconnect_requested = True
            self.log.append(
                f"No UVC frame for {elapsed:.1f}s; requesting automatic reconnect for camera {self.uvc_camera_combo.currentData()}."
            )
            worker.force_reconnect()

    def handle_uvc_frame(self, frame):
        if self.sender() is not self.uvc_worker:
            return
        now = time.perf_counter()
        self.uvc_last_frame_time = now
        self.uvc_reconnect_requested = False
        if self.uvc_waiting_for_frame:
            self.uvc_waiting_for_frame = False
            self.log.append("UVC stream resumed.")

        try:
            original_gray = uvc_to_gray(frame)
            processed_gray, processing_details = self.process_uvc_frame(original_gray)
        except Exception as exc:
            message = str(exc)
            if message != self.uvc_last_processing_error:
                self.log.append(f"UVC frame processing error: {message}")
            self.uvc_last_processing_error = message
            self.uvc_processed_frame = None
            self.current_result = None
            self.preview_result.set_array(None)
            self.uvc_capture_btn.setEnabled(False)
            self.uvc_status.setText(f"Frame processing error: {message}")
            return
        self.uvc_last_processing_error = None
        self.uvc_original_source = original_gray.copy()
        if original_gray.dtype == np.uint8:
            self.uvc_original_frame = original_gray.copy()
        else:
            self.uvc_original_frame = raw_to_display_u8(original_gray, self.raw_max)
        self.uvc_processed_frame = processed_gray.copy()
        self.current_original = self.uvc_original_frame
        self.current_result = self.uvc_processed_frame
        self.preview_original.set_array(self.uvc_original_frame)
        self.preview_result.set_array(processed_gray)
        self.uvc_capture_btn.setEnabled(True)

        self.uvc_frame_count += 1
        elapsed = now - self.uvc_last_time
        if elapsed >= 0.5:
            self.uvc_fps_display = self.uvc_frame_count / elapsed
            self.uvc_frame_count = 0
            self.uvc_last_time = now
        self.uvc_status.setText(
            f"Camera {self.uvc_camera_combo.currentData()} | {original_gray.shape[1]}x{original_gray.shape[0]} | "
            f"FPS {self.uvc_fps_display:.1f} | {self.uvc_algorithm_combo.currentText()} | {processing_details}"
        )

    def capture_uvc_pair(self):
        if self.uvc_original_frame is None or self.uvc_processed_frame is None:
            QMessageBox.information(self, "No UVC Frame", "Start the camera and wait for a frame first.")
            return
        algorithm_key = self.uvc_algorithm_combo.currentData()
        out = self.make_output("uvc_algorithm_compare")
        original_path = out / "original.png"
        nuc_tag = "nuc_" if self.uvc_apply_nuc.isChecked() else ""
        processed_path = out / f"result_{nuc_tag}{algorithm_key}.png"
        save_png(original_path, self.uvc_original_frame)
        save_png(processed_path, self.uvc_processed_frame)
        self.log.append(f"Saved: {original_path}")
        self.log.append(f"Saved: {processed_path}")

    def save_current_preview(self):
        if self.current_result is None:
            QMessageBox.information(self,"No Preview","Run a function first."); return
        function_index = self.function_combo.currentIndex()
        is_uvc_algorithm = self.FUNCTIONS[function_index] == "UVC Algorithm Compare"
        if is_uvc_algorithm:
            if self.uvc_original_frame is None or self.uvc_processed_frame is None:
                QMessageBox.information(
                    self,
                    "No UVC Frame",
                    "Start the camera and wait for both previews first.",
                )
                return
        out=self.make_output("manual_preview")
        if is_uvc_algorithm:
            original = self.uvc_original_frame.copy()
            processed = self.uvc_processed_frame.copy()
            algorithm_key = self.uvc_algorithm_combo.currentData()
            algorithm_title = self.uvc_result_title()
            nuc_tag = "nuc_" if self.uvc_apply_nuc.isChecked() else ""
            original_path = out / "original_uvc.png"
            processed_path = out / f"processed_{nuc_tag}{algorithm_key}.png"
            comparison_path = out / f"comparison_{nuc_tag}{algorithm_key}.png"
            save_png(original_path, original)
            save_png(processed_path, processed)
            save_uvc_comparison_png(
                comparison_path, original, processed, algorithm_title
            )
            self.log.append(f"Saved UVC algorithm: {algorithm_title}")
            self.log.append(f"Saved original: {original_path}")
            self.log.append(f"Saved processed: {processed_path}")
            self.log.append(f"Saved comparison: {comparison_path}")
            return

        path=out/"preview.png"
        is_bpr_marking = self.FUNCTIONS[function_index] == "BPR Bad Pixel Marking"
        if is_bpr_marking:
            preview_to_save = np.clip(self.current_result, 0, 255).astype(np.uint8)
        elif function_index == len(self.FUNCTIONS) - 1:
            preview_to_save = self.current_result
        elif function_index == 4:
            preview_to_save = signed_delta_to_display_u8(self.current_result, self.raw_max)
        elif function_index in (0, 1, 2, 3, 5):
            preview_to_save = raw_to_display_u8(self.current_result, self.raw_max)
        else:
            preview_to_save = np.clip(self.current_result, 0, 255).astype(np.uint8)
        if is_bpr_marking:
            save_rgb_png(path, preview_to_save)
        else:
            save_png(path, preview_to_save)
        self.log.append(f"Saved preview: {path}")

    def open_last_result(self):
        if not self.last_output_dir or not os.path.isdir(self.last_output_dir):
            QMessageBox.information(self,"No Result","No result folder yet."); return
        path=self.last_output_dir
        if sys.platform.startswith("win"): os.startfile(path)
        elif sys.platform=="darwin": os.system(f'open "{path}"')
        else: os.system(f'xdg-open "{path}" >/dev/null 2>&1 &')

    def closeEvent(self, event):
        self.uvc_restart_when_stopped = False
        self.uvc_watchdog_timer.stop()
        worker = self.uvc_worker
        if worker is not None and worker.isRunning():
            if not self.uvc_close_pending:
                self.uvc_close_pending = True
                self.uvc_start_btn.setEnabled(False)
                self.uvc_stop_btn.setEnabled(False)
                self.uvc_capture_btn.setEnabled(False)
                self.uvc_status.setText("Stopping camera before exit...")
                worker.stop()
                QTimer.singleShot(
                    UVC_CLOSE_TIMEOUT_MS,
                    self.force_close_after_uvc_timeout,
                )
            event.ignore()
            return
        self.uvc_worker = None
        self.uvc_close_pending = False
        super().closeEvent(event)


def main():
    app=QApplication(sys.argv)
    app.setStyle("Fusion")
    w=ThermalRawToolkit(); w.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()

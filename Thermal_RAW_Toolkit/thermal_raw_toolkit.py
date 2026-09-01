import os
import sys
import csv
import glob
import json
import re
import time
import threading
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

import numpy as np

try:
    import cv2
except ImportError:
    cv2 = None

try:
    from PyQt5.QtCore import Qt, QThread, QTimer, pyqtSignal
    from PyQt5.QtGui import QColor, QFont, QImage, QPainter, QPixmap
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QFileDialog, QMessageBox,
        QVBoxLayout, QHBoxLayout, QGridLayout, QFormLayout, QLabel,
        QPushButton, QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit,
        QTextEdit, QGroupBox, QStackedWidget, QSplitter, QCheckBox,
        QListWidget, QListWidgetItem, QScrollArea, QFrame, QSizePolicy
    )
except ImportError as exc:
    raise SystemExit("PyQt5 is required. Install with: pip install PyQt5") from exc

try:
    from PyQt5.QtMultimedia import QCameraInfo
except (ImportError, OSError):
    QCameraInfo = None


APP_VERSION = "v1.1.0"
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
CAMERA_DEVICE_ID_ROLE = Qt.UserRole + 1
CAMERA_FRIENDLY_NAME_ROLE = Qt.UserRole + 2
CAMERA_REFRESH_INTERVAL_MS = 1000
DEFAULT_UVC_FPS = 30
UVC_CAPTURE_FOURCC = "MJPG"
DEFAULT_UVC_CAPTURE_MODE = "auto_detect"
UVC_CAPTURE_MODE_OPTIONS = [
    ("Auto Detect (MJPG -> Generic)", "auto_detect"),
    ("Novatek Thermal (MJPG Strict)", "novatek_mjpg"),
    ("Generic Webcam (Auto Format)", "generic_auto"),
]
UVC_AUTO_DETECT_FRAME_TIMEOUT_SECONDS = 2.0
UVC_FRAME_WAIT_SECONDS = 0.5
UVC_RECONNECT_SECONDS = 5.0
UVC_CLOSE_TIMEOUT_MS = 3000
UVC_RECORDING_FOURCC = "MJPG"
UVC_RECORDING_QUEUE_SIZE = 8
DUAL_UVC_IMAGE_NAME_OPTIONS = [
    ("PEGA", "PEGA"),
    ("天淵", "TianYuan"),
    ("方見", "FangJian"),
]


def camera_device_id_text(device_name):
    if isinstance(device_name, str):
        return device_name
    try:
        return bytes(device_name).decode("utf-8", errors="replace")
    except (TypeError, ValueError):
        return str(device_name)


def enumerate_camera_devices():
    """Return camera index/name/stable ID in Qt multimedia enumeration order."""
    if QCameraInfo is not None:
        try:
            devices = []
            for camera_index, camera_info in enumerate(
                QCameraInfo.availableCameras()
            ):
                friendly_name = str(camera_info.description()).strip()
                device_id = camera_device_id_text(camera_info.deviceName())
                devices.append(
                    {
                        "index": camera_index,
                        "name": friendly_name or f"Camera {camera_index}",
                        "device_id": device_id or f"camera-index:{camera_index}",
                    }
                )
            return devices
        except Exception:
            pass

    return [
        {
            "index": camera_index,
            "name": f"Camera {camera_index}",
            "device_id": f"camera-index:{camera_index}",
        }
        for camera_index in CAMERA_INDEX_OPTIONS
    ]


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
PIPELINE_ALGORITHM_OPTIONS = [
    ("Percentile Auto Contrast", "auto_contrast"),
    ("Fieldscale Local Adaptive AGC (Firmware Core)", "fieldscale"),
    ("Histogram Equalization", "hist_equalization"),
    ("CLAHE", "clahe"),
    ("Gamma Correction", "gamma"),
    ("Bilateral Denoise", "bilateral"),
    ("Unsharp Mask", "unsharp"),
]
RAW_INPUT_MAPPING_OPTIONS = [
    ("Fixed Bit Depth Scale", "fixed_scale"),
    ("P1/P99 from Original RAW DN", "raw_percentile_preview"),
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

DEFAULT_ALGORITHM_PARAMETERS = {
    "auto_contrast": {"low_percentile": 1.0, "high_percentile": 99.0},
    "fieldscale": {},
    "hist_equalization": {},
    "clahe": {"clahe_clip": 2.0, "clahe_grid": 8},
    "gamma": {"gamma": 0.7},
    "bilateral": {"bilateral_strength": 2},
    "unsharp": {"unsharp_amount": 1.0},
}


def algorithm_display_name(algorithm_key):
    for display_name, key in PIPELINE_ALGORITHM_OPTIONS:
        if key == algorithm_key:
            return display_name
    return algorithm_key


def default_algorithm_parameters(algorithm_key):
    if algorithm_key not in DEFAULT_ALGORITHM_PARAMETERS:
        raise ValueError(f"Unsupported pipeline algorithm: {algorithm_key}")
    return dict(DEFAULT_ALGORITHM_PARAMETERS[algorithm_key])


@dataclass
class AlgorithmStage:
    algorithm_key: str
    enabled: bool = True
    parameters: dict = field(default_factory=dict)

    def __post_init__(self):
        defaults = default_algorithm_parameters(self.algorithm_key)
        defaults.update(self.parameters)
        self.parameters = defaults

    def clone(self):
        return AlgorithmStage(self.algorithm_key, self.enabled, dict(self.parameters))

    def to_dict(self):
        return {
            "algorithm": self.algorithm_key,
            "name": algorithm_display_name(self.algorithm_key),
            "enabled": bool(self.enabled),
            "parameters": dict(self.parameters),
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


def apply_algorithm_pipeline(
    gray,
    stages,
    input_max=255,
    clahe_cache=None,
    fieldscale_states=None,
    fieldscale_temporal=False,
):
    """Apply enabled stages in top-to-bottom order and keep every step result."""
    if cv2 is None:
        raise RuntimeError("OpenCV not installed. pip install opencv-python")
    if gray.ndim != 2:
        raise ValueError("Algorithm pipeline input must be a 2D grayscale image.")
    if not any(stage.enabled for stage in stages):
        raise ValueError("Add and enable at least one algorithm stage.")

    processed = np.ascontiguousarray(gray.copy())
    current_max = int(input_max)
    records = []
    execution_index = 0
    for pipeline_index, stage in enumerate(stages, start=1):
        if not stage.enabled:
            continue
        execution_index += 1
        params = default_algorithm_parameters(stage.algorithm_key)
        params.update(stage.parameters)
        clahe = None
        if stage.algorithm_key == "clahe" and clahe_cache is not None:
            cache_key = (
                pipeline_index,
                float(params["clahe_clip"]),
                int(params["clahe_grid"]),
            )
            clahe = clahe_cache.get(cache_key)
            if clahe is None:
                clahe = cv2.createCLAHE(
                    clipLimit=cache_key[1],
                    tileGridSize=(cache_key[2], cache_key[2]),
                )
                clahe_cache[cache_key] = clahe

        fieldscale_state = None
        if stage.algorithm_key == "fieldscale" and fieldscale_states is not None:
            fieldscale_state = fieldscale_states.setdefault(pipeline_index, {})

        started = time.perf_counter()
        processed, details = apply_image_algorithm(
            processed,
            stage.algorithm_key,
            low_percentile=params.get("low_percentile", 1.0),
            high_percentile=params.get("high_percentile", 99.0),
            clahe_clip=params.get("clahe_clip", 2.0),
            clahe_grid=params.get("clahe_grid", 8),
            gamma=params.get("gamma", 0.7),
            bilateral_strength=params.get("bilateral_strength", 2),
            unsharp_amount=params.get("unsharp_amount", 1.0),
            clahe=clahe,
            input_max=current_max,
            fieldscale_state=fieldscale_state,
            fieldscale_temporal=(
                fieldscale_temporal and stage.algorithm_key == "fieldscale"
            ),
        )
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        current_max = 255
        records.append(
            {
                "pipeline_index": pipeline_index,
                "execution_index": execution_index,
                "algorithm": stage.algorithm_key,
                "name": algorithm_display_name(stage.algorithm_key),
                "parameters": dict(params),
                "details": list(details),
                "elapsed_ms": elapsed_ms,
                "image": processed,
            }
        )
    return processed, records


def pipeline_execution_json(records):
    return [
        {
            "pipeline_index": record["pipeline_index"],
            "execution_index": record["execution_index"],
            "algorithm": record["algorithm"],
            "name": record["name"],
            "parameters": dict(record["parameters"]),
            "details": list(record["details"]),
            "elapsed_ms": round(float(record["elapsed_ms"]), 3),
        }
        for record in records
    ]


def pipeline_summary_text(stages):
    names = [algorithm_display_name(stage.algorithm_key) for stage in stages if stage.enabled]
    return " -> ".join(names) if names else "No enabled algorithm"


def pipeline_record_image(records, pipeline_index, final_image):
    if pipeline_index is None:
        return final_image
    for record in records:
        if record["pipeline_index"] == pipeline_index:
            return record["image"]
    return final_image


def uvc_to_gray(frame):
    if frame.ndim == 2:
        return frame
    if frame.shape[2] == 1:
        return frame[:, :, 0]
    if frame.shape[2] == 4:
        return cv2.cvtColor(frame, cv2.COLOR_BGRA2GRAY)
    return cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)


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


def save_pipeline_overview_png(path, items, max_columns=3):
    """Save numbered pipeline images as one labeled contact sheet."""
    if cv2 is None:
        raise RuntimeError("OpenCV is required to save PNG. Install: pip install opencv-python")
    if not items:
        raise ValueError("Pipeline overview requires at least one image.")

    prepared = []
    for label, image, display_max in items:
        if image.ndim != 2:
            raise ValueError("Pipeline overview supports grayscale images only.")
        if display_max <= 0:
            raise ValueError("Pipeline overview display maximum must be positive.")
        image_u8 = np.rint(
            np.clip(image, 0, display_max).astype(np.float64)
            * 255.0
            / display_max
        ).astype(np.uint8)
        prepared.append((str(label), image_u8))

    source_height, source_width = prepared[0][1].shape
    if any(image.shape != (source_height, source_width) for _, image in prepared):
        raise ValueError("All pipeline overview images must have the same dimensions.")

    scale = min(1.0, 480.0 / source_width, 360.0 / source_height)
    tile_width = max(1, int(round(source_width * scale)))
    tile_height = max(1, int(round(source_height * scale)))
    header_height = 42
    gap = 8
    margin = 8
    columns = min(max(1, int(max_columns)), len(prepared))
    rows = (len(prepared) + columns - 1) // columns
    panel_width = tile_width
    panel_height = header_height + tile_height
    canvas_width = margin * 2 + columns * panel_width + (columns - 1) * gap
    canvas_height = margin * 2 + rows * panel_height + (rows - 1) * gap
    canvas = np.full((canvas_height, canvas_width), 24, dtype=np.uint8)

    for item_index, (label, image_u8) in enumerate(prepared):
        row = item_index // columns
        column = item_index % columns
        x = margin + column * (panel_width + gap)
        y = margin + row * (panel_height + gap)
        resized = (
            image_u8
            if image_u8.shape == (tile_height, tile_width)
            else cv2.resize(
                image_u8,
                (tile_width, tile_height),
                interpolation=cv2.INTER_AREA,
            )
        )
        canvas[y + header_height:y + panel_height, x:x + tile_width] = resized
        cv2.rectangle(
            canvas,
            (x, y),
            (x + panel_width - 1, y + panel_height - 1),
            72,
            1,
        )
        font_scale = min(
            0.62,
            max(0.36, panel_width / max(500.0, len(label) * 17.0)),
        )
        text_size, _ = cv2.getTextSize(
            label, cv2.FONT_HERSHEY_SIMPLEX, font_scale, 1
        )
        text_x = x + max(6, (panel_width - text_size[0]) // 2)
        cv2.putText(
            canvas,
            label,
            (text_x, y + 28),
            cv2.FONT_HERSHEY_SIMPLEX,
            font_scale,
            255,
            1,
            cv2.LINE_AA,
        )

    save_png(path, canvas)


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


class AlgorithmPipelineEditor(QWidget):
    pipeline_changed = pyqtSignal()

    def __init__(self, parent=None, allow_step_algorithm_change=True):
        super().__init__(parent)
        self._stages = []
        self._updating = False
        self._allow_step_algorithm_change = bool(
            allow_step_algorithm_change
        )

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(7)
        title = QLabel("Algorithm Pipeline (top to bottom)")
        title.setWordWrap(True)
        title.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        title.setStyleSheet("font-size:15px; font-weight:700; color:#c9e2f2;")
        root.addWidget(title)

        self.add_algorithm_combo = QComboBox()
        for display_name, algorithm_key in PIPELINE_ALGORITHM_OPTIONS:
            self.add_algorithm_combo.addItem(display_name, algorithm_key)
        self.add_algorithm_combo.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.add_button = QPushButton("Add Step")
        self.add_button.setObjectName("primaryButton")
        root.addWidget(self.add_algorithm_combo)
        root.addWidget(self.add_button)

        self.stage_list = QListWidget()
        self.stage_list.setMinimumHeight(132)
        self.stage_list.setMaximumHeight(190)
        self.stage_list.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.stage_list.setTextElideMode(Qt.ElideRight)
        self.stage_list.setToolTip(
            "Checked steps run from top to bottom. Duplicate algorithms are allowed."
        )
        root.addWidget(self.stage_list)

        order_grid = QGridLayout()
        order_grid.setHorizontalSpacing(7)
        order_grid.setVerticalSpacing(7)
        self.move_up_button = QPushButton("Move Up")
        self.move_down_button = QPushButton("Move Down")
        self.remove_button = QPushButton("Remove")
        self.reset_step_button = QPushButton("Reset Step")
        self.reset_pipeline_button = QPushButton("Reset Pipeline")
        self.reset_pipeline_button.setObjectName("resetButton")
        order_grid.addWidget(self.move_up_button, 0, 0)
        order_grid.addWidget(self.move_down_button, 0, 1)
        order_grid.addWidget(self.remove_button, 1, 0)
        order_grid.addWidget(self.reset_step_button, 1, 1)
        order_grid.addWidget(self.reset_pipeline_button, 2, 0, 1, 2)
        for button in (
            self.move_up_button,
            self.move_down_button,
            self.remove_button,
            self.reset_step_button,
            self.reset_pipeline_button,
        ):
            button.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
        root.addLayout(order_grid)

        self.parameter_group = QGroupBox("Step Parameters")
        self.parameter_group.setObjectName("pipelineParameterCard")
        self.parameter_form = QFormLayout(self.parameter_group)
        self.parameter_form.setRowWrapPolicy(QFormLayout.WrapAllRows)
        self.stage_algorithm_combo = QComboBox()
        for display_name, algorithm_key in PIPELINE_ALGORITHM_OPTIONS:
            self.stage_algorithm_combo.addItem(display_name, algorithm_key)
        self.stage_algorithm_combo.setSizePolicy(
            QSizePolicy.Expanding, QSizePolicy.Fixed
        )
        self.stage_description = QLabel()
        self.stage_description.setWordWrap(True)
        self.stage_description.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        self.stage_description.setStyleSheet(
            "color:#c8d0d9; background:#292d33; border-left:3px solid #79afd3; "
            "border-radius:5px; padding:7px;"
        )
        self.stage_low_percentile = QDoubleSpinBox()
        self.stage_low_percentile.setRange(0.0, 99.5)
        self.stage_low_percentile.setDecimals(1)
        self.stage_low_percentile.setSingleStep(0.5)
        self.stage_high_percentile = QDoubleSpinBox()
        self.stage_high_percentile.setRange(0.5, 100.0)
        self.stage_high_percentile.setDecimals(1)
        self.stage_high_percentile.setSingleStep(0.5)
        self.stage_clahe_clip = QDoubleSpinBox()
        self.stage_clahe_clip.setRange(0.1, 100.0)
        self.stage_clahe_clip.setDecimals(1)
        self.stage_clahe_clip.setSingleStep(0.1)
        self.stage_clahe_grid = QSpinBox()
        self.stage_clahe_grid.setRange(1, 64)
        self.stage_gamma = QDoubleSpinBox()
        self.stage_gamma.setRange(0.1, 3.0)
        self.stage_gamma.setDecimals(1)
        self.stage_gamma.setSingleStep(0.1)
        self.stage_bilateral_strength = QSpinBox()
        self.stage_bilateral_strength.setRange(1, 7)
        self.stage_unsharp_amount = QDoubleSpinBox()
        self.stage_unsharp_amount.setRange(0.1, 3.0)
        self.stage_unsharp_amount.setDecimals(1)
        self.stage_unsharp_amount.setSingleStep(0.1)
        if self._allow_step_algorithm_change:
            self.parameter_form.addRow("Algorithm", self.stage_algorithm_combo)
        self.parameter_form.addRow(self.stage_description)
        self.parameter_form.addRow("Low Percentile", self.stage_low_percentile)
        self.parameter_form.addRow("High Percentile", self.stage_high_percentile)
        self.parameter_form.addRow("CLAHE Clip Limit", self.stage_clahe_clip)
        self.parameter_form.addRow("CLAHE Grid Size", self.stage_clahe_grid)
        self.parameter_form.addRow("Gamma", self.stage_gamma)
        self.parameter_form.addRow("Bilateral Strength", self.stage_bilateral_strength)
        self.parameter_form.addRow("Unsharp Amount", self.stage_unsharp_amount)
        root.addWidget(self.parameter_group)

        self.add_button.clicked.connect(self.add_stage)
        self.move_up_button.clicked.connect(lambda: self.move_selected(-1))
        self.move_down_button.clicked.connect(lambda: self.move_selected(1))
        self.remove_button.clicked.connect(self.remove_selected)
        self.reset_step_button.clicked.connect(self.reset_selected_step)
        self.reset_pipeline_button.clicked.connect(self.reset_pipeline)
        self.stage_list.currentRowChanged.connect(self.load_selected_step)
        self.stage_list.itemChanged.connect(self.stage_check_changed)
        if self._allow_step_algorithm_change:
            self.stage_algorithm_combo.currentIndexChanged.connect(
                self.selected_algorithm_changed
            )
        for widget in (
            self.stage_low_percentile,
            self.stage_high_percentile,
            self.stage_clahe_clip,
            self.stage_clahe_grid,
            self.stage_gamma,
            self.stage_bilateral_strength,
            self.stage_unsharp_amount,
        ):
            widget.valueChanged.connect(self.selected_parameter_changed)
        self.set_stages([AlgorithmStage("auto_contrast")])

    def stages(self):
        return [stage.clone() for stage in self._stages]

    def enabled_stages(self):
        return [stage.clone() for stage in self._stages if stage.enabled]

    def configuration(self):
        return [stage.to_dict() for stage in self._stages]

    def summary(self):
        return pipeline_summary_text(self._stages)

    def set_stages(self, stages, selected_row=0):
        self._stages = [
            stage.clone()
            if isinstance(stage, AlgorithmStage)
            else AlgorithmStage(
                stage["algorithm"],
                stage.get("enabled", True),
                stage.get("parameters", {}),
            )
            for stage in stages
        ]
        self.refresh_stage_list(selected_row)

    @staticmethod
    def parameter_summary(stage):
        params = stage.parameters
        if stage.algorithm_key == "auto_contrast":
            return f"P{params['low_percentile']:.1f}/P{params['high_percentile']:.1f}"
        if stage.algorithm_key == "clahe":
            return f"clip={params['clahe_clip']:.1f}, grid={params['clahe_grid']}"
        if stage.algorithm_key == "gamma":
            return f"gamma={params['gamma']:.1f}"
        if stage.algorithm_key == "bilateral":
            return f"strength={params['bilateral_strength']}"
        if stage.algorithm_key == "unsharp":
            return f"amount={params['unsharp_amount']:.1f}"
        return "no parameters"

    def refresh_stage_list(self, selected_row=None):
        if selected_row is None:
            selected_row = self.stage_list.currentRow()
        self._updating = True
        self.stage_list.blockSignals(True)
        self.stage_list.clear()
        for index, stage in enumerate(self._stages, start=1):
            text = (
                f"{index}. {algorithm_display_name(stage.algorithm_key)}"
                f"    [{self.parameter_summary(stage)}]"
            )
            item = QListWidgetItem(text)
            item.setToolTip(text)
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(Qt.Checked if stage.enabled else Qt.Unchecked)
            self.stage_list.addItem(item)
        self.stage_list.blockSignals(False)
        self._updating = False
        if self._stages:
            self.stage_list.setCurrentRow(
                min(max(selected_row, 0), len(self._stages) - 1)
            )
        else:
            self.load_selected_step(-1)
        self.update_button_states()

    def add_stage(self, _checked=False):
        self._stages.append(AlgorithmStage(self.add_algorithm_combo.currentData()))
        self.refresh_stage_list(len(self._stages) - 1)
        self.pipeline_changed.emit()

    def remove_selected(self, _checked=False):
        row = self.stage_list.currentRow()
        if row < 0:
            return
        del self._stages[row]
        self.refresh_stage_list(min(row, len(self._stages) - 1))
        self.pipeline_changed.emit()

    def move_selected(self, offset):
        row = self.stage_list.currentRow()
        target = row + offset
        if row < 0 or target < 0 or target >= len(self._stages):
            return
        self._stages[row], self._stages[target] = self._stages[target], self._stages[row]
        self.refresh_stage_list(target)
        self.pipeline_changed.emit()

    def reset_selected_step(self, _checked=False):
        row = self.stage_list.currentRow()
        if row < 0:
            return
        self._stages[row].parameters = default_algorithm_parameters(
            self._stages[row].algorithm_key
        )
        self.refresh_stage_list(row)
        self.pipeline_changed.emit()

    def reset_pipeline(self, _checked=False):
        self.set_stages([AlgorithmStage("auto_contrast")])
        self.pipeline_changed.emit()

    def stage_check_changed(self, item):
        if self._updating:
            return
        row = self.stage_list.row(item)
        if 0 <= row < len(self._stages):
            self._stages[row].enabled = item.checkState() == Qt.Checked
            self.pipeline_changed.emit()

    def selected_algorithm_changed(self, _combo_index):
        if self._updating:
            return
        row = self.stage_list.currentRow()
        if row < 0:
            return
        self._stages[row] = AlgorithmStage(
            self.stage_algorithm_combo.currentData(),
            enabled=self._stages[row].enabled,
        )
        self.refresh_stage_list(row)
        self.pipeline_changed.emit()

    def selected_parameter_changed(self, _value=None):
        if self._updating:
            return
        row = self.stage_list.currentRow()
        if row < 0:
            return
        self.update_percentile_limits()
        stage = self._stages[row]
        if stage.algorithm_key == "auto_contrast":
            stage.parameters["low_percentile"] = self.stage_low_percentile.value()
            stage.parameters["high_percentile"] = self.stage_high_percentile.value()
        elif stage.algorithm_key == "clahe":
            stage.parameters["clahe_clip"] = self.stage_clahe_clip.value()
            stage.parameters["clahe_grid"] = self.stage_clahe_grid.value()
        elif stage.algorithm_key == "gamma":
            stage.parameters["gamma"] = self.stage_gamma.value()
        elif stage.algorithm_key == "bilateral":
            stage.parameters["bilateral_strength"] = self.stage_bilateral_strength.value()
        elif stage.algorithm_key == "unsharp":
            stage.parameters["unsharp_amount"] = self.stage_unsharp_amount.value()
        self.refresh_stage_list(row)
        self.pipeline_changed.emit()

    def set_parameter_visible(self, widget, visible):
        widget.setVisible(visible)
        label = self.parameter_form.labelForField(widget)
        if label is not None:
            label.setVisible(visible)

    def load_selected_step(self, row):
        valid = 0 <= row < len(self._stages)
        self.parameter_group.setEnabled(valid)
        self._updating = True
        if valid:
            stage = self._stages[row]
            index = self.stage_algorithm_combo.findData(stage.algorithm_key)
            self.stage_algorithm_combo.setCurrentIndex(max(index, 0))
            params = stage.parameters
            self.stage_low_percentile.setValue(params.get("low_percentile", 1.0))
            self.stage_high_percentile.setValue(params.get("high_percentile", 99.0))
            self.stage_clahe_clip.setValue(params.get("clahe_clip", 2.0))
            self.stage_clahe_grid.setValue(params.get("clahe_grid", 8))
            self.stage_gamma.setValue(params.get("gamma", 0.7))
            self.stage_bilateral_strength.setValue(params.get("bilateral_strength", 2))
            self.stage_unsharp_amount.setValue(params.get("unsharp_amount", 1.0))
            self.stage_description.setText(
                IMAGE_ALGORITHM_DESCRIPTIONS.get(stage.algorithm_key, "")
            )
            self.set_parameter_visible(
                self.stage_low_percentile, stage.algorithm_key == "auto_contrast"
            )
            self.set_parameter_visible(
                self.stage_high_percentile, stage.algorithm_key == "auto_contrast"
            )
            self.set_parameter_visible(
                self.stage_clahe_clip, stage.algorithm_key == "clahe"
            )
            self.set_parameter_visible(
                self.stage_clahe_grid, stage.algorithm_key == "clahe"
            )
            self.set_parameter_visible(self.stage_gamma, stage.algorithm_key == "gamma")
            self.set_parameter_visible(
                self.stage_bilateral_strength, stage.algorithm_key == "bilateral"
            )
            self.set_parameter_visible(
                self.stage_unsharp_amount, stage.algorithm_key == "unsharp"
            )
            self.update_percentile_limits()
        else:
            self.stage_description.setText("Add a step to configure its parameters.")
        self._updating = False
        self.update_button_states()

    def update_percentile_limits(self):
        self.stage_high_percentile.setMinimum(
            min(100.0, self.stage_low_percentile.value() + 0.5)
        )
        self.stage_low_percentile.setMaximum(
            max(0.0, self.stage_high_percentile.value() - 0.5)
        )

    def update_button_states(self):
        row = self.stage_list.currentRow()
        valid = 0 <= row < len(self._stages)
        self.move_up_button.setEnabled(valid and row > 0)
        self.move_down_button.setEnabled(valid and row < len(self._stages) - 1)
        self.remove_button.setEnabled(valid)
        self.reset_step_button.setEnabled(valid)


class LatestFrameBuffer:
    """Thread-safe single-frame mailbox; newer frames replace pending old ones."""

    def __init__(self):
        self._lock = threading.Lock()
        self._frame = None
        self._frame_time = 0.0
        self._sequence = 0

    def submit(self, frame):
        if frame is None or not getattr(frame, "size", 0):
            return
        with self._lock:
            self._frame = frame.copy()
            self._frame_time = time.perf_counter()
            self._sequence += 1

    def take_latest(self, previous_sequence):
        with self._lock:
            if self._frame is None or self._sequence == previous_sequence:
                return None, previous_sequence, 0.0
            return self._frame.copy(), self._sequence, self._frame_time

    def clear(self):
        with self._lock:
            self._frame = None
            self._frame_time = 0.0
            self._sequence = 0


class UvcCaptureThread(QThread):
    frame_ready = pyqtSignal(object)
    frame_seen = pyqtSignal(float)
    camera_opened = pyqtSignal(int, int, int, float)
    status_changed = pyqtSignal(str)

    def __init__(
        self,
        camera_index,
        width,
        height,
        fps,
        reconnect_seconds,
        parent=None,
        frame_consumer=None,
        capture_mode=DEFAULT_UVC_CAPTURE_MODE,
        camera_device_id=None,
        camera_name=None,
    ):
        super().__init__(parent)
        self.camera_index = camera_index
        self.width = width
        self.height = height
        self.fps = fps
        self.reconnect_seconds = reconnect_seconds
        self.frame_consumer = frame_consumer
        self.capture_mode = capture_mode
        self.camera_device_id = camera_device_id
        self.camera_name = camera_name or f"Camera {camera_index}"
        self.selected_capture_mode = None
        self._camera_info_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._reconnect_event = threading.Event()
        self._capture_lock = threading.Lock()
        self._capture = None

    def stop(self):
        self._stop_event.set()

    def force_reconnect(self):
        self._reconnect_event.set()

    def update_camera_device(self, camera_index, camera_name=None):
        with self._camera_info_lock:
            self.camera_index = camera_index
            if camera_name:
                self.camera_name = camera_name

    def _camera_device_snapshot(self):
        with self._camera_info_lock:
            return self.camera_index, self.camera_name

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

    def _open_capture(self, capture_mode=None, camera_index=None):
        capture_mode = capture_mode or self.capture_mode
        if camera_index is None:
            camera_index, _camera_name = self._camera_device_snapshot()
        if camera_index is None:
            return None
        if capture_mode == "generic_auto":
            if sys.platform.startswith("win"):
                backends = [cv2.CAP_DSHOW]
                if hasattr(cv2, "CAP_MSMF"):
                    backends.append(cv2.CAP_MSMF)
                for backend in backends:
                    try:
                        capture = cv2.VideoCapture(camera_index, backend)
                    except (TypeError, cv2.error):
                        continue
                    if capture.isOpened():
                        return capture
                    capture.release()
            return cv2.VideoCapture(camera_index)

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
                    camera_index,
                    cv2.CAP_DSHOW,
                    open_params,
                )
            except (TypeError, cv2.error):
                capture = cv2.VideoCapture(camera_index, cv2.CAP_DSHOW)

            if capture.isOpened():
                return capture

            capture.release()
            capture = cv2.VideoCapture(camera_index, cv2.CAP_DSHOW)
            if capture.isOpened():
                capture.set(cv2.CAP_PROP_FOURCC, mjpg_fourcc)
                capture.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
                capture.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
                capture.set(cv2.CAP_PROP_FOURCC, mjpg_fourcc)
            return capture

        capture = cv2.VideoCapture(camera_index)
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
    def _capture_mode_name(capture_mode):
        for mode_name, mode_key in UVC_CAPTURE_MODE_OPTIONS:
            if mode_key == capture_mode:
                return mode_name
        return capture_mode

    @staticmethod
    def _alternate_auto_mode(capture_mode):
        if capture_mode == "novatek_mjpg":
            return "generic_auto"
        return "novatek_mjpg"

    @staticmethod
    def _fourcc_text(value):
        fourcc = int(value)
        text = "".join(chr((fourcc >> (8 * index)) & 0xFF) for index in range(4))
        return text.strip("\x00 ") or "UNKNOWN"

    def run(self):
        capture = None
        next_auto_mode = "novatek_mjpg"
        try:
            while not self._stop_event.is_set():
                self._reconnect_event.clear()
                camera_index, camera_name = self._camera_device_snapshot()
                if camera_index is None:
                    self.status_changed.emit(
                        f"{camera_name} disconnected; waiting for hot-plug..."
                    )
                    if self._stop_event.wait(1.0):
                        break
                    continue
                active_capture_mode = self.capture_mode
                if self.capture_mode == "auto_detect":
                    active_capture_mode = next_auto_mode
                    self.status_changed.emit(
                        f"Auto Detect: trying {camera_name} "
                        f"(index {camera_index}) with "
                        f"{self._capture_mode_name(active_capture_mode)}..."
                    )
                else:
                    self.status_changed.emit(
                        f"Opening {camera_name} (index {camera_index}) "
                        f"({self._capture_mode_name(active_capture_mode)})..."
                    )
                try:
                    capture = self._open_capture(
                        active_capture_mode, camera_index
                    )
                except Exception as exc:
                    capture = None
                    self.status_changed.emit(f"Camera open error: {exc}; retrying...")

                if capture is None or not capture.isOpened():
                    if capture is not None:
                        self._clear_capture(capture)
                        capture = None
                    if self.capture_mode == "auto_detect":
                        next_auto_mode = self._alternate_auto_mode(
                            active_capture_mode
                        )
                    self.status_changed.emit(
                        f"Cannot open {camera_name} (index {camera_index}); "
                        "retrying..."
                    )
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
                    and active_capture_mode == "novatek_mjpg"
                    and actual_fourcc != UVC_CAPTURE_FOURCC
                ):
                    self.status_changed.emit(
                        f"{camera_name} negotiated {actual_fourcc!r}, "
                        f"expected {UVC_CAPTURE_FOURCC}; retrying..."
                    )
                    self._clear_capture(capture)
                    capture = None
                    if self.capture_mode == "auto_detect":
                        next_auto_mode = "generic_auto"
                    if self._stop_event.wait(1.0):
                        break
                    continue
                self.status_changed.emit(
                    f"{camera_name} (index {camera_index}): {actual_fourcc} "
                    f"{actual_width}x{actual_height} {actual_fps:.2f} FPS"
                )
                self.camera_opened.emit(
                    camera_index, actual_width, actual_height, actual_fps
                )

                failed_since = None
                received_frame = False
                last_frame_seen_emit = 0.0
                while not self._stop_event.is_set():
                    try:
                        ok, frame = capture.read()
                    except Exception:
                        ok, frame = False, None

                    if self._stop_event.is_set():
                        break
                    if self._reconnect_event.is_set():
                        self.status_changed.emit(
                            f"Reconnecting {camera_name} (index {camera_index})..."
                        )
                        break
                    if ok and frame is not None and frame.size:
                        failed_since = None
                        if not received_frame:
                            received_frame = True
                            self.selected_capture_mode = active_capture_mode
                            if self.capture_mode == "auto_detect":
                                next_auto_mode = active_capture_mode
                                self.status_changed.emit(
                                    f"Auto Detect selected: "
                                    f"{self._capture_mode_name(active_capture_mode)}; "
                                    f"{actual_fourcc} {actual_width}x{actual_height} "
                                    f"{actual_fps:.2f} FPS"
                                )
                        now = time.perf_counter()
                        if now - last_frame_seen_emit >= 0.1:
                            self.frame_seen.emit(now)
                            last_frame_seen_emit = now
                        if self.frame_consumer is None:
                            self.frame_ready.emit(frame)
                        else:
                            self.frame_consumer(frame)
                        continue

                    now = time.perf_counter()
                    if failed_since is None:
                        failed_since = now
                    no_frame_timeout = self.reconnect_seconds
                    if self.capture_mode == "auto_detect" and not received_frame:
                        no_frame_timeout = UVC_AUTO_DETECT_FRAME_TIMEOUT_SECONDS
                    if now - failed_since >= no_frame_timeout:
                        if self.capture_mode == "auto_detect" and not received_frame:
                            next_auto_mode = self._alternate_auto_mode(
                                active_capture_mode
                            )
                        self.status_changed.emit(
                            f"No frame for {no_frame_timeout:.1f}s; "
                            f"reconnecting {camera_name} "
                            f"(index {camera_index})..."
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


def process_uvc_pipeline_frame(
    frame,
    config,
    clahe_cache=None,
    fieldscale_states=None,
):
    started = time.perf_counter()
    original_gray = uvc_to_gray(frame)
    preprocessing_details = []
    pipeline_input_max = 255

    first_enabled = next(
        (stage for stage in config["stages"] if stage.enabled), None
    )
    use_native_fieldscale = (
        first_enabled is not None
        and first_enabled.algorithm_key == "fieldscale"
        and original_gray.dtype.itemsize > 1
    )
    if use_native_fieldscale:
        pipeline_input = original_gray.copy()
        pipeline_input_max = config["raw_max"]
        preprocessing_details.append("Native DN input for first Fieldscale step")
    elif original_gray.dtype == np.uint8:
        pipeline_input = original_gray.copy()
    else:
        pipeline_input = raw_to_display_u8(original_gray, config["raw_max"])

    final_image, records = apply_algorithm_pipeline(
        pipeline_input,
        config["stages"],
        input_max=pipeline_input_max,
        clahe_cache=clahe_cache,
        fieldscale_states=fieldscale_states,
        fieldscale_temporal=True,
    )
    total_ms = (time.perf_counter() - started) * 1000.0
    original_display = (
        original_gray.copy()
        if original_gray.dtype == np.uint8
        else raw_to_display_u8(original_gray, config["raw_max"])
    )
    return {
        "revision": config["revision"],
        "original_source": original_gray.copy(),
        "original_display": original_display,
        "pipeline_input": pipeline_input,
        "pipeline_input_max": pipeline_input_max,
        "final_image": final_image,
        "records": records,
        "preprocessing_details": preprocessing_details,
        "total_ms": total_ms,
    }


class UvcProcessingThread(QThread):
    frame_processed = pyqtSignal(object)
    frame_failed = pyqtSignal(object)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._condition = threading.Condition()
        self._pending = None
        self._config = None
        self._config_revision = None
        self._stop_requested = False
        self._clahe_cache = {}
        self._fieldscale_states = {}

    def set_config(self, config):
        with self._condition:
            revision = None if config is None else config.get("revision")
            if revision != self._config_revision:
                self._clahe_cache.clear()
                self._fieldscale_states.clear()
                self._config_revision = revision
            self._config = config
            self._pending = None

    def submit_frame(self, frame):
        with self._condition:
            if self._stop_requested or self._config is None:
                return
            self._pending = (frame.copy(), self._config)
            self._condition.notify()

    def submit(self, frame, config):
        with self._condition:
            if self._stop_requested:
                return
            self._pending = (frame.copy(), config)
            self._condition.notify()

    def stop(self):
        with self._condition:
            self._stop_requested = True
            self._pending = None
            self._config = None
            self._condition.notify_all()

    def run(self):
        while True:
            with self._condition:
                while self._pending is None and not self._stop_requested:
                    self._condition.wait()
                if self._stop_requested:
                    return
                frame, config = self._pending
                self._pending = None
            try:
                payload = process_uvc_pipeline_frame(
                    frame,
                    config,
                    clahe_cache=self._clahe_cache,
                    fieldscale_states=self._fieldscale_states,
                )
            except Exception as exc:
                self.frame_failed.emit(
                    {"revision": config.get("revision"), "message": str(exc)}
                )
            else:
                self.frame_processed.emit(payload)


class UvcRecordingThread(QThread):
    """Encode synchronized Original/Final comparison frames off the GUI thread."""

    def __init__(
        self,
        video_path,
        fps,
        parent=None,
        left_label="Original UVC",
        right_label="Final Pipeline Output",
    ):
        super().__init__(parent)
        self.video_path = Path(video_path)
        self.fps = max(1.0, float(fps))
        self.left_label = str(left_label)
        self.right_label = str(right_label)
        self._condition = threading.Condition()
        self._queue = deque()
        self._stop_requested = False
        self._stop_frame_time = None
        self._submitted_frames = 0
        self._queue_dropped_frames = 0
        self.result_summary = None
        self.error_message = None

    def submit(self, original, processed, frame_time=None):
        if original is None or processed is None:
            return False
        timestamp_value = (
            time.perf_counter() if frame_time is None else float(frame_time)
        )
        with self._condition:
            if self._stop_requested:
                return False
            if len(self._queue) >= UVC_RECORDING_QUEUE_SIZE:
                self._queue.popleft()
                self._queue_dropped_frames += 1
            self._queue.append(
                (original.copy(), processed.copy(), timestamp_value)
            )
            self._submitted_frames += 1
            self._condition.notify()
        return True

    def stop(self, frame_time=None):
        with self._condition:
            if not self._stop_requested:
                self._stop_frame_time = (
                    time.perf_counter()
                    if frame_time is None
                    else float(frame_time)
                )
            self._stop_requested = True
            self._condition.notify_all()

    @staticmethod
    def _display_bgr(image):
        array = np.asarray(image)
        if array.ndim != 2:
            raise ValueError("UVC recording requires 2D grayscale images.")
        if array.dtype == np.uint8:
            gray = np.ascontiguousarray(array)
        else:
            gray = np.clip(array, 0, 255).astype(np.uint8)
        return cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)

    @classmethod
    def _compose_frame(
        cls,
        original,
        processed,
        left_label="Original UVC",
        right_label="Final Pipeline Output",
    ):
        left = cls._display_bgr(original)
        right = cls._display_bgr(processed)
        if right.shape[:2] != left.shape[:2]:
            right = cv2.resize(
                right,
                (left.shape[1], left.shape[0]),
                interpolation=cv2.INTER_AREA,
            )
        canvas = np.ascontiguousarray(np.hstack((left, right)))
        if canvas.shape[0] % 2:
            canvas = cv2.copyMakeBorder(
                canvas, 0, 1, 0, 0, cv2.BORDER_CONSTANT, value=(0, 0, 0)
            )
        if canvas.shape[1] % 2:
            canvas = cv2.copyMakeBorder(
                canvas, 0, 0, 0, 1, cv2.BORDER_CONSTANT, value=(0, 0, 0)
            )

        panel_width = left.shape[1]
        cv2.rectangle(canvas, (0, 0), (panel_width - 1, 34), (0, 0, 0), -1)
        cv2.rectangle(
            canvas,
            (panel_width, 0),
            (canvas.shape[1] - 1, 34),
            (0, 0, 0),
            -1,
        )
        return cls._draw_comparison_labels(
            canvas, panel_width, left_label, right_label
        )

    @staticmethod
    def _draw_comparison_labels(canvas, panel_width, left_label, right_label):
        """Draw Unicode camera names into a BGR frame using Qt's system fonts."""
        bgr_format = getattr(QImage, "Format_BGR888", None)
        if bgr_format is not None:
            drawing_array = canvas
            image = QImage(
                drawing_array.data,
                drawing_array.shape[1],
                drawing_array.shape[0],
                drawing_array.strides[0],
                bgr_format,
            )
            convert_back = False
        else:
            drawing_array = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
            image = QImage(
                drawing_array.data,
                drawing_array.shape[1],
                drawing_array.shape[0],
                drawing_array.strides[0],
                QImage.Format_RGB888,
            )
            convert_back = True

        painter = QPainter(image)
        painter.setRenderHint(QPainter.TextAntialiasing, True)
        font = QFont()
        font.setPixelSize(18)
        font.setBold(True)
        painter.setFont(font)
        painter.setPen(QColor(255, 255, 255))
        painter.drawText(12, 24, str(left_label))
        painter.drawText(panel_width + 12, 24, str(right_label))
        painter.end()

        byte_count = (
            image.sizeInBytes() if hasattr(image, "sizeInBytes") else image.byteCount()
        )
        bits = image.bits()
        bits.setsize(byte_count)
        painted = np.frombuffer(bits, dtype=np.uint8).reshape(
            image.height(), image.bytesPerLine()
        )
        painted = painted[:, : image.width() * 3].reshape(
            image.height(), image.width(), 3
        ).copy()
        if convert_back:
            return cv2.cvtColor(painted, cv2.COLOR_RGB2BGR)
        return painted

    def run(self):
        writer = None
        first_frame_time = None
        last_frame_time = None
        last_canvas = None
        frame_size = None
        written_frames = 0
        duplicate_frames = 0
        timestamp_dropped_frames = 0

        try:
            if cv2 is None:
                raise RuntimeError(
                    "OpenCV not installed. pip install opencv-python"
                )
            while True:
                with self._condition:
                    while not self._queue and not self._stop_requested:
                        self._condition.wait()
                    if self._queue:
                        original, processed, frame_time = self._queue.popleft()
                    elif self._stop_requested:
                        break
                    else:
                        continue

                if first_frame_time is None:
                    first_frame_time = frame_time
                elapsed_seconds = max(0.0, frame_time - first_frame_time)
                canvas = self._compose_frame(
                    original,
                    processed,
                    self.left_label,
                    self.right_label,
                )
                current_size = (canvas.shape[1], canvas.shape[0])
                if writer is None:
                    frame_size = current_size
                    fourcc = cv2.VideoWriter_fourcc(*UVC_RECORDING_FOURCC)
                    writer = cv2.VideoWriter(
                        str(self.video_path), fourcc, self.fps, frame_size, True
                    )
                    if not writer.isOpened():
                        raise RuntimeError(
                            f"Cannot open UVC recording writer: {self.video_path}"
                        )
                elif current_size != frame_size:
                    raise RuntimeError(
                        "UVC recording frame size changed from "
                        f"{frame_size[0]}x{frame_size[1]} to "
                        f"{current_size[0]}x{current_size[1]}"
                    )

                target_index = int(round(elapsed_seconds * self.fps))
                if target_index < written_frames:
                    timestamp_dropped_frames += 1
                    last_frame_time = frame_time
                    continue
                while written_frames < target_index and last_canvas is not None:
                    writer.write(last_canvas)
                    written_frames += 1
                    duplicate_frames += 1
                writer.write(canvas)
                written_frames += 1
                last_canvas = canvas
                last_frame_time = frame_time

            if (
                writer is not None
                and last_canvas is not None
                and first_frame_time is not None
                and self._stop_frame_time is not None
            ):
                stop_elapsed = max(
                    0.0, self._stop_frame_time - first_frame_time
                )
                target_count = int(round(stop_elapsed * self.fps)) + 1
                while written_frames < target_count:
                    writer.write(last_canvas)
                    written_frames += 1
                    duplicate_frames += 1
        except Exception as exc:
            self.error_message = str(exc)
        finally:
            if writer is not None:
                writer.release()
            duration_seconds = 0.0
            if first_frame_time is not None:
                end_time = self._stop_frame_time
                if end_time is None:
                    end_time = last_frame_time
                if end_time is not None:
                    duration_seconds = max(0.0, end_time - first_frame_time)
            file_size = (
                self.video_path.stat().st_size
                if self.video_path.is_file()
                else 0
            )
            self.result_summary = {
                "codec": UVC_RECORDING_FOURCC,
                "fps": round(self.fps, 3),
                "frame_size": (
                    None
                    if frame_size is None
                    else {"width": frame_size[0], "height": frame_size[1]}
                ),
                "duration_seconds": round(duration_seconds, 3),
                "submitted_frames": self._submitted_frames,
                "written_frames": written_frames,
                "duplicate_frames": duplicate_frames,
                "queue_dropped_frames": self._queue_dropped_frames,
                "timestamp_dropped_frames": timestamp_dropped_frames,
                "file_size_bytes": int(file_size),
            }


class ThermalRawToolkit(QMainWindow):
    FUNCTIONS = [
        "RAW Viewer",
        "Average RAW Folder",
        "NUC Calculate",
        "NUC Apply",
        "High / Low Analyzer",
        "Temporal Drift Analyzer",
        "BPR Bad Pixel Marking",
        "Dual UVC Compare",
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
        "讀取單張 RAW，選擇輸入映射，再依自訂數量與順序執行影像演算法 Pipeline。",
        "開啟 UVC Camera：預設 Auto Detect 先嘗試 Novatek MJPG，失敗自動改用相機原生格式，再依自訂順序即時執行 Pipeline。",
    ]
    PREVIEW_LAYOUTS = [
        (False, "", "RAW Preview (Fixed Scale)"),
        (False, "", "Average RAW (Fixed Scale)"),
        (True, "HIGH Input (Fixed Scale)", "NUC Corrected HIGH (Fixed Scale)"),
        (True, "Original RAW (Fixed Scale)", "NUC Corrected RAW (Fixed Scale)"),
        (False, "", "HIGH - LOW Difference (Fixed Scale)"),
        (False, "", "Temporal Standard Deviation (Fixed Scale)"),
        (True, "RAW Fixed-Scale / White Canvas", "BPR Bad Pixels Marked"),
        (True, "UVC Camera A", "UVC Camera B"),
        (True, "Original RAW (Fixed Scale)", "Final Pipeline Output"),
        (True, "Original UVC", "Final Pipeline Output"),
    ]
    FUNCTION_DESCRIPTIONS.insert(
        6,
        "Load a Novatek BPR dpc_table.bin or x,y coordinate file. Optionally overlay bad-pixel circles on a RAW image; otherwise use a white canvas.",
    )
    FUNCTION_DESCRIPTIONS.insert(
        7,
        "Open two independent UVC cameras with per-camera Auto Detect for side-by-side monitoring, paired capture, and synchronized comparison recording.",
    )

    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(1450, 900)
        self.current_original = None
        self.current_result = None
        self.last_output_dir = None
        self.camera_devices = []
        self._camera_device_signature = None
        self.raw_algorithm_source = None
        self.raw_pipeline_input = None
        self.raw_pipeline_input_max = 255
        self.raw_pipeline_final = None
        self.raw_pipeline_records = []
        self.dual_uvc_worker_a = None
        self.dual_uvc_worker_b = None
        self.dual_uvc_frame_a = None
        self.dual_uvc_frame_b = None
        self.dual_uvc_frame_time_a = 0.0
        self.dual_uvc_frame_time_b = 0.0
        self.dual_uvc_actual_fps_a = 0.0
        self.dual_uvc_actual_fps_b = 0.0
        self.dual_uvc_output_fps_a = 0.0
        self.dual_uvc_output_fps_b = 0.0
        self.dual_uvc_frame_count_a = 0
        self.dual_uvc_frame_count_b = 0
        self.dual_uvc_fps_started_a = 0.0
        self.dual_uvc_fps_started_b = 0.0
        self.dual_uvc_recording_worker = None
        self.dual_uvc_recording_output_dir = None
        self.dual_uvc_recording_manifest = None
        self.dual_uvc_recording_stop_reason = None
        self.dual_uvc_recording_started_perf = 0.0
        self.dual_uvc_restart_when_stopped = False
        self.dual_uvc_close_pending = False
        self.dual_uvc_buffer_a = LatestFrameBuffer()
        self.dual_uvc_buffer_b = LatestFrameBuffer()
        self.dual_uvc_buffer_sequence_a = 0
        self.dual_uvc_buffer_sequence_b = 0
        self.dual_uvc_display_timer = QTimer(self)
        self.dual_uvc_display_timer.setInterval(10)
        self.dual_uvc_display_timer.timeout.connect(
            self.consume_dual_uvc_latest_frames
        )
        self.uvc_worker = None
        self.uvc_processing_worker = None
        self.uvc_recording_worker = None
        self.uvc_recording_output_dir = None
        self.uvc_recording_manifest = None
        self.uvc_recording_stop_reason = None
        self.uvc_recording_started_perf = 0.0
        self.uvc_actual_fps = 0.0
        self.uvc_original_source = None
        self.uvc_original_frame = None
        self.uvc_processed_frame = None
        self.uvc_pipeline_input = None
        self.uvc_pipeline_input_max = 255
        self.uvc_pipeline_records = []
        self.uvc_pipeline_total_ms = 0.0
        self.uvc_processing_revision = 0
        self.uvc_last_processing_error = None
        self.uvc_frame_count = 0
        self.uvc_fps_display = 0.0
        self.uvc_last_time = 0.0
        self.uvc_last_frame_time = 0.0
        self.uvc_waiting_for_frame = False
        self.uvc_reconnect_requested = False
        self.uvc_restart_when_stopped = False
        self.uvc_close_pending = False
        self.uvc_watchdog_timer = QTimer(self)
        self.uvc_watchdog_timer.setInterval(200)
        self.uvc_watchdog_timer.timeout.connect(self.check_uvc_stream)
        self._build_ui()
        self._apply_ui_style()
        self.refresh_camera_devices(log_changes=False)
        self.camera_refresh_timer = QTimer(self)
        self.camera_refresh_timer.setInterval(CAMERA_REFRESH_INTERVAL_MS)
        self.camera_refresh_timer.timeout.connect(self.refresh_camera_devices)
        self.camera_refresh_timer.start()

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
            QGroupBox#pipelineParameterCard {
                background-color: #202328;
                border: 1px solid #424850;
                border-radius: 7px;
                margin-top: 10px;
                padding: 10px 8px 8px 8px;
                color: #c9e2f2;
                font-weight: 700;
            }
            QGroupBox#pipelineParameterCard::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px;
                color: #c9e2f2;
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
            QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QTextEdit, QListWidget {
                background-color: #191c21;
                color: #f0f1f3;
                border: 1px solid #454b54;
                border-radius: 6px;
                padding: 6px 8px;
                selection-background-color: #4f8fbd;
            }
            QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,
            QTextEdit:focus, QListWidget:focus {
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
            QListWidget::item {
                color: #e7e9ed;
                padding: 4px 5px;
                border-radius: 3px;
            }
            QListWidget::item:selected {
                background-color: #4f8fbd;
                color: #ffffff;
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
            QScrollArea#formScroll,
            QWidget#formViewport,
            QWidget#formContent {
                background-color: #22252a;
                color: #e7e9ed;
                border: none;
            }
            QScrollBar:vertical {
                background-color: #191c21;
                width: 12px;
                margin: 0;
                border: none;
            }
            QScrollBar::handle:vertical {
                background-color: #4a515b;
                min-height: 28px;
                border-radius: 5px;
            }
            QScrollBar:horizontal {
                background-color: #191c21;
                height: 12px;
                margin: 0;
                border: none;
            }
            QScrollBar::handle:horizontal {
                background-color: #4a515b;
                min-width: 28px;
                border-radius: 5px;
            }
            QScrollBar::handle:hover {
                background-color: #626c78;
            }
            QScrollBar::add-line, QScrollBar::sub-line {
                width: 0;
                height: 0;
                border: none;
                background: transparent;
            }
            QScrollBar::add-page, QScrollBar::sub-page {
                background: transparent;
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
        uvc_function_names = (
            "Dual UVC Compare",
            "UVC Algorithm Compare",
        )
        function_menu_order = [
            function_index
            for function_index, function_name in enumerate(self.FUNCTIONS)
            if function_name not in uvc_function_names
        ]
        function_menu_order.extend(
            self.FUNCTIONS.index(function_name)
            for function_name in uvc_function_names
        )
        for function_index in function_menu_order:
            self.function_combo.addItem(
                self.FUNCTIONS[function_index], function_index
            )
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
        page = QWidget()
        page_layout = QVBoxLayout(page)
        page_layout.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea()
        scroll.setObjectName("formScroll")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        scroll.viewport().setObjectName("formViewport")
        scroll.viewport().setAutoFillBackground(True)
        content = QWidget()
        content.setObjectName("formContent")
        content.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        content_layout = QVBoxLayout(content)
        form = QFormLayout()
        form.setRowWrapPolicy(QFormLayout.WrapLongRows)
        content_layout.addLayout(form)
        content_layout.addStretch()
        scroll.setWidget(content)
        page_layout.addWidget(scroll)
        return page, form

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

        # Dual UVC Compare
        p, f = self._form_page()
        self.dual_uvc_note = QLabel(
            "Camera A and Camera B run in independent capture threads. Capture "
            "and recording pair the latest available frames in software; this is "
            "not hardware-trigger synchronization. Camera indexes must differ."
        )
        self.dual_uvc_note.setWordWrap(True)
        self.dual_uvc_note.setStyleSheet(
            "color:#c8d0d9; background:#292d33; border-left:3px solid #79afd3; "
            "border-radius:5px; padding:8px;"
        )
        f.addRow(self.dual_uvc_note)

        self.dual_uvc_group_a = QGroupBox("UVC Camera A")
        self.dual_uvc_group_a.setObjectName("settingsCard")
        dual_form_a = QFormLayout(self.dual_uvc_group_a)
        self.dual_uvc_image_name_a = QComboBox()
        self.dual_uvc_image_name_b = QComboBox()
        for image_name, file_tag in DUAL_UVC_IMAGE_NAME_OPTIONS:
            self.dual_uvc_image_name_a.addItem(image_name, file_tag)
            self.dual_uvc_image_name_b.addItem(image_name, file_tag)
        self.dual_uvc_image_name_a.setCurrentIndex(0)
        self.dual_uvc_image_name_b.setCurrentIndex(1)
        self.dual_uvc_camera_a = QComboBox()
        self.dual_uvc_camera_b = QComboBox()
        self.dual_uvc_fps_a = QComboBox()
        self.dual_uvc_fps_b = QComboBox()
        for fps in (30, 60):
            self.dual_uvc_fps_a.addItem(f"{fps} FPS", fps)
            self.dual_uvc_fps_b.addItem(f"{fps} FPS", fps)
        self.dual_uvc_fps_a.setCurrentIndex(
            max(self.dual_uvc_fps_a.findData(DEFAULT_UVC_FPS), 0)
        )
        self.dual_uvc_fps_b.setCurrentIndex(
            max(self.dual_uvc_fps_b.findData(DEFAULT_UVC_FPS), 0)
        )
        self.dual_uvc_status_a = QLabel("Camera A stopped")
        self.dual_uvc_status_a.setWordWrap(True)
        dual_form_a.addRow("Image Name", self.dual_uvc_image_name_a)
        dual_form_a.addRow("CAMERA_INDEX", self.dual_uvc_camera_a)
        dual_form_a.addRow("Requested FPS", self.dual_uvc_fps_a)
        dual_form_a.addRow("Status", self.dual_uvc_status_a)

        self.dual_uvc_group_b = QGroupBox("UVC Camera B")
        self.dual_uvc_group_b.setObjectName("settingsCard")
        dual_form_b = QFormLayout(self.dual_uvc_group_b)
        self.dual_uvc_status_b = QLabel("Camera B stopped")
        self.dual_uvc_status_b.setWordWrap(True)
        dual_form_b.addRow("Image Name", self.dual_uvc_image_name_b)
        dual_form_b.addRow("CAMERA_INDEX", self.dual_uvc_camera_b)
        dual_form_b.addRow("Requested FPS", self.dual_uvc_fps_b)
        dual_form_b.addRow("Status", self.dual_uvc_status_b)
        f.addRow(self.dual_uvc_group_a)
        f.addRow(self.dual_uvc_group_b)

        dual_controls = QWidget()
        dual_controls_layout = QGridLayout(dual_controls)
        dual_controls_layout.setContentsMargins(0, 0, 0, 0)
        self.dual_uvc_start_btn = QPushButton("Start Cameras")
        self.dual_uvc_start_btn.setObjectName("startButton")
        self.dual_uvc_start_btn.clicked.connect(self.start_dual_uvc_cameras)
        self.dual_uvc_stop_btn = QPushButton("Stop Cameras")
        self.dual_uvc_stop_btn.setObjectName("stopButton")
        self.dual_uvc_stop_btn.clicked.connect(self.stop_dual_uvc_cameras)
        self.dual_uvc_stop_btn.setEnabled(False)
        self.dual_uvc_capture_btn = QPushButton("Capture Pair")
        self.dual_uvc_capture_btn.setObjectName("captureButton")
        self.dual_uvc_capture_btn.clicked.connect(self.capture_dual_uvc_pair)
        self.dual_uvc_capture_btn.setEnabled(False)
        self.dual_uvc_record_start_btn = QPushButton("Start Recording")
        self.dual_uvc_record_start_btn.setObjectName("startButton")
        self.dual_uvc_record_start_btn.clicked.connect(
            self.start_dual_uvc_recording
        )
        self.dual_uvc_record_start_btn.setEnabled(False)
        self.dual_uvc_record_stop_btn = QPushButton("Stop Recording")
        self.dual_uvc_record_stop_btn.setObjectName("stopButton")
        self.dual_uvc_record_stop_btn.clicked.connect(
            self.stop_dual_uvc_recording
        )
        self.dual_uvc_record_stop_btn.setEnabled(False)
        self.dual_uvc_refresh_btn = QPushButton("Refresh Cameras")
        self.dual_uvc_refresh_btn.clicked.connect(self.refresh_camera_devices)
        dual_controls_layout.addWidget(self.dual_uvc_start_btn, 0, 0)
        dual_controls_layout.addWidget(self.dual_uvc_stop_btn, 0, 1)
        dual_controls_layout.addWidget(self.dual_uvc_capture_btn, 1, 0, 1, 2)
        dual_controls_layout.addWidget(self.dual_uvc_record_start_btn, 2, 0)
        dual_controls_layout.addWidget(self.dual_uvc_record_stop_btn, 2, 1)
        dual_controls_layout.addWidget(self.dual_uvc_refresh_btn, 3, 0, 1, 2)
        f.addRow(dual_controls)
        self.stack.addWidget(p)

        self.dual_uvc_camera_a.currentIndexChanged.connect(
            self.dual_uvc_setting_changed
        )
        self.dual_uvc_camera_b.currentIndexChanged.connect(
            self.dual_uvc_setting_changed
        )
        self.dual_uvc_fps_a.currentIndexChanged.connect(
            self.dual_uvc_setting_changed
        )
        self.dual_uvc_fps_b.currentIndexChanged.connect(
            self.dual_uvc_setting_changed
        )
        self.dual_uvc_image_name_a.currentIndexChanged.connect(
            self.dual_uvc_image_name_changed
        )
        self.dual_uvc_image_name_b.currentIndexChanged.connect(
            self.dual_uvc_image_name_changed
        )
        self.resolution_combo.currentIndexChanged.connect(
            self.dual_uvc_setting_changed
        )
        self.dual_uvc_image_name_changed()

        # Single RAW Algorithm Compare
        p, f = self._form_page()
        self.raw_algorithm_file = PathRow(
            "file",
            "RAW Files (*.raw *.RAW *.bin *.BIN)",
            "Select RAW File for Algorithm Compare",
        )
        self.raw_input_mapping_combo = QComboBox()
        for mapping_name, mapping_key in RAW_INPUT_MAPPING_OPTIONS:
            self.raw_input_mapping_combo.addItem(mapping_name, mapping_key)
        self.raw_input_mapping_combo.setSizePolicy(
            QSizePolicy.Expanding, QSizePolicy.Fixed
        )
        self.raw_input_mapping_note = QLabel(
            "Input Mapping runs before the display pipeline. With Fixed Bit Depth "
            "and Fieldscale as the first enabled step, Fieldscale reads original RAW "
            "DN to preserve its firmware-matching behavior; following steps are 8-bit."
        )
        self.raw_input_mapping_note.setWordWrap(True)
        self.raw_input_mapping_note.setSizePolicy(
            QSizePolicy.Ignored, QSizePolicy.Preferred
        )
        self.raw_input_mapping_note.setStyleSheet("color:#c98;")
        self.raw_pipeline_editor = AlgorithmPipelineEditor(
            allow_step_algorithm_change=False
        )
        self.raw_preview_stage_combo = QComboBox()
        self.raw_preview_stage_combo.setSizePolicy(
            QSizePolicy.Expanding, QSizePolicy.Fixed
        )
        self.raw_save_intermediates = QCheckBox("Save Step PNGs")
        self.raw_save_intermediates.setToolTip(
            "Save one PNG after every enabled pipeline step"
        )
        self.raw_save_intermediates.setChecked(True)
        f.addRow("RAW File", self.raw_algorithm_file)
        f.addRow("Input Mapping", self.raw_input_mapping_combo)
        f.addRow(self.raw_input_mapping_note)
        f.addRow(self.raw_pipeline_editor)
        f.addRow("Right Preview", self.raw_preview_stage_combo)
        f.addRow("Intermediate Output", self.raw_save_intermediates)
        self.stack.addWidget(p)
        self.raw_algorithm_file.edit.textChanged.connect(
            self.invalidate_raw_pipeline_source
        )
        self.raw_input_mapping_combo.currentIndexChanged.connect(
            self.update_raw_pipeline_ui
        )
        self.raw_pipeline_editor.pipeline_changed.connect(
            self.update_raw_pipeline_ui
        )
        self.raw_preview_stage_combo.currentIndexChanged.connect(
            self.update_raw_stage_preview
        )
        self.resolution_combo.currentIndexChanged.connect(
            self.invalidate_raw_pipeline_source
        )
        self.bit_depth_combo.currentIndexChanged.connect(
            self.update_raw_pipeline_ui
        )
        self.update_raw_pipeline_ui()

        # UVC Algorithm Compare
        p, f = self._form_page()
        self.uvc_camera_combo = QComboBox()
        self.uvc_fps_combo = QComboBox()
        for fps in (30, 60):
            self.uvc_fps_combo.addItem(f"{fps} FPS", fps)
        default_fps_index = self.uvc_fps_combo.findData(DEFAULT_UVC_FPS)
        self.uvc_fps_combo.setCurrentIndex(max(default_fps_index, 0))
        self.uvc_pipeline_editor = AlgorithmPipelineEditor(
            allow_step_algorithm_change=False
        )
        self.uvc_preview_stage_combo = QComboBox()
        self.uvc_preview_stage_combo.setSizePolicy(
            QSizePolicy.Expanding, QSizePolicy.Fixed
        )
        self.uvc_save_intermediates = QCheckBox("Save Step PNGs")
        self.uvc_save_intermediates.setToolTip(
            "Save one PNG after every enabled pipeline step when capturing"
        )
        self.uvc_save_intermediates.setChecked(True)
        self.uvc_camera_combo.currentIndexChanged.connect(self.switch_uvc_camera)
        self.bit_depth_combo.currentIndexChanged.connect(
            self.mark_uvc_processing_config_changed
        )
        self.uvc_pipeline_editor.pipeline_changed.connect(
            self.update_uvc_pipeline_ui
        )
        self.uvc_preview_stage_combo.currentIndexChanged.connect(
            self.update_uvc_stage_preview
        )
        controls = QWidget()
        controls_layout = QGridLayout(controls)
        controls_layout.setContentsMargins(0, 0, 0, 0)
        self.uvc_start_btn = QPushButton("Start Camera")
        self.uvc_start_btn.setObjectName("startButton")
        self.uvc_start_btn.clicked.connect(self.start_uvc_camera)
        self.uvc_stop_btn = QPushButton("Stop Camera")
        self.uvc_stop_btn.setObjectName("stopButton")
        self.uvc_stop_btn.clicked.connect(self.stop_uvc_camera)
        self.uvc_stop_btn.setEnabled(False)
        self.uvc_capture_btn = QPushButton("Capture Pair")
        self.uvc_capture_btn.setObjectName("captureButton")
        self.uvc_capture_btn.clicked.connect(self.capture_uvc_pair)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_record_start_btn = QPushButton("Start Recording")
        self.uvc_record_start_btn.setObjectName("startButton")
        self.uvc_record_start_btn.setToolTip(
            "Record synchronized Original UVC and Final Pipeline Output "
            "side by side as MJPG AVI"
        )
        self.uvc_record_start_btn.clicked.connect(self.start_uvc_recording)
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn = QPushButton("Stop Recording")
        self.uvc_record_stop_btn.setObjectName("stopButton")
        self.uvc_record_stop_btn.clicked.connect(self.stop_uvc_recording)
        self.uvc_record_stop_btn.setEnabled(False)
        self.uvc_refresh_btn = QPushButton("Refresh Cameras")
        self.uvc_refresh_btn.clicked.connect(self.refresh_camera_devices)
        controls_layout.addWidget(self.uvc_start_btn, 0, 0)
        controls_layout.addWidget(self.uvc_stop_btn, 0, 1)
        controls_layout.addWidget(self.uvc_capture_btn, 1, 0, 1, 2)
        controls_layout.addWidget(self.uvc_record_start_btn, 2, 0)
        controls_layout.addWidget(self.uvc_record_stop_btn, 2, 1)
        controls_layout.addWidget(self.uvc_refresh_btn, 3, 0, 1, 2)
        self.uvc_status = QLabel("Camera stopped")
        self.uvc_status.setWordWrap(True)
        self.uvc_status.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        f.addRow("CAMERA_INDEX", self.uvc_camera_combo)
        f.addRow("Requested FPS", self.uvc_fps_combo)
        f.addRow(self.uvc_pipeline_editor)
        f.addRow("Right Preview", self.uvc_preview_stage_combo)
        f.addRow("Intermediate Output", self.uvc_save_intermediates)
        f.addRow(controls)
        f.addRow("Status", self.uvc_status)
        self.stack.addWidget(p)
        self.update_uvc_pipeline_ui()
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

    def rebuild_pipeline_preview_combo(self, editor, combo):
        previous_data = combo.currentData()
        combo.blockSignals(True)
        combo.clear()
        combo.addItem("Final Pipeline Output", None)
        for pipeline_index, stage in enumerate(editor.stages(), start=1):
            if stage.enabled:
                combo.addItem(
                    f"After Step {pipeline_index}: "
                    f"{algorithm_display_name(stage.algorithm_key)}",
                    pipeline_index,
                )
        target_index = combo.findData(previous_data)
        combo.setCurrentIndex(target_index if target_index >= 0 else 0)
        combo.blockSignals(False)

    def invalidate_raw_pipeline_source(self, _value=None):
        self.raw_algorithm_source = None
        self.raw_pipeline_input = None
        self.raw_pipeline_input_max = 255
        self.raw_pipeline_final = None
        self.raw_pipeline_records = []
        if self.current_function_index() == len(self.FUNCTIONS) - 2:
            self.current_original = None
            self.current_result = None
            self.preview_original.set_array(None)
            self.preview_result.set_array(None)

    def prepare_raw_pipeline_input(self, raw):
        mapping_key = self.raw_input_mapping_combo.currentData()
        if mapping_key == "raw_percentile_preview":
            mapped, p1, p99 = percentile_preview_u8(raw)
            return (
                mapped,
                255,
                f"Original RAW DN P1={p1:.1f}, P99={p99:.1f} -> 8-bit",
            )

        first_enabled = next(
            (
                stage
                for stage in self.raw_pipeline_editor.stages()
                if stage.enabled
            ),
            None,
        )
        if first_enabled is not None and first_enabled.algorithm_key == "fieldscale":
            return (
                raw.copy(),
                self.raw_max,
                f"Original RAW DN 0..{self.raw_max} -> first Fieldscale step",
            )
        return (
            raw_to_display_u8(raw, self.raw_max),
            255,
            f"Fixed 0..{self.raw_max} DN -> 0..255",
        )

    def update_raw_pipeline_ui(self, _value=None):
        self.rebuild_pipeline_preview_combo(
            self.raw_pipeline_editor, self.raw_preview_stage_combo
        )
        if self.current_function_index() == len(self.FUNCTIONS) - 2:
            self.run_btn.setEnabled(bool(self.raw_pipeline_editor.enabled_stages()))
        self.refresh_raw_pipeline_preview()
        self.update_raw_stage_preview()

    def refresh_raw_pipeline_preview(self):
        if self.raw_algorithm_source is None:
            return
        try:
            pipeline_input, input_max, _mapping_details = (
                self.prepare_raw_pipeline_input(self.raw_algorithm_source)
            )
            final_image, records = apply_algorithm_pipeline(
                pipeline_input,
                self.raw_pipeline_editor.stages(),
                input_max=input_max,
            )
        except Exception as exc:
            self.raw_pipeline_input = None
            self.raw_pipeline_input_max = 255
            self.raw_pipeline_final = None
            self.raw_pipeline_records = []
            self.current_result = None
            if self.current_function_index() == len(self.FUNCTIONS) - 2:
                self.preview_result.set_array(None)
            self.log.append(f"RAW pipeline preview error: {exc}")
            return
        self.raw_pipeline_input = pipeline_input
        self.raw_pipeline_input_max = input_max
        self.raw_pipeline_final = final_image
        self.raw_pipeline_records = records
        self.update_raw_stage_preview()

    def update_raw_stage_preview(self, _combo_index=None):
        if self.raw_pipeline_final is None:
            return
        selected_pipeline_index = self.raw_preview_stage_combo.currentData()
        preview = pipeline_record_image(
            self.raw_pipeline_records,
            selected_pipeline_index,
            self.raw_pipeline_final,
        )
        self.current_result = preview
        if self.current_function_index() == len(self.FUNCTIONS) - 2:
            title = self.raw_preview_stage_combo.currentText()
            self.preview_result.set_title(title)
            self.preview_result_caption.setText(title)
            self.preview_result.set_array(preview)

    def current_function_index(self):
        function_index = self.function_combo.currentData()
        if function_index is None:
            return 0
        return int(function_index)

    def change_page(self, _combo_index):
        index = self.function_combo.currentData()
        if index is None:
            return
        index = int(index)
        uvc_page_index = len(self.FUNCTIONS) - 1
        raw_algorithm_page_index = len(self.FUNCTIONS) - 2
        dual_uvc_page_index = len(self.FUNCTIONS) - 3
        if index != uvc_page_index and (
            self.uvc_worker is not None or self.uvc_processing_worker is not None
        ):
            self.uvc_restart_when_stopped = False
            self.stop_uvc_camera(log_message=False)
        if index != dual_uvc_page_index and (
            self.dual_uvc_worker_a is not None
            or self.dual_uvc_worker_b is not None
        ):
            self.dual_uvc_restart_when_stopped = False
            self.stop_dual_uvc_cameras(log_message=False)
        self.stack.setCurrentIndex(index)
        self.function_title.setText(self.FUNCTIONS[index])
        self.function_description.setText(self.FUNCTION_DESCRIPTIONS[index])
        show_comparison, original_title, result_title = self.PREVIEW_LAYOUTS[index]
        if index == raw_algorithm_page_index:
            result_title = self.raw_preview_stage_combo.currentText()
        elif index == uvc_page_index:
            result_title = self.uvc_result_title()
        elif index == dual_uvc_page_index:
            original_title, result_title = self.dual_uvc_image_names()
        self.preview_original_container.setVisible(show_comparison)
        self.preview_original.setVisible(show_comparison)
        self.preview_original.set_title(original_title)
        self.preview_original_caption.setText(original_title)
        self.preview_result.set_title(result_title)
        self.preview_result_caption.setText(result_title)
        self.preview_original.set_array(None)
        self.preview_result.set_array(None)
        self.run_btn.setVisible(
            index not in (dual_uvc_page_index, uvc_page_index)
        )
        self.run_btn.setEnabled(
            index != raw_algorithm_page_index
            or bool(self.raw_pipeline_editor.enabled_stages())
        )
        self.save_current_btn.setVisible(
            index not in (3, dual_uvc_page_index)
        )
        self.log.clear()
        self.log.append(f"Selected: {self.FUNCTIONS[index]}")
    def auto_preview_raw(self, _path=None):
        if self.current_function_index() == 0:
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
            idx = self.current_function_index()
            handlers = [
                self.run_viewer,
                self.run_average,
                self.run_nuc_calculate,
                self.run_nuc_apply,
                self.run_high_low,
                self.run_drift,
                self.run_bpr_marking,
                self.start_dual_uvc_cameras,
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
        stages = self.raw_pipeline_editor.stages()
        pipeline_input, input_max, mapping_details = (
            self.prepare_raw_pipeline_input(raw)
        )
        total_started = time.perf_counter()
        processed, records = apply_algorithm_pipeline(
            pipeline_input,
            stages,
            input_max=input_max,
        )
        total_ms = (time.perf_counter() - total_started) * 1000.0
        out = self.make_output("raw_algorithm_compare")
        original_path = out / "original_fixed.png"
        mapping_path = out / (
            "input_native_dn.png" if input_max > 255 else "input_mapped.png"
        )
        result_path = out / "final_pipeline.png"
        overview_path = out / "00_pipeline_overview.png"
        save_png(original_path, raw8)
        save_png(mapping_path, pipeline_input)
        if self.raw_save_intermediates.isChecked():
            for record in records:
                step_path = out / (
                    f"step_{record['pipeline_index']:02d}_"
                    f"{record['algorithm']}.png"
                )
                save_png(step_path, record["image"])
                record["output_file"] = step_path.name
        save_png(result_path, processed)
        mapping_label = (
            "P1/P99 RAW DN"
            if self.raw_input_mapping_combo.currentData() == "raw_percentile_preview"
            else "Native RAW DN"
            if input_max > 255
            else "Fixed Bit Depth"
        )
        overview_items = [
            ("00 Original Fixed", raw8, 255),
            (f"01 Pipeline Input - {mapping_label}", pipeline_input, input_max),
        ]
        for overview_index, record in enumerate(records, start=2):
            overview_items.append(
                (
                    f"{overview_index:02d} Step {record['pipeline_index']:02d} "
                    f"{record['name']}",
                    record["image"],
                    255,
                )
            )
        save_pipeline_overview_png(overview_path, overview_items)
        manifest = {
            "format_version": 1,
            "mode": "raw_algorithm_compare",
            "source": str(Path(self.raw_algorithm_file.text()).resolve()),
            "resolution": {"width": self.width, "height": self.height},
            "bit_depth": self.bit_depth,
            "input_mapping": {
                "key": self.raw_input_mapping_combo.currentData(),
                "name": self.raw_input_mapping_combo.currentText(),
                "details": mapping_details,
                "input_max": input_max,
                "output_file": mapping_path.name,
            },
            "pipeline": self.raw_pipeline_editor.configuration(),
            "execution": pipeline_execution_json(records),
            "total_ms": round(total_ms, 3),
            "original_output": original_path.name,
            "final_output": result_path.name,
            "overview_output": overview_path.name,
        }
        for execution, record in zip(manifest["execution"], records):
            if "output_file" in record:
                execution["output_file"] = record["output_file"]
        with open(out / "pipeline.json", "w", encoding="utf-8") as destination:
            json.dump(manifest, destination, ensure_ascii=False, indent=2)
        with open(out / "pipeline_summary.txt", "w", encoding="utf-8") as summary:
            summary.write(f"Source: {manifest['source']}\n")
            summary.write(f"Input Mapping: {mapping_details}\n")
            summary.write(f"Pipeline: {self.raw_pipeline_editor.summary()}\n")
            summary.write(f"Overview: {overview_path.name}\n")
            for execution in manifest["execution"]:
                summary.write(
                    f"Step {execution['pipeline_index']}: {execution['name']} - "
                    f"{execution['elapsed_ms']:.3f} ms - "
                    f"{' | '.join(execution['details'])}\n"
                )
            summary.write(f"Total: {total_ms:.3f} ms\n")

        self.raw_algorithm_source = raw
        self.raw_pipeline_input = pipeline_input
        self.raw_pipeline_input_max = input_max
        self.raw_pipeline_final = processed
        self.raw_pipeline_records = records
        self.current_original = raw8
        self.preview_original.set_array(raw8)
        self.update_raw_stage_preview()
        self.log.append(f"Input Mapping: {mapping_details}")
        self.log.append(f"Pipeline: {self.raw_pipeline_editor.summary()}")
        for record in records:
            self.log.append(
                f"Step {record['pipeline_index']} {record['name']}: "
                f"{record['elapsed_ms']:.3f} ms | "
                f"{' | '.join(record['details'])}"
            )
        self.log.append(f"Pipeline total: {total_ms:.3f} ms")
        self.log_stats("Original 8-bit", raw8)
        self.log_stats("Final 8-bit", processed)
        self.log.append(f"Saved: {original_path}")
        self.log.append(f"Saved: {result_path}")
        self.log.append(f"Saved overview: {overview_path}")
        self.log.append(f"Pipeline settings: {out / 'pipeline.json'}")

    @staticmethod
    def camera_combo_device_id(combo):
        return combo.currentData(CAMERA_DEVICE_ID_ROLE)

    @staticmethod
    def camera_combo_friendly_name(combo):
        name = combo.currentData(CAMERA_FRIENDLY_NAME_ROLE)
        return str(name) if name else combo.currentText()

    @staticmethod
    def populate_camera_combo(
        combo,
        devices,
        default_index,
        preferred_device_id=None,
        preferred_name=None,
    ):
        combo.blockSignals(True)
        try:
            combo.clear()
            selected_item = -1
            for device in devices:
                camera_index = int(device["index"])
                friendly_name = str(device["name"])
                device_id = str(device["device_id"])
                combo.addItem(
                    f"{friendly_name} (Camera {camera_index})",
                    camera_index,
                )
                item_index = combo.count() - 1
                combo.setItemData(
                    item_index, device_id, CAMERA_DEVICE_ID_ROLE
                )
                combo.setItemData(
                    item_index, friendly_name, CAMERA_FRIENDLY_NAME_ROLE
                )
                if preferred_device_id and device_id == preferred_device_id:
                    selected_item = item_index

            if preferred_device_id and selected_item < 0:
                disconnected_name = preferred_name or "Previously selected camera"
                combo.addItem(f"Disconnected - {disconnected_name}", None)
                selected_item = combo.count() - 1
                combo.setItemData(
                    selected_item,
                    preferred_device_id,
                    CAMERA_DEVICE_ID_ROLE,
                )
                combo.setItemData(
                    selected_item,
                    disconnected_name,
                    CAMERA_FRIENDLY_NAME_ROLE,
                )

            if combo.count() == 0:
                combo.addItem("No camera detected", None)
                combo.setItemData(
                    0, "", CAMERA_DEVICE_ID_ROLE
                )
                combo.setItemData(
                    0, "No camera detected", CAMERA_FRIENDLY_NAME_ROLE
                )
                selected_item = 0
            elif selected_item < 0:
                selected_item = combo.findData(default_index)
                if selected_item < 0:
                    selected_item = 0
            combo.setCurrentIndex(selected_item)
        finally:
            combo.blockSignals(False)

    def selected_camera_info(self, combo):
        camera_index = combo.currentData()
        return {
            "index": None if camera_index is None else int(camera_index),
            "device_id": self.camera_combo_device_id(combo),
            "name": self.camera_combo_friendly_name(combo),
        }

    def refresh_camera_devices(self, _checked=False, log_changes=True):
        devices = enumerate_camera_devices()
        signature = tuple(
            (
                int(device["index"]),
                str(device["device_id"]),
                str(device["name"]),
            )
            for device in devices
        )
        if signature == self._camera_device_signature:
            return

        old_devices = {
            str(device["device_id"]): str(device["name"])
            for device in self.camera_devices
        }
        new_devices = {
            str(device["device_id"]): str(device["name"])
            for device in devices
        }
        selections = []
        for combo in (
            self.uvc_camera_combo,
            self.dual_uvc_camera_a,
            self.dual_uvc_camera_b,
        ):
            selections.append(
                (
                    self.camera_combo_device_id(combo),
                    self.camera_combo_friendly_name(combo),
                )
            )

        self.populate_camera_combo(
            self.uvc_camera_combo,
            devices,
            DEFAULT_CAMERA_INDEX,
            *selections[0],
        )
        self.populate_camera_combo(
            self.dual_uvc_camera_a,
            devices,
            DEFAULT_CAMERA_INDEX,
            *selections[1],
        )
        default_b_index = 0 if DEFAULT_CAMERA_INDEX != 0 else 1
        self.populate_camera_combo(
            self.dual_uvc_camera_b,
            devices,
            default_b_index,
            *selections[2],
        )

        self.camera_devices = devices
        self._camera_device_signature = signature
        devices_by_id = {
            str(device["device_id"]): device for device in devices
        }
        for worker in (
            self.uvc_worker,
            self.dual_uvc_worker_a,
            self.dual_uvc_worker_b,
        ):
            if worker is None or not worker.camera_device_id:
                continue
            device = devices_by_id.get(str(worker.camera_device_id))
            if device is None:
                worker.update_camera_device(None)
            else:
                worker.update_camera_device(
                    int(device["index"]), str(device["name"])
                )

        if log_changes and hasattr(self, "log"):
            added = [
                new_devices[device_id]
                for device_id in new_devices.keys() - old_devices.keys()
            ]
            removed = [
                old_devices[device_id]
                for device_id in old_devices.keys() - new_devices.keys()
            ]
            if added:
                self.log.append("Camera connected: " + ", ".join(added))
            if removed:
                self.log.append("Camera disconnected: " + ", ".join(removed))

    def dual_uvc_image_names(self):
        return (
            self.dual_uvc_image_name_a.currentText(),
            self.dual_uvc_image_name_b.currentText(),
        )

    def dual_uvc_image_file_tags(self):
        return (
            self.dual_uvc_image_name_a.currentData(),
            self.dual_uvc_image_name_b.currentData(),
        )

    def dual_uvc_image_name_changed(self, _combo_index=None):
        name_a, name_b = self.dual_uvc_image_names()
        self.dual_uvc_group_a.setTitle(f"UVC Camera A — {name_a}")
        self.dual_uvc_group_b.setTitle(f"UVC Camera B — {name_b}")
        if self.dual_uvc_worker_a is None:
            self.dual_uvc_status_a.setText(f"{name_a} (Camera A) stopped")
        if self.dual_uvc_worker_b is None:
            self.dual_uvc_status_b.setText(f"{name_b} (Camera B) stopped")
        dual_page_index = len(self.FUNCTIONS) - 3
        if self.current_function_index() == dual_page_index:
            self.preview_original.set_title(name_a)
            self.preview_result.set_title(name_b)
            self.preview_original_caption.setText(name_a)
            self.preview_result_caption.setText(name_b)

    def dual_uvc_setting_changed(self, _combo_index=None):
        if (
            self.dual_uvc_worker_a is not None
            or self.dual_uvc_worker_b is not None
        ):
            self.start_dual_uvc_cameras()

    def dual_uvc_frames_ready(self):
        return (
            self.dual_uvc_worker_a is not None
            and self.dual_uvc_worker_b is not None
            and self.dual_uvc_frame_a is not None
            and self.dual_uvc_frame_b is not None
        )

    def update_dual_uvc_button_states(self):
        running = (
            self.dual_uvc_worker_a is not None
            or self.dual_uvc_worker_b is not None
        )
        ready = self.dual_uvc_frames_ready()
        recording = self.dual_uvc_recording_worker is not None
        self.dual_uvc_image_name_a.setEnabled(not recording)
        self.dual_uvc_image_name_b.setEnabled(not recording)
        self.dual_uvc_start_btn.setEnabled(not running)
        self.dual_uvc_stop_btn.setEnabled(running)
        self.dual_uvc_capture_btn.setEnabled(ready)
        self.dual_uvc_record_start_btn.setEnabled(ready and not recording)
        self.dual_uvc_record_stop_btn.setEnabled(recording)

    def start_dual_uvc_cameras(self, _checked=False):
        if cv2 is None:
            message = "OpenCV not installed. pip install opencv-python"
            self.log.append(f"\nERROR: {message}")
            QMessageBox.critical(self, "Camera Error", message)
            return
        camera_info_a = self.selected_camera_info(self.dual_uvc_camera_a)
        camera_info_b = self.selected_camera_info(self.dual_uvc_camera_b)
        if camera_info_a["index"] is None or camera_info_b["index"] is None:
            QMessageBox.warning(
                self,
                "Camera Disconnected",
                "Select two connected cameras before starting Dual UVC.",
            )
            return
        camera_a = camera_info_a["index"]
        camera_b = camera_info_b["index"]
        if camera_a == camera_b:
            if (
                self.dual_uvc_worker_a is not None
                or self.dual_uvc_worker_b is not None
            ):
                self.stop_dual_uvc_cameras(log_message=False)
            QMessageBox.warning(
                self,
                "Duplicate Camera Index",
                "Camera A and Camera B must use different CAMERA_INDEX values.",
            )
            return

        if not self.stop_dual_uvc_cameras(log_message=False):
            self.dual_uvc_restart_when_stopped = True
            self.log.append(
                "Previous dual UVC capture threads are still stopping; "
                "both cameras will restart afterward."
            )
            return
        self.dual_uvc_restart_when_stopped = False

        worker_a = UvcCaptureThread(
            camera_a,
            self.width,
            self.height,
            int(self.dual_uvc_fps_a.currentData()),
            UVC_RECONNECT_SECONDS,
            self,
            frame_consumer=self.dual_uvc_buffer_a.submit,
            capture_mode=DEFAULT_UVC_CAPTURE_MODE,
            camera_device_id=camera_info_a["device_id"],
            camera_name=camera_info_a["name"],
        )
        worker_b = UvcCaptureThread(
            camera_b,
            self.width,
            self.height,
            int(self.dual_uvc_fps_b.currentData()),
            UVC_RECONNECT_SECONDS,
            self,
            frame_consumer=self.dual_uvc_buffer_b.submit,
            capture_mode=DEFAULT_UVC_CAPTURE_MODE,
            camera_device_id=camera_info_b["device_id"],
            camera_name=camera_info_b["name"],
        )
        worker_a.camera_opened.connect(self.handle_dual_uvc_camera_opened_a)
        worker_b.camera_opened.connect(self.handle_dual_uvc_camera_opened_b)
        worker_a.status_changed.connect(self.handle_dual_uvc_status_a)
        worker_b.status_changed.connect(self.handle_dual_uvc_status_b)
        worker_a.finished.connect(self.handle_dual_uvc_worker_finished)
        worker_b.finished.connect(self.handle_dual_uvc_worker_finished)
        worker_a.finished.connect(worker_a.deleteLater)
        worker_b.finished.connect(worker_b.deleteLater)
        self.dual_uvc_worker_a = worker_a
        self.dual_uvc_worker_b = worker_b
        self.dual_uvc_frame_a = None
        self.dual_uvc_frame_b = None
        self.dual_uvc_buffer_a.clear()
        self.dual_uvc_buffer_b.clear()
        self.dual_uvc_buffer_sequence_a = 0
        self.dual_uvc_buffer_sequence_b = 0
        self.dual_uvc_frame_time_a = 0.0
        self.dual_uvc_frame_time_b = 0.0
        self.dual_uvc_actual_fps_a = 0.0
        self.dual_uvc_actual_fps_b = 0.0
        self.dual_uvc_output_fps_a = 0.0
        self.dual_uvc_output_fps_b = 0.0
        self.dual_uvc_frame_count_a = 0
        self.dual_uvc_frame_count_b = 0
        now = time.perf_counter()
        self.dual_uvc_fps_started_a = now
        self.dual_uvc_fps_started_b = now
        name_a, name_b = self.dual_uvc_image_names()
        self.dual_uvc_status_a.setText(
            f"Opening {name_a} (Camera A, index {camera_a})..."
        )
        self.dual_uvc_status_b.setText(
            f"Opening {name_b} (Camera B, index {camera_b})..."
        )
        self.preview_original.set_array(None)
        self.preview_result.set_array(None)
        self.update_dual_uvc_button_states()
        self.dual_uvc_display_timer.start()
        self.log.append(
            f"Starting Dual UVC: A={name_a}, "
            f"{camera_info_a['name']}, index {camera_a} "
            f"({DEFAULT_UVC_CAPTURE_MODE}), B={name_b}, "
            f"{camera_info_b['name']}, index {camera_b} "
            f"({DEFAULT_UVC_CAPTURE_MODE})"
        )
        worker_a.start()
        worker_b.start()

    def stop_dual_uvc_cameras(
        self, _checked=False, log_message=True, wait_ms=2000
    ):
        self.dual_uvc_restart_when_stopped = False
        self.dual_uvc_display_timer.stop()
        self.stop_dual_uvc_recording(
            reason="Dual UVC cameras stopped", log_message=log_message
        )
        worker_a = self.dual_uvc_worker_a
        worker_b = self.dual_uvc_worker_b
        was_running = worker_a is not None or worker_b is not None
        for worker in (worker_a, worker_b):
            if worker is not None:
                worker.stop()

        all_stopped = True
        if worker_a is not None:
            if worker_a.wait(wait_ms):
                if self.dual_uvc_worker_a is worker_a:
                    self.dual_uvc_worker_a = None
            else:
                all_stopped = False
        if worker_b is not None:
            if worker_b.wait(wait_ms):
                if self.dual_uvc_worker_b is worker_b:
                    self.dual_uvc_worker_b = None
            else:
                all_stopped = False

        if not all_stopped:
            self.dual_uvc_start_btn.setEnabled(False)
            self.dual_uvc_stop_btn.setEnabled(False)
            self.dual_uvc_capture_btn.setEnabled(False)
            self.dual_uvc_record_start_btn.setEnabled(False)
            self.dual_uvc_record_stop_btn.setEnabled(False)
            self.log.append("Dual UVC capture threads are still stopping.")
            return False

        self.dual_uvc_actual_fps_a = 0.0
        self.dual_uvc_actual_fps_b = 0.0
        name_a, name_b = self.dual_uvc_image_names()
        self.dual_uvc_status_a.setText(f"{name_a} (Camera A) stopped")
        self.dual_uvc_status_b.setText(f"{name_b} (Camera B) stopped")
        self.update_dual_uvc_button_states()
        if was_running and log_message:
            self.log.append("Dual UVC cameras stopped")
        return True

    def handle_dual_uvc_camera_opened_a(
        self, camera_index, actual_width, actual_height, actual_fps
    ):
        if self.sender() is not self.dual_uvc_worker_a:
            return
        self.dual_uvc_actual_fps_a = float(actual_fps)
        name_a = self.dual_uvc_image_name_a.currentText()
        camera_name = getattr(
            self.dual_uvc_worker_a, "camera_name", f"Camera {camera_index}"
        )
        self.dual_uvc_status_a.setText(
            f"{name_a} (Camera A): {camera_name}, index {camera_index}, "
            f"{actual_width}x{actual_height}, "
            f"reported {actual_fps:.1f} FPS"
        )

    def handle_dual_uvc_camera_opened_b(
        self, camera_index, actual_width, actual_height, actual_fps
    ):
        if self.sender() is not self.dual_uvc_worker_b:
            return
        self.dual_uvc_actual_fps_b = float(actual_fps)
        name_b = self.dual_uvc_image_name_b.currentText()
        camera_name = getattr(
            self.dual_uvc_worker_b, "camera_name", f"Camera {camera_index}"
        )
        self.dual_uvc_status_b.setText(
            f"{name_b} (Camera B): {camera_name}, index {camera_index}, "
            f"{actual_width}x{actual_height}, "
            f"reported {actual_fps:.1f} FPS"
        )

    def handle_dual_uvc_status_a(self, message):
        if self.sender() is self.dual_uvc_worker_a:
            self.dual_uvc_status_a.setText(message)

    def handle_dual_uvc_status_b(self, message):
        if self.sender() is self.dual_uvc_worker_b:
            self.dual_uvc_status_b.setText(message)

    def consume_dual_uvc_latest_frames(self):
        updated = False
        frame_a, sequence_a, frame_time_a = self.dual_uvc_buffer_a.take_latest(
            self.dual_uvc_buffer_sequence_a
        )
        if frame_a is not None:
            self.dual_uvc_buffer_sequence_a = sequence_a
            self.handle_dual_uvc_frame(
                "a", frame_a, frame_time_a, submit_recording=False
            )
            updated = True
        frame_b, sequence_b, frame_time_b = self.dual_uvc_buffer_b.take_latest(
            self.dual_uvc_buffer_sequence_b
        )
        if frame_b is not None:
            self.dual_uvc_buffer_sequence_b = sequence_b
            self.handle_dual_uvc_frame(
                "b", frame_b, frame_time_b, submit_recording=False
            )
            updated = True
        if updated:
            self.submit_dual_uvc_recording_pair()

    def handle_dual_uvc_frame_a(self, frame):
        if self.sender() is not self.dual_uvc_worker_a:
            return
        self.handle_dual_uvc_frame("a", frame)

    def handle_dual_uvc_frame_b(self, frame):
        if self.sender() is not self.dual_uvc_worker_b:
            return
        self.handle_dual_uvc_frame("b", frame)

    def handle_dual_uvc_frame(
        self, camera_key, frame, frame_time=None, submit_recording=True
    ):
        gray = uvc_to_gray(frame)
        display = (
            gray.copy()
            if gray.dtype == np.uint8
            else raw_to_display_u8(gray, self.raw_max)
        )
        now = time.perf_counter() if frame_time is None else float(frame_time)
        if camera_key == "a":
            self.dual_uvc_frame_a = display
            self.dual_uvc_frame_time_a = now
            self.dual_uvc_frame_count_a += 1
            elapsed = now - self.dual_uvc_fps_started_a
            if elapsed >= 0.5:
                self.dual_uvc_output_fps_a = (
                    self.dual_uvc_frame_count_a / elapsed
                )
                self.dual_uvc_frame_count_a = 0
                self.dual_uvc_fps_started_a = now
            self.dual_uvc_status_a.setText(
                f"{self.dual_uvc_image_name_a.currentText()} "
                f"(Camera A) {display.shape[1]}x{display.shape[0]} | "
                f"Output FPS {self.dual_uvc_output_fps_a:.1f}"
            )
        else:
            self.dual_uvc_frame_b = display
            self.dual_uvc_frame_time_b = now
            self.dual_uvc_frame_count_b += 1
            elapsed = now - self.dual_uvc_fps_started_b
            if elapsed >= 0.5:
                self.dual_uvc_output_fps_b = (
                    self.dual_uvc_frame_count_b / elapsed
                )
                self.dual_uvc_frame_count_b = 0
                self.dual_uvc_fps_started_b = now
            self.dual_uvc_status_b.setText(
                f"{self.dual_uvc_image_name_b.currentText()} "
                f"(Camera B) {display.shape[1]}x{display.shape[0]} | "
                f"Output FPS {self.dual_uvc_output_fps_b:.1f}"
            )

        dual_page_index = len(self.FUNCTIONS) - 3
        if self.current_function_index() == dual_page_index:
            if self.dual_uvc_frame_a is not None:
                self.current_original = self.dual_uvc_frame_a
                self.preview_original.set_array(self.dual_uvc_frame_a)
            if self.dual_uvc_frame_b is not None:
                self.current_result = self.dual_uvc_frame_b
                self.preview_result.set_array(self.dual_uvc_frame_b)

        recording_worker = self.dual_uvc_recording_worker
        if (
            submit_recording
            and recording_worker is not None
            and recording_worker.isRunning()
            and self.dual_uvc_frame_a is not None
            and self.dual_uvc_frame_b is not None
        ):
            recording_worker.submit(
                self.dual_uvc_frame_a,
                self.dual_uvc_frame_b,
                time.perf_counter(),
            )
        self.update_dual_uvc_button_states()

    def submit_dual_uvc_recording_pair(self):
        recording_worker = self.dual_uvc_recording_worker
        if (
            recording_worker is not None
            and recording_worker.isRunning()
            and self.dual_uvc_frame_a is not None
            and self.dual_uvc_frame_b is not None
        ):
            recording_worker.submit(
                self.dual_uvc_frame_a,
                self.dual_uvc_frame_b,
                time.perf_counter(),
            )

    def handle_dual_uvc_worker_finished(self):
        worker = self.sender()
        if worker is self.dual_uvc_worker_a:
            self.dual_uvc_worker_a = None
            self.dual_uvc_status_a.setText(
                f"{self.dual_uvc_image_name_a.currentText()} (Camera A) stopped"
            )
        elif worker is self.dual_uvc_worker_b:
            self.dual_uvc_worker_b = None
            self.dual_uvc_status_b.setText(
                f"{self.dual_uvc_image_name_b.currentText()} (Camera B) stopped"
            )
        else:
            return
        self.stop_dual_uvc_recording(
            reason="A Dual UVC capture thread finished"
        )
        self.update_dual_uvc_button_states()
        if self.dual_uvc_close_pending and (
            self.dual_uvc_worker_a is None
            and self.dual_uvc_worker_b is None
        ):
            self.dual_uvc_display_timer.stop()
            self.dual_uvc_close_pending = False
            QTimer.singleShot(0, self.close)
        elif self.dual_uvc_restart_when_stopped and (
            self.dual_uvc_worker_a is None
            and self.dual_uvc_worker_b is None
        ):
            self.dual_uvc_display_timer.stop()
            self.dual_uvc_restart_when_stopped = False
            QTimer.singleShot(0, self.start_dual_uvc_cameras)

    def dual_uvc_labels(self):
        return self.dual_uvc_image_names()

    def save_dual_uvc_overview(self, path):
        if self.dual_uvc_frame_a is None or self.dual_uvc_frame_b is None:
            raise ValueError("Both Dual UVC frames are required for overview.")
        label_a, label_b = self.dual_uvc_labels()
        comparison_bgr = UvcRecordingThread._compose_frame(
            self.dual_uvc_frame_a,
            self.dual_uvc_frame_b,
            label_a,
            label_b,
        )
        save_rgb_png(
            path, cv2.cvtColor(comparison_bgr, cv2.COLOR_BGR2RGB)
        )

    def dual_uvc_camera_manifest(self):
        camera_info_a = self.selected_camera_info(self.dual_uvc_camera_a)
        camera_info_b = self.selected_camera_info(self.dual_uvc_camera_b)
        return {
            "camera_a": {
                "image_name": self.dual_uvc_image_name_a.currentText(),
                "image_name_file_tag": self.dual_uvc_image_name_a.currentData(),
                "camera_index": camera_info_a["index"],
                "camera_name": camera_info_a["name"],
                "camera_device_id": camera_info_a["device_id"],
                "capture_mode": DEFAULT_UVC_CAPTURE_MODE,
                "selected_capture_mode": getattr(
                    self.dual_uvc_worker_a, "selected_capture_mode", None
                ),
                "requested_fps": int(self.dual_uvc_fps_a.currentData()),
                "reported_fps": round(self.dual_uvc_actual_fps_a, 3),
                "resolution": {
                    "width": int(self.dual_uvc_frame_a.shape[1]),
                    "height": int(self.dual_uvc_frame_a.shape[0]),
                },
            },
            "camera_b": {
                "image_name": self.dual_uvc_image_name_b.currentText(),
                "image_name_file_tag": self.dual_uvc_image_name_b.currentData(),
                "camera_index": camera_info_b["index"],
                "camera_name": camera_info_b["name"],
                "camera_device_id": camera_info_b["device_id"],
                "capture_mode": DEFAULT_UVC_CAPTURE_MODE,
                "selected_capture_mode": getattr(
                    self.dual_uvc_worker_b, "selected_capture_mode", None
                ),
                "requested_fps": int(self.dual_uvc_fps_b.currentData()),
                "reported_fps": round(self.dual_uvc_actual_fps_b, 3),
                "resolution": {
                    "width": int(self.dual_uvc_frame_b.shape[1]),
                    "height": int(self.dual_uvc_frame_b.shape[0]),
                },
            },
            "software_pair_delta_ms": round(
                abs(
                    self.dual_uvc_frame_time_a
                    - self.dual_uvc_frame_time_b
                )
                * 1000.0,
                3,
            ),
        }

    def capture_dual_uvc_pair(self):
        if not self.dual_uvc_frames_ready():
            QMessageBox.information(
                self,
                "No Dual UVC Frames",
                "Start both cameras and wait for Camera A and Camera B frames.",
            )
            return
        out = self.make_output("dual_uvc_compare")
        file_tag_a, file_tag_b = self.dual_uvc_image_file_tags()
        camera_a_path = out / f"camera_a_{file_tag_a}.png"
        camera_b_path = out / f"camera_b_{file_tag_b}.png"
        overview_path = out / "00_dual_uvc_overview.png"
        save_png(camera_a_path, self.dual_uvc_frame_a)
        save_png(camera_b_path, self.dual_uvc_frame_b)
        self.save_dual_uvc_overview(overview_path)
        manifest = {
            "format_version": 1,
            "mode": "dual_uvc_compare",
            "captured_at": datetime.now().astimezone().isoformat(
                timespec="seconds"
            ),
            "pairing": "latest-frame software pairing; not hardware synchronized",
            **self.dual_uvc_camera_manifest(),
            "camera_a_output": camera_a_path.name,
            "camera_b_output": camera_b_path.name,
            "overview_output": overview_path.name,
        }
        with open(out / "capture.json", "w", encoding="utf-8") as destination:
            json.dump(manifest, destination, ensure_ascii=False, indent=2)
        with open(
            out / "capture_summary.txt", "w", encoding="utf-8"
        ) as summary:
            summary.write(f"Captured: {manifest['captured_at']}\n")
            summary.write(
                f"Camera A ({manifest['camera_a']['image_name']}): "
                f"{manifest['camera_a']['camera_name']}, "
                f"index {manifest['camera_a']['camera_index']}, "
                f"{manifest['camera_a']['resolution']['width']}x"
                f"{manifest['camera_a']['resolution']['height']}, "
                f"requested {manifest['camera_a']['capture_mode']}, "
                f"selected {manifest['camera_a']['selected_capture_mode']}\n"
            )
            summary.write(
                f"Camera B ({manifest['camera_b']['image_name']}): "
                f"{manifest['camera_b']['camera_name']}, "
                f"index {manifest['camera_b']['camera_index']}, "
                f"{manifest['camera_b']['resolution']['width']}x"
                f"{manifest['camera_b']['resolution']['height']}, "
                f"requested {manifest['camera_b']['capture_mode']}, "
                f"selected {manifest['camera_b']['selected_capture_mode']}\n"
            )
            summary.write(
                f"Software pair delta: "
                f"{manifest['software_pair_delta_ms']:.3f} ms\n"
            )
            summary.write(f"Overview: {overview_path.name}\n")
        self.log.append(f"Saved Dual UVC Camera A: {camera_a_path}")
        self.log.append(f"Saved Dual UVC Camera B: {camera_b_path}")
        self.log.append(f"Saved Dual UVC overview: {overview_path}")

    def start_dual_uvc_recording(self, _checked=False):
        if self.dual_uvc_recording_worker is not None:
            QMessageBox.information(
                self,
                "Recording Active",
                "Dual UVC recording is already active.",
            )
            return
        if not self.dual_uvc_frames_ready():
            QMessageBox.information(
                self,
                "No Dual UVC Frames",
                "Start both cameras and wait for Camera A and Camera B frames.",
            )
            return

        fps_a = (
            self.dual_uvc_actual_fps_a
            if 1.0 <= self.dual_uvc_actual_fps_a <= 240.0
            else float(self.dual_uvc_fps_a.currentData())
        )
        fps_b = (
            self.dual_uvc_actual_fps_b
            if 1.0 <= self.dual_uvc_actual_fps_b <= 240.0
            else float(self.dual_uvc_fps_b.currentData())
        )
        recording_fps = min(fps_a, fps_b)
        out = self.make_output("dual_uvc_recording")
        video_path = out / "dual_uvc_comparison.avi"
        overview_path = out / "00_dual_uvc_overview.png"
        self.save_dual_uvc_overview(overview_path)
        started_at = datetime.now().astimezone().isoformat(timespec="seconds")
        manifest = {
            "format_version": 1,
            "mode": "dual_uvc_recording",
            "started_at": started_at,
            "pairing": "latest-frame software pairing; not hardware synchronized",
            **self.dual_uvc_camera_manifest(),
            "recording_fps": round(recording_fps, 3),
            "recording_output": video_path.name,
            "overview_output": overview_path.name,
        }
        with open(out / "recording.json", "w", encoding="utf-8") as destination:
            json.dump(manifest, destination, ensure_ascii=False, indent=2)

        label_a, label_b = self.dual_uvc_labels()
        worker = UvcRecordingThread(
            video_path,
            recording_fps,
            self,
            left_label=label_a,
            right_label=label_b,
        )
        worker.finished.connect(self.handle_dual_uvc_recording_finished)
        worker.finished.connect(worker.deleteLater)
        self.dual_uvc_recording_worker = worker
        self.dual_uvc_recording_output_dir = out
        self.dual_uvc_recording_manifest = manifest
        self.dual_uvc_recording_stop_reason = None
        self.dual_uvc_recording_started_perf = time.perf_counter()
        self.update_dual_uvc_button_states()
        worker.start()
        worker.submit(
            self.dual_uvc_frame_a,
            self.dual_uvc_frame_b,
            self.dual_uvc_recording_started_perf,
        )
        self.log.append(
            f"Dual UVC recording started: {video_path} "
            f"({recording_fps:.3f} FPS, {UVC_RECORDING_FOURCC})"
        )

    def write_dual_uvc_recording_summary(self, worker, reason):
        out = self.dual_uvc_recording_output_dir
        manifest = self.dual_uvc_recording_manifest
        if out is None or manifest is None:
            return
        stats = worker.result_summary or {}
        manifest["ended_at"] = datetime.now().astimezone().isoformat(
            timespec="seconds"
        )
        manifest["stop_reason"] = reason
        manifest["success"] = worker.error_message is None and bool(
            stats.get("written_frames", 0)
        )
        manifest["error"] = worker.error_message
        manifest["recording"] = stats
        with open(out / "recording.json", "w", encoding="utf-8") as destination:
            json.dump(manifest, destination, ensure_ascii=False, indent=2)
        with open(
            out / "recording_summary.txt", "w", encoding="utf-8"
        ) as summary:
            summary.write(f"Started: {manifest['started_at']}\n")
            summary.write(f"Ended: {manifest['ended_at']}\n")
            summary.write(f"Stop reason: {reason}\n")
            summary.write(f"Success: {manifest['success']}\n")
            if worker.error_message:
                summary.write(f"Error: {worker.error_message}\n")
            summary.write(
                f"Camera A / B: {manifest['camera_a']['image_name']} "
                f"(index {manifest['camera_a']['camera_index']}) / "
                f"{manifest['camera_b']['image_name']} "
                f"(index {manifest['camera_b']['camera_index']})\n"
            )
            summary.write(
                f"Recording FPS: {manifest['recording_fps']:.3f}\n"
            )
            summary.write(
                f"Duration: {stats.get('duration_seconds', 0.0):.3f} s\n"
            )
            summary.write(
                f"Submitted / written / duplicate frames: "
                f"{stats.get('submitted_frames', 0)} / "
                f"{stats.get('written_frames', 0)} / "
                f"{stats.get('duplicate_frames', 0)}\n"
            )
            summary.write(
                f"Queue / timestamp dropped frames: "
                f"{stats.get('queue_dropped_frames', 0)} / "
                f"{stats.get('timestamp_dropped_frames', 0)}\n"
            )
            summary.write(
                f"File size: {stats.get('file_size_bytes', 0)} bytes\n"
            )
            summary.write(f"Overview: {manifest['overview_output']}\n")

    def finalize_dual_uvc_recording_worker(self, worker, reason):
        if worker is not self.dual_uvc_recording_worker:
            return
        try:
            self.write_dual_uvc_recording_summary(worker, reason)
        except Exception as exc:
            self.log.append(f"Dual UVC recording summary error: {exc}")
        out = self.dual_uvc_recording_output_dir
        video_path = None if out is None else out / "dual_uvc_comparison.avi"
        if worker.error_message:
            self.log.append(f"Dual UVC recording error: {worker.error_message}")
        else:
            stats = worker.result_summary or {}
            self.log.append(
                f"Dual UVC recording stopped: {video_path}; "
                f"written={stats.get('written_frames', 0)}, "
                f"duplicates={stats.get('duplicate_frames', 0)}, "
                f"dropped={stats.get('queue_dropped_frames', 0) + stats.get('timestamp_dropped_frames', 0)}"
            )
        self.dual_uvc_recording_worker = None
        self.dual_uvc_recording_output_dir = None
        self.dual_uvc_recording_manifest = None
        self.dual_uvc_recording_stop_reason = None
        self.dual_uvc_recording_started_perf = 0.0
        self.update_dual_uvc_button_states()

    def stop_dual_uvc_recording(
        self,
        _checked=False,
        reason=None,
        wait_ms=5000,
        log_message=True,
    ):
        worker = self.dual_uvc_recording_worker
        if worker is None:
            return True
        stop_reason = reason or "User requested"
        self.dual_uvc_recording_stop_reason = stop_reason
        worker.stop()
        self.dual_uvc_record_start_btn.setEnabled(False)
        self.dual_uvc_record_stop_btn.setEnabled(False)
        if not worker.wait(wait_ms):
            if log_message:
                self.log.append(
                    "Dual UVC recording thread is still finishing the AVI file."
                )
            return False
        self.finalize_dual_uvc_recording_worker(worker, stop_reason)
        return True

    def handle_dual_uvc_recording_finished(self):
        worker = self.sender()
        if worker is not self.dual_uvc_recording_worker:
            return
        reason = self.dual_uvc_recording_stop_reason
        if reason is None:
            reason = (
                "Recording error"
                if worker.error_message
                else "Recording worker finished"
            )
        self.finalize_dual_uvc_recording_worker(worker, reason)

    def uvc_result_title(self):
        return self.uvc_preview_stage_combo.currentText()

    def mark_uvc_processing_config_changed(self, _value=None):
        if self.uvc_recording_worker is not None:
            self.stop_uvc_recording(
                reason="Pipeline configuration changed"
            )
        self.uvc_processing_revision += 1
        if hasattr(self, "uvc_capture_btn"):
            self.uvc_capture_btn.setEnabled(False)
        worker = self.uvc_processing_worker
        if worker is not None and worker.isRunning():
            try:
                config = self.build_uvc_processing_config()
            except Exception as exc:
                worker.set_config(None)
                self.uvc_status.setText(f"Pipeline configuration error: {exc}")
            else:
                worker.set_config(config)

    def update_uvc_pipeline_ui(self, _value=None):
        self.rebuild_pipeline_preview_combo(
            self.uvc_pipeline_editor, self.uvc_preview_stage_combo
        )
        self.mark_uvc_processing_config_changed()
        self.update_uvc_result_title()
        self.refresh_uvc_processed_preview()

    def update_uvc_result_title(self):
        if self.current_function_index() == len(self.FUNCTIONS) - 1:
            title = self.uvc_result_title()
            self.preview_result.set_title(title)
            self.preview_result_caption.setText(title)

    def build_uvc_processing_config(self):
        stages = self.uvc_pipeline_editor.stages()
        if not any(stage.enabled for stage in stages):
            raise ValueError("Add and enable at least one UVC algorithm step.")
        return {
            "revision": self.uvc_processing_revision,
            "capture_mode": DEFAULT_UVC_CAPTURE_MODE,
            "raw_max": self.raw_max,
            "stages": stages,
        }

    def refresh_uvc_processed_preview(self):
        worker = self.uvc_processing_worker
        if (
            self.uvc_original_source is None
            or worker is None
            or not worker.isRunning()
        ):
            return
        try:
            config = self.build_uvc_processing_config()
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
        worker.submit(self.uvc_original_source, config)

    def update_uvc_stage_preview(self, _combo_index=None):
        if self.uvc_processed_frame is None:
            self.update_uvc_result_title()
            return
        selected_pipeline_index = self.uvc_preview_stage_combo.currentData()
        preview = pipeline_record_image(
            self.uvc_pipeline_records,
            selected_pipeline_index,
            self.uvc_processed_frame,
        )
        self.current_result = preview
        if self.current_function_index() == len(self.FUNCTIONS) - 1:
            self.preview_result.set_array(preview)
        self.update_uvc_result_title()

    def process_uvc_frame(self, original_gray):
        return process_uvc_pipeline_frame(
            original_gray,
            self.build_uvc_processing_config(),
            clahe_cache={},
            fieldscale_states={},
        )

    def build_uvc_overview_items(self):
        input_label = (
            "Native DN" if self.uvc_pipeline_input_max > 255 else "UVC Gray"
        )
        items = [
            ("00 Original UVC", self.uvc_original_frame, 255),
            (
                f"01 Pipeline Input - {input_label}",
                self.uvc_pipeline_input,
                self.uvc_pipeline_input_max,
            ),
        ]
        for overview_index, record in enumerate(
            self.uvc_pipeline_records, start=2
        ):
            items.append(
                (
                    f"{overview_index:02d} Step {record['pipeline_index']:02d} "
                    f"{record['name']}",
                    record["image"],
                    255,
                )
            )
        return items

    def start_uvc_recording(self, _checked=False):
        if self.uvc_recording_worker is not None:
            QMessageBox.information(
                self, "Recording Active", "UVC recording is already active."
            )
            return
        if (
            self.uvc_worker is None
            or self.uvc_original_frame is None
            or self.uvc_processed_frame is None
            or self.uvc_pipeline_input is None
        ):
            QMessageBox.information(
                self,
                "No UVC Frame",
                "Start the camera and wait for both previews first.",
            )
            return

        requested_fps = float(self.uvc_fps_combo.currentData())
        recording_fps = (
            self.uvc_actual_fps
            if 1.0 <= self.uvc_actual_fps <= 240.0
            else requested_fps
        )
        started_at = datetime.now().astimezone().isoformat(timespec="seconds")
        out = self.make_output("uvc_recording")
        video_path = out / "comparison_pipeline.avi"
        overview_path = out / "00_pipeline_overview.png"
        manifest_path = out / "pipeline.json"

        try:
            save_pipeline_overview_png(
                overview_path, self.build_uvc_overview_items()
            )
            manifest = {
                "format_version": 1,
                "mode": "uvc_recording",
                "started_at": started_at,
                "capture_mode": DEFAULT_UVC_CAPTURE_MODE,
                "selected_capture_mode": getattr(
                    self.uvc_worker, "selected_capture_mode", None
                ),
                "camera_index": self.selected_camera_info(
                    self.uvc_camera_combo
                )["index"],
                "camera_name": self.selected_camera_info(
                    self.uvc_camera_combo
                )["name"],
                "camera_device_id": self.selected_camera_info(
                    self.uvc_camera_combo
                )["device_id"],
                "requested_fps": requested_fps,
                "reported_camera_fps": round(self.uvc_actual_fps, 3),
                "recording_fps": round(recording_fps, 3),
                "source_resolution": {
                    "width": int(self.uvc_original_frame.shape[1]),
                    "height": int(self.uvc_original_frame.shape[0]),
                },
                "recording_layout": "Original UVC | Final Pipeline Output",
                "recording_output": video_path.name,
                "overview_output": overview_path.name,
                "bit_depth": self.bit_depth,
                "pipeline": self.uvc_pipeline_editor.configuration(),
                "execution_at_start": pipeline_execution_json(
                    self.uvc_pipeline_records
                ),
            }
            with open(manifest_path, "w", encoding="utf-8") as destination:
                json.dump(manifest, destination, ensure_ascii=False, indent=2)
        except Exception as exc:
            QMessageBox.critical(self, "Recording Error", str(exc))
            self.log.append(f"UVC recording setup error: {exc}")
            return

        worker = UvcRecordingThread(video_path, recording_fps, self)
        worker.finished.connect(self.handle_uvc_recording_finished)
        worker.finished.connect(worker.deleteLater)
        self.uvc_recording_worker = worker
        self.uvc_recording_output_dir = out
        self.uvc_recording_manifest = manifest
        self.uvc_recording_stop_reason = None
        self.uvc_recording_started_perf = time.perf_counter()
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn.setEnabled(True)
        worker.start()
        worker.submit(
            self.uvc_original_frame,
            self.uvc_processed_frame,
            self.uvc_recording_started_perf,
        )
        self.log.append(
            f"UVC recording started: {video_path} "
            f"({recording_fps:.3f} FPS, {UVC_RECORDING_FOURCC})"
        )
        self.log.append(f"Saved recording overview: {overview_path}")

    def write_uvc_recording_summary(self, worker, reason):
        out = self.uvc_recording_output_dir
        manifest = self.uvc_recording_manifest
        if out is None or manifest is None:
            return
        stats = worker.result_summary or {}
        error_message = worker.error_message
        manifest["ended_at"] = datetime.now().astimezone().isoformat(
            timespec="seconds"
        )
        manifest["stop_reason"] = reason
        manifest["success"] = error_message is None and bool(
            stats.get("written_frames", 0)
        )
        manifest["error"] = error_message
        manifest["recording"] = stats
        recorded_pipeline_text = " -> ".join(
            stage["name"]
            for stage in manifest["pipeline"]
            if stage.get("enabled")
        )
        with open(out / "pipeline.json", "w", encoding="utf-8") as destination:
            json.dump(manifest, destination, ensure_ascii=False, indent=2)

        with open(
            out / "recording_summary.txt", "w", encoding="utf-8"
        ) as summary:
            summary.write(f"Started: {manifest['started_at']}\n")
            summary.write(f"Ended: {manifest['ended_at']}\n")
            summary.write(f"Stop reason: {reason}\n")
            summary.write(f"Success: {manifest['success']}\n")
            if error_message:
                summary.write(f"Error: {error_message}\n")
            summary.write(
                f"Camera: {manifest['camera_name']} "
                f"(index {manifest['camera_index']})\n"
            )
            summary.write(
                f"Camera device ID: {manifest['camera_device_id']}\n"
            )
            summary.write(f"Capture mode: {manifest['capture_mode']}\n")
            summary.write(
                f"Selected capture mode: {manifest['selected_capture_mode']}\n"
            )
            summary.write(
                f"Source resolution: "
                f"{manifest['source_resolution']['width']}x"
                f"{manifest['source_resolution']['height']}\n"
            )
            summary.write(
                f"Requested / reported / recording FPS: "
                f"{manifest['requested_fps']:.3f} / "
                f"{manifest['reported_camera_fps']:.3f} / "
                f"{manifest['recording_fps']:.3f}\n"
            )
            summary.write(f"Codec: {stats.get('codec', '--')}\n")
            summary.write(
                f"Duration: {stats.get('duration_seconds', 0.0):.3f} s\n"
            )
            summary.write(
                f"Submitted / written / duplicate frames: "
                f"{stats.get('submitted_frames', 0)} / "
                f"{stats.get('written_frames', 0)} / "
                f"{stats.get('duplicate_frames', 0)}\n"
            )
            summary.write(
                f"Queue / timestamp dropped frames: "
                f"{stats.get('queue_dropped_frames', 0)} / "
                f"{stats.get('timestamp_dropped_frames', 0)}\n"
            )
            summary.write(
                f"File size: {stats.get('file_size_bytes', 0)} bytes\n"
            )
            summary.write(f"Overview: {manifest['overview_output']}\n")
            summary.write(f"Pipeline: {recorded_pipeline_text}\n")

    def finalize_uvc_recording_worker(self, worker, reason):
        if worker is not self.uvc_recording_worker:
            return
        try:
            self.write_uvc_recording_summary(worker, reason)
        except Exception as exc:
            self.log.append(f"UVC recording summary error: {exc}")

        out = self.uvc_recording_output_dir
        video_path = None if out is None else out / "comparison_pipeline.avi"
        if worker.error_message:
            self.log.append(f"UVC recording error: {worker.error_message}")
            self.uvc_status.setText(
                f"Recording error: {worker.error_message}"
            )
        else:
            stats = worker.result_summary or {}
            self.log.append(
                f"UVC recording stopped: {video_path}; "
                f"written={stats.get('written_frames', 0)}, "
                f"duplicates={stats.get('duplicate_frames', 0)}, "
                f"dropped={stats.get('queue_dropped_frames', 0) + stats.get('timestamp_dropped_frames', 0)}"
            )
            self.uvc_status.setText(f"Recording saved: {video_path}")

        self.uvc_recording_worker = None
        self.uvc_recording_output_dir = None
        self.uvc_recording_manifest = None
        self.uvc_recording_stop_reason = None
        self.uvc_recording_started_perf = 0.0
        frame_ready = (
            self.uvc_worker is not None
            and self.uvc_original_frame is not None
            and self.uvc_processed_frame is not None
        )
        self.uvc_record_start_btn.setEnabled(frame_ready)
        self.uvc_record_stop_btn.setEnabled(False)

    def stop_uvc_recording(
        self,
        _checked=False,
        reason=None,
        wait_ms=5000,
        log_message=True,
    ):
        worker = self.uvc_recording_worker
        if worker is None:
            return True
        stop_reason = reason or "User requested"
        self.uvc_recording_stop_reason = stop_reason
        worker.stop()
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn.setEnabled(False)
        if not worker.wait(wait_ms):
            self.uvc_status.setText("Finishing UVC recording...")
            if log_message:
                self.log.append(
                    "UVC recording thread is still finishing the AVI file."
                )
            return False
        self.finalize_uvc_recording_worker(worker, stop_reason)
        return True

    def handle_uvc_recording_finished(self):
        worker = self.sender()
        if worker is not self.uvc_recording_worker:
            return
        reason = self.uvc_recording_stop_reason
        if reason is None:
            reason = (
                "Recording error"
                if worker.error_message
                else "Recording worker finished"
            )
        self.finalize_uvc_recording_worker(worker, reason)

    def start_uvc_camera(self, _checked=False):
        if cv2 is None:
            message = "OpenCV not installed. pip install opencv-python"
            self.log.append(f"\nERROR: {message}")
            QMessageBox.critical(self, "Camera Error", message)
            return
        try:
            processing_config = self.build_uvc_processing_config()
        except Exception as exc:
            message = str(exc)
            self.log.append(f"\nERROR: {message}")
            QMessageBox.critical(self, "UVC Pipeline Error", message)
            return

        if not self.stop_uvc_camera(log_message=False):
            self.uvc_restart_when_stopped = True
            self.log.append(
                "Previous capture thread is still stopping; camera will start "
                "automatically afterward."
            )
            return
        self.uvc_restart_when_stopped = False
        capture_mode = DEFAULT_UVC_CAPTURE_MODE
        camera_info = self.selected_camera_info(self.uvc_camera_combo)
        camera_index = camera_info["index"]
        if camera_index is None:
            QMessageBox.warning(
                self,
                "Camera Disconnected",
                "Select a connected camera before starting UVC.",
            )
            return
        requested_fps = self.uvc_fps_combo.currentData()

        processing_worker = UvcProcessingThread(self)
        processing_worker.set_config(processing_config)
        processing_worker.frame_processed.connect(self.handle_uvc_processed_frame)
        processing_worker.frame_failed.connect(self.handle_uvc_processing_failed)
        processing_worker.finished.connect(processing_worker.deleteLater)
        worker = UvcCaptureThread(
            camera_index,
            self.width,
            self.height,
            requested_fps,
            UVC_RECONNECT_SECONDS,
            self,
            frame_consumer=processing_worker.submit_frame,
            capture_mode=capture_mode,
            camera_device_id=camera_info["device_id"],
            camera_name=camera_info["name"],
        )
        worker.frame_seen.connect(self.handle_uvc_frame_seen)
        worker.camera_opened.connect(self.handle_uvc_camera_opened)
        worker.status_changed.connect(self.handle_uvc_worker_status)
        worker.finished.connect(self.handle_uvc_worker_finished)
        worker.finished.connect(worker.deleteLater)
        self.uvc_worker = worker
        self.uvc_processing_worker = processing_worker
        self.uvc_original_source = None
        self.uvc_original_frame = None
        self.uvc_processed_frame = None
        self.uvc_pipeline_input = None
        self.uvc_pipeline_input_max = 255
        self.uvc_pipeline_records = []
        self.uvc_pipeline_total_ms = 0.0
        self.uvc_last_processing_error = None
        self.uvc_frame_count = 0
        self.uvc_fps_display = 0.0
        now = time.perf_counter()
        self.uvc_last_time = now
        self.uvc_last_frame_time = now
        self.uvc_waiting_for_frame = False
        self.uvc_reconnect_requested = False
        self.uvc_actual_fps = 0.0
        self.preview_original.set_array(None)
        self.preview_result.set_array(None)
        self.uvc_watchdog_timer.start()
        self.uvc_start_btn.setEnabled(False)
        self.uvc_stop_btn.setEnabled(True)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn.setEnabled(False)
        self.uvc_status.setText(
            f"Opening {camera_info['name']} (Camera {camera_index}, "
            f"{capture_mode})..."
        )
        self.log.append(
            f"Starting {camera_info['name']} (Camera {camera_index}) in "
            f"{capture_mode} mode "
            "using the background capture thread..."
        )
        processing_order = self.uvc_pipeline_editor.summary()
        self.log.append(f"UVC processing order: {processing_order}")
        self.log.append(
            "UVC processor uses latest-frame-wins; pending old frames are replaced."
        )
        processing_worker.start()
        worker.start()

    def stop_uvc_processing_worker(self, wait_ms=5000):
        worker = self.uvc_processing_worker
        if worker is None:
            return True
        worker.stop()
        if worker.isRunning() and not worker.wait(wait_ms):
            return False
        self.uvc_processing_worker = None
        return True

    def stop_uvc_camera(self, _checked=False, log_message=True):
        self.uvc_restart_when_stopped = False
        self.stop_uvc_recording(
            reason="Camera stopped", log_message=log_message
        )
        worker = self.uvc_worker
        was_running = (
            worker is not None or self.uvc_processing_worker is not None
        )
        self.uvc_watchdog_timer.stop()
        if worker is not None:
            worker.stop()
            if not worker.wait(2000):
                self.uvc_start_btn.setEnabled(False)
                self.uvc_stop_btn.setEnabled(False)
                self.uvc_capture_btn.setEnabled(False)
                self.uvc_record_start_btn.setEnabled(False)
                self.uvc_record_stop_btn.setEnabled(False)
                self.uvc_status.setText("Camera capture thread is stopping...")
                return False
            self.uvc_worker = None
        if not self.stop_uvc_processing_worker():
            self.uvc_start_btn.setEnabled(False)
            self.uvc_stop_btn.setEnabled(False)
            self.uvc_capture_btn.setEnabled(False)
            self.uvc_record_start_btn.setEnabled(False)
            self.uvc_record_stop_btn.setEnabled(False)
            self.uvc_status.setText("UVC processing thread is stopping...")
            return False
        self.uvc_start_btn.setEnabled(True)
        self.uvc_stop_btn.setEnabled(False)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn.setEnabled(False)
        self.uvc_actual_fps = 0.0
        self.uvc_status.setText("Camera stopped")
        if was_running and log_message:
            self.log.append("Camera stopped")
        return True

    def switch_uvc_camera(self, _combo_index):
        if self.uvc_worker is not None:
            self.start_uvc_camera()

    def handle_uvc_camera_opened(
        self, camera_index, actual_width, actual_height, actual_fps
    ):
        if self.sender() is not self.uvc_worker:
            return
        self.uvc_actual_fps = float(actual_fps)
        self.uvc_last_frame_time = time.perf_counter()
        self.uvc_reconnect_requested = False
        camera_name = getattr(
            self.uvc_worker, "camera_name", f"Camera {camera_index}"
        )
        self.uvc_status.setText(
            f"{camera_name} (Camera {camera_index}) opened: "
            f"{actual_width}x{actual_height}, "
            f"reported {actual_fps:.1f} FPS; waiting for frame..."
        )
        self.log.append(
            f"{camera_name} (Camera {camera_index}) opened: "
            f"{actual_width}x{actual_height}, "
            f"requested {self.uvc_fps_combo.currentData()} FPS, "
            f"reported {actual_fps:.1f} FPS"
        )

    def handle_uvc_worker_status(self, message):
        if self.sender() is not self.uvc_worker:
            return
        self.uvc_status.setText(message)

    def handle_uvc_worker_finished(self):
        if self.sender() is not self.uvc_worker:
            return
        self.stop_uvc_recording(reason="Camera capture thread finished")
        restart = self.uvc_restart_when_stopped
        close_pending = self.uvc_close_pending
        self.uvc_restart_when_stopped = False
        self.uvc_worker = None
        self.stop_uvc_processing_worker()
        self.uvc_watchdog_timer.stop()
        self.uvc_start_btn.setEnabled(True)
        self.uvc_stop_btn.setEnabled(False)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn.setEnabled(False)
        self.uvc_actual_fps = 0.0
        self.uvc_status.setText("Camera stopped")
        if close_pending:
            self.uvc_close_pending = False
            QTimer.singleShot(0, self.close)
        elif (
            restart
            and self.current_function_index() == len(self.FUNCTIONS) - 1
        ):
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
        self.stop_uvc_processing_worker(wait_ms=500)
        self.uvc_close_pending = False
        QTimer.singleShot(0, self.close)

    def force_close_after_dual_uvc_timeout(self):
        if not self.dual_uvc_close_pending:
            return
        self.dual_uvc_display_timer.stop()
        still_running = False
        for worker in (self.dual_uvc_worker_a, self.dual_uvc_worker_b):
            if worker is not None and worker.isRunning():
                worker.terminate()
                if not worker.wait(500):
                    still_running = True
        if still_running:
            QTimer.singleShot(500, self.force_close_after_dual_uvc_timeout)
            return
        self.dual_uvc_worker_a = None
        self.dual_uvc_worker_b = None
        self.stop_dual_uvc_recording(
            reason="Application closing", wait_ms=500
        )
        self.dual_uvc_close_pending = False
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
            self.log.append(
                "UVC frame paused; keeping the last image (possible FFC)."
            )
        self.uvc_status.setText(
            f"Waiting for UVC frame {elapsed:.1f}s (possible FFC); "
            "last image retained."
        )
        if (
            elapsed >= UVC_RECONNECT_SECONDS
            and not self.uvc_reconnect_requested
        ):
            self.uvc_reconnect_requested = True
            camera_info = self.selected_camera_info(self.uvc_camera_combo)
            self.log.append(
                f"No UVC frame for {elapsed:.1f}s; requesting automatic "
                f"reconnect for {camera_info['name']} "
                f"(Camera {camera_info['index']})."
            )
            worker.force_reconnect()

    def handle_uvc_frame_seen(self, frame_time):
        if self.sender() is not self.uvc_worker:
            return
        self.uvc_last_frame_time = frame_time
        self.uvc_reconnect_requested = False
        if self.uvc_waiting_for_frame:
            self.uvc_waiting_for_frame = False
            self.log.append("UVC stream resumed.")

    def handle_uvc_frame(self, frame):
        if self.sender() is not self.uvc_worker:
            return
        processing_worker = self.uvc_processing_worker
        if processing_worker is None or not processing_worker.isRunning():
            return
        try:
            config = self.build_uvc_processing_config()
        except Exception as exc:
            self.handle_uvc_processing_error(str(exc))
            return
        processing_worker.submit(frame, config)

    def handle_uvc_processed_frame(self, payload):
        if self.sender() is not self.uvc_processing_worker:
            return
        if payload["revision"] != self.uvc_processing_revision:
            return
        self.uvc_last_processing_error = None
        self.uvc_original_source = payload["original_source"]
        self.uvc_original_frame = payload["original_display"]
        self.uvc_pipeline_input = payload["pipeline_input"]
        self.uvc_pipeline_input_max = payload["pipeline_input_max"]
        self.uvc_processed_frame = payload["final_image"]
        self.uvc_pipeline_records = payload["records"]
        self.uvc_pipeline_total_ms = payload["total_ms"]
        self.current_original = self.uvc_original_frame
        self.preview_original.set_array(self.uvc_original_frame)
        self.update_uvc_stage_preview()
        self.uvc_capture_btn.setEnabled(True)

        now = time.perf_counter()
        recording_worker = self.uvc_recording_worker
        if recording_worker is not None and recording_worker.isRunning():
            recording_worker.submit(
                self.uvc_original_frame,
                self.uvc_processed_frame,
                now,
            )
            self.uvc_record_start_btn.setEnabled(False)
            self.uvc_record_stop_btn.setEnabled(True)
        elif recording_worker is None:
            self.uvc_record_start_btn.setEnabled(True)
            self.uvc_record_stop_btn.setEnabled(False)
        else:
            self.uvc_record_start_btn.setEnabled(False)
            self.uvc_record_stop_btn.setEnabled(False)
        self.uvc_frame_count += 1
        elapsed = now - self.uvc_last_time
        if elapsed >= 0.5:
            self.uvc_fps_display = self.uvc_frame_count / elapsed
            self.uvc_frame_count = 0
            self.uvc_last_time = now
        requested_fps = self.uvc_fps_combo.currentData()
        budget_ms = 1000.0 / requested_fps
        budget_text = (
            "OVER BUDGET"
            if self.uvc_pipeline_total_ms > budget_ms
            else "within budget"
        )
        detail_parts = list(payload["preprocessing_details"])
        detail_parts.extend(
            f"S{record['pipeline_index']} {record['name']} "
            f"{record['elapsed_ms']:.1f}ms"
            for record in self.uvc_pipeline_records
        )
        detail_text = " | ".join(detail_parts)
        recording_text = ""
        if recording_worker is not None and recording_worker.isRunning():
            recording_text = (
                f"REC {max(0.0, now - self.uvc_recording_started_perf):.1f}s | "
            )
        selected_capture_mode = getattr(
            self.uvc_worker, "selected_capture_mode", None
        )
        selected_capture_name = UvcCaptureThread._capture_mode_name(
            selected_capture_mode or DEFAULT_UVC_CAPTURE_MODE
        )
        camera_info = self.selected_camera_info(self.uvc_camera_combo)
        self.uvc_status.setText(
            recording_text
            + f"{camera_info['name']} (Camera {camera_info['index']}) | "
            f"Capture {selected_capture_name} | "
            f"{self.uvc_original_frame.shape[1]}x"
            f"{self.uvc_original_frame.shape[0]} | "
            f"Output FPS {self.uvc_fps_display:.1f} | "
            f"Pipeline {self.uvc_pipeline_total_ms:.1f} ms / "
            f"{budget_ms:.1f} ms ({budget_text})"
            + (f" | {detail_text}" if detail_text else "")
        )

    def handle_uvc_processing_error(self, message):
        if self.uvc_recording_worker is not None:
            self.stop_uvc_recording(reason="UVC processing error")
        if message != self.uvc_last_processing_error:
            self.log.append(f"UVC frame processing error: {message}")
        self.uvc_last_processing_error = message
        self.uvc_processed_frame = None
        self.uvc_pipeline_records = []
        self.current_result = None
        self.preview_result.set_array(None)
        self.uvc_capture_btn.setEnabled(False)
        self.uvc_record_start_btn.setEnabled(False)
        self.uvc_record_stop_btn.setEnabled(False)
        self.uvc_status.setText(f"Frame processing error: {message}")

    def handle_uvc_processing_failed(self, payload):
        if self.sender() is not self.uvc_processing_worker:
            return
        if payload.get("revision") != self.uvc_processing_revision:
            return
        self.handle_uvc_processing_error(payload["message"])

    def capture_uvc_pair(self):
        if self.uvc_original_frame is None or self.uvc_processed_frame is None:
            QMessageBox.information(
                self,
                "No UVC Frame",
                "Start the camera and wait for a frame first.",
            )
            return
        out = self.make_output("uvc_algorithm_compare")
        original_path = out / "original.png"
        pipeline_input_path = out / "pipeline_input.png"
        processed_path = out / "final_pipeline.png"
        comparison_path = out / "comparison_pipeline.png"
        overview_path = out / "00_pipeline_overview.png"
        save_png(original_path, self.uvc_original_frame)
        save_png(pipeline_input_path, self.uvc_pipeline_input)
        step_output_files = {}
        if self.uvc_save_intermediates.isChecked():
            for record in self.uvc_pipeline_records:
                step_path = out / (
                    f"step_{record['pipeline_index']:02d}_"
                    f"{record['algorithm']}.png"
                )
                save_png(step_path, record["image"])
                step_output_files[record["pipeline_index"]] = step_path.name
        save_png(processed_path, self.uvc_processed_frame)
        save_uvc_comparison_png(
            comparison_path,
            self.uvc_original_frame,
            self.uvc_processed_frame,
            "Final Pipeline Output",
        )
        input_label = (
            "Native DN" if self.uvc_pipeline_input_max > 255 else "UVC Gray"
        )
        overview_items = [
            ("00 Original UVC", self.uvc_original_frame, 255),
            (
                f"01 Pipeline Input - {input_label}",
                self.uvc_pipeline_input,
                self.uvc_pipeline_input_max,
            ),
        ]
        for overview_index, record in enumerate(
            self.uvc_pipeline_records, start=2
        ):
            overview_items.append(
                (
                    f"{overview_index:02d} Step {record['pipeline_index']:02d} "
                    f"{record['name']}",
                    record["image"],
                    255,
                )
            )
        save_pipeline_overview_png(overview_path, overview_items)
        manifest = {
            "format_version": 1,
            "mode": "uvc_algorithm_compare",
            "capture_mode": DEFAULT_UVC_CAPTURE_MODE,
            "selected_capture_mode": getattr(
                self.uvc_worker, "selected_capture_mode", None
            ),
            "camera_index": self.selected_camera_info(
                self.uvc_camera_combo
            )["index"],
            "camera_name": self.selected_camera_info(
                self.uvc_camera_combo
            )["name"],
            "camera_device_id": self.selected_camera_info(
                self.uvc_camera_combo
            )["device_id"],
            "requested_fps": self.uvc_fps_combo.currentData(),
            "resolution": {
                "width": int(self.uvc_original_frame.shape[1]),
                "height": int(self.uvc_original_frame.shape[0]),
            },
            "selected_thermal_resolution": {
                "width": self.width,
                "height": self.height,
            },
            "bit_depth": self.bit_depth,
            "pipeline": self.uvc_pipeline_editor.configuration(),
            "execution": pipeline_execution_json(self.uvc_pipeline_records),
            "total_ms": round(self.uvc_pipeline_total_ms, 3),
            "original_output": original_path.name,
            "pipeline_input_output": pipeline_input_path.name,
            "final_output": processed_path.name,
            "comparison_output": comparison_path.name,
            "overview_output": overview_path.name,
        }
        for execution in manifest["execution"]:
            output_file = step_output_files.get(execution["pipeline_index"])
            if output_file:
                execution["output_file"] = output_file
        with open(out / "pipeline.json", "w", encoding="utf-8") as destination:
            json.dump(manifest, destination, ensure_ascii=False, indent=2)
        with open(out / "pipeline_summary.txt", "w", encoding="utf-8") as summary:
            summary.write(
                f"Camera: {manifest['camera_name']} "
                f"(index {manifest['camera_index']})\n"
            )
            summary.write(
                f"Camera device ID: {manifest['camera_device_id']}\n"
            )
            summary.write(f"Capture mode: {manifest['capture_mode']}\n")
            summary.write(
                f"Selected capture mode: {manifest['selected_capture_mode']}\n"
            )
            summary.write(
                f"Actual resolution: {manifest['resolution']['width']}x"
                f"{manifest['resolution']['height']}\n"
            )
            summary.write(f"Overview: {overview_path.name}\n")
            summary.write(f"Pipeline: {self.uvc_pipeline_editor.summary()}\n")
            for execution in manifest["execution"]:
                summary.write(
                    f"Step {execution['pipeline_index']}: {execution['name']} - "
                    f"{execution['elapsed_ms']:.3f} ms - "
                    f"{' | '.join(execution['details'])}\n"
                )
            summary.write(f"Total: {self.uvc_pipeline_total_ms:.3f} ms\n")
        self.log.append(f"Saved: {original_path}")
        self.log.append(f"Saved: {processed_path}")
        self.log.append(f"Saved comparison: {comparison_path}")
        self.log.append(f"Saved overview: {overview_path}")
        self.log.append(f"Pipeline settings: {out / 'pipeline.json'}")

    def save_current_preview(self):
        if self.current_result is None:
            QMessageBox.information(
                self, "No Preview", "Run a function first."
            )
            return
        function_index = self.current_function_index()
        is_uvc_algorithm = (
            self.FUNCTIONS[function_index] == "UVC Algorithm Compare"
        )
        if is_uvc_algorithm:
            if self.uvc_original_frame is None:
                QMessageBox.information(
                    self,
                    "No UVC Frame",
                    "Start the camera and wait for both previews first.",
                )
                return
            out = self.make_output("manual_preview")
            original = self.uvc_original_frame.copy()
            processed = self.current_result.copy()
            algorithm_title = self.uvc_result_title()
            original_path = out / "original_uvc.png"
            processed_path = out / "processed_selected_pipeline.png"
            comparison_path = out / "comparison_selected_pipeline.png"
            save_png(original_path, original)
            save_png(processed_path, processed)
            save_uvc_comparison_png(
                comparison_path, original, processed, algorithm_title
            )
            self.log.append(f"Saved UVC preview: {algorithm_title}")
            self.log.append(f"Saved original: {original_path}")
            self.log.append(f"Saved processed: {processed_path}")
            self.log.append(f"Saved comparison: {comparison_path}")
            return

        out = self.make_output("manual_preview")
        path = out / "preview.png"
        is_bpr_marking = (
            self.FUNCTIONS[function_index] == "BPR Bad Pixel Marking"
        )
        if is_bpr_marking:
            preview_to_save = np.clip(
                self.current_result, 0, 255
            ).astype(np.uint8)
        elif function_index == 4:
            preview_to_save = signed_delta_to_display_u8(
                self.current_result, self.raw_max
            )
        elif function_index in (0, 1, 2, 3, 5):
            preview_to_save = raw_to_display_u8(
                self.current_result, self.raw_max
            )
        else:
            preview_to_save = np.clip(
                self.current_result, 0, 255
            ).astype(np.uint8)
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
        self.dual_uvc_restart_when_stopped = False
        self.camera_refresh_timer.stop()
        self.uvc_watchdog_timer.stop()
        self.dual_uvc_display_timer.stop()
        if not self.stop_dual_uvc_recording(
            reason="Application closing", wait_ms=3000
        ):
            QTimer.singleShot(500, self.close)
            event.ignore()
            return
        dual_workers = (
            self.dual_uvc_worker_a,
            self.dual_uvc_worker_b,
        )
        if any(
            worker is not None and worker.isRunning()
            for worker in dual_workers
        ):
            if not self.dual_uvc_close_pending:
                self.dual_uvc_close_pending = True
                self.dual_uvc_start_btn.setEnabled(False)
                self.dual_uvc_stop_btn.setEnabled(False)
                self.dual_uvc_capture_btn.setEnabled(False)
                self.dual_uvc_record_start_btn.setEnabled(False)
                self.dual_uvc_record_stop_btn.setEnabled(False)
                self.dual_uvc_status_a.setText(
                    f"Stopping {self.dual_uvc_image_name_a.currentText()} before exit..."
                )
                self.dual_uvc_status_b.setText(
                    f"Stopping {self.dual_uvc_image_name_b.currentText()} before exit..."
                )
                for dual_worker in dual_workers:
                    if dual_worker is not None:
                        dual_worker.stop()
                QTimer.singleShot(
                    UVC_CLOSE_TIMEOUT_MS,
                    self.force_close_after_dual_uvc_timeout,
                )
            event.ignore()
            return
        self.dual_uvc_worker_a = None
        self.dual_uvc_worker_b = None
        if not self.stop_uvc_recording(
            reason="Application closing", wait_ms=3000
        ):
            self.uvc_status.setText("Finishing recording before exit...")
            QTimer.singleShot(500, self.close)
            event.ignore()
            return
        worker = self.uvc_worker
        if worker is not None and worker.isRunning():
            if not self.uvc_close_pending:
                self.uvc_close_pending = True
                self.uvc_start_btn.setEnabled(False)
                self.uvc_stop_btn.setEnabled(False)
                self.uvc_capture_btn.setEnabled(False)
                self.uvc_record_start_btn.setEnabled(False)
                self.uvc_record_stop_btn.setEnabled(False)
                self.uvc_status.setText("Stopping camera before exit...")
                worker.stop()
                QTimer.singleShot(
                    UVC_CLOSE_TIMEOUT_MS,
                    self.force_close_after_uvc_timeout,
                )
            event.ignore()
            return
        self.uvc_worker = None
        self.stop_uvc_processing_worker()
        self.uvc_close_pending = False
        super().closeEvent(event)


def main():
    app=QApplication(sys.argv)
    app.setStyle("Fusion")
    w=ThermalRawToolkit(); w.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()

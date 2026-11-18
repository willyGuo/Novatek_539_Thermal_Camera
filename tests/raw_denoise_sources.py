from pathlib import Path

RAW_DENOISE_DIR = Path(__file__).resolve().parents[1] / 'Enhancement' / 'RAW'
RAW_DENOISE_SOURCES = [
    str(RAW_DENOISE_DIR / 'raw_temporal_denoise.c'),
    str(RAW_DENOISE_DIR / 'Spatial' / 'raw_spatial_denoise.c'),
    str(RAW_DENOISE_DIR / 'Temporal' / 'raw_temporal_kernel.c'),
]

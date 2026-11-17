"""Extract the matched SDK's filter initializer; never modify the SDK."""
import argparse
import hashlib
from pathlib import Path
import re

def prepare(sdk, output):
    source = sdk / 'hdal/vendor/isp/drivers/source/iq_539a/iq_flow/iq_common_param.c'
    text = source.read_text(encoding='utf-8')
    start = text.index('CTL_IFE_ISP_FILTER ctl_ife_filter_init = ')
    body = text[text.index('{', start):]
    depth = 0
    for i, ch in enumerate(body):
        depth += (ch == '{') - (ch == '}')
        if depth == 0:
            body = body[:i+1]
            break
    # Only enums in the real initializer are translated; unknown enums fail.
    translations = {
        'CTL_IFE_ISP_SPTL_FILT_5X5': 'KDRV_IFE_SPTL_FILT_5X5',
        'CTL_IFE_ISP_BILAT_CEN_DPC': 'KDRV_IFE_IQ_BILAT_CEN_DPC',
    }
    for old, new in translations.items():
        body = body.replace(old, new)
    if 'CTL_IFE_' in body:
        raise ValueError('Unmapped SDK enum in filter initializer')
    output.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    (output / 'probe_filter_generated.h').write_text(
        '/* Generated from iq_539a/iq_flow/iq_common_param.c\n'
        f' * SHA256: {digest}\n * Diagnostic baseline, not sensor tuning. */\n'
        'static const KDRV_IFE_IQ_FILTER probe_sdk_filter = ' + body + ';\n',
        encoding='utf-8')
    print(f'SDK filter source SHA256: {digest}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path, required=True, help='matching SDK code directory')
    parser.add_argument('--output', type=Path, default=Path(__file__).parent / 'generated')
    args = parser.parse_args()
    prepare(args.sdk, args.output)

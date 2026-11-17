#!/bin/sh
# Run only after manually stopping every ISP/VideoProc application.
set -eu
out=${1:?usage: sh run_probe.sh /mnt/sd/ife_probe_NEW_DIRECTORY}
root=/proc/ife_raw14_probe
[ -r "$root/status" ] || { echo 'Load the matching probe module first'; exit 1; }
mkdir "$out" # Fail if it exists; never overwrite a previous capture.
capture() {
    name=$1
    command=$2
    printf '%s\n' "$command" > "$root/status"
    cat "$root/status" > "$out/${name}_status.txt"
    cat "$root/input.raw" > "$out/${name}_input.raw"
    cat "$root/output.raw" > "$out/${name}_output.raw"
    [ "$(wc -c < "$out/${name}_input.raw")" -eq 614400 ]
    [ "$(wc -c < "$out/${name}_output.raw")" -eq 614400 ]
}
capture bypass_ramp 'bypass 0'
cmp "$out/bypass_ramp_input.raw" "$out/bypass_ramp_output.raw" || {
    echo 'STOP: bypass ramp changed DN; saved evidence. Do not enable NR.'; exit 1;
}
capture bypass_levels 'bypass 1'
cmp "$out/bypass_levels_input.raw" "$out/bypass_levels_output.raw" || {
    echo 'STOP: bypass boundaries changed DN; saved evidence.'; exit 1;
}
capture denoise_levels 'denoise 1'
capture bypass_noise 'bypass 2'
cmp "$out/bypass_noise_input.raw" "$out/bypass_noise_output.raw" || exit 1
capture denoise_noise 'denoise 2'
echo "Captures saved to $out; analyze them on the host with analyze.py"

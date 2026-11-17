NUC Q14 -> Q8 source/host audit

Scope: read-only firmware audit; no firmware or CFG changes. No SDK linking,
board GFX execution, or current SD K/B validation was performed.

1. Format consistency
- NUC_GAIN_SHIFT=8, NUC_GAIN_ONE=256.
- Calibration K calculation, B conversion, unity initialization and GFX property
  all reference the same Q8 constants.
- Fixed tables load from /mnt/sd/thermal_op/sensor_temp; dynamic tables use
  /mnt/sd/thermal_op/blackbody_temp. No Q14 -> Q8 runtime conversion is present.
- The pre-v3.6.0 source used the same Q8 constants and correction submission.
- No leftover Q14 rescale was found in the current NUC implementation.

2. Actual production C builder versus independent calculation
Historical 20261029 ATTO640 Low/High calibration inputs, 640x480 (307200 pixels),
were used solely as test vectors. They are NOT current SD-card calibration data.
All 307200 generated K entries and all 307200 B entries matched the independent
NumPy calculation exactly. K=231..342; signed B=-3101..6031.
The CPU forward model (K*(RAW-B))>>8 did not force 4-DN spacing.
Unity K=256, B=0 preserves every input DN across the complete 0..16383 ramp.
Host model results: Low target 9213, spatial SD 0.492188 DN, error -1..1 DN.
High target 11173, mean 11169.249336, SD 2.263270 DN, error -9..1 DN.
This High bias is consistent with truncating Q8 K (coarser than Q14), not proof
of a board format error. The arithmetic model is not GFX hardware validation.

3. Confirmed validation limitation
The Q8 loader only accepts/rejects based on 1..32767 numeric range and length.
A Q14 unity coefficient 16384 passes that check but represents 64x in Q8.
The complete historical Q14 point1 K table (14832..21890) also passes the current
Q8 range check. Directory separation is the only current format distinction.
Therefore copying or renaming Q14 K/B into sensor_temp is unsafe. There is no
evidence that the user has done this; current SD files were unavailable.

4. Other consequences of restoring Q8
- A pre-existing sensor_temp folder may contain an older calibration than the
  latest sensor_temp_q14 folder. Restoring the path does not migrate calibration.
- FFC remains Q8 independently. Its saved shading header includes NUC_GAIN_SHIFT;
  a shading file saved with Q14 NUC will fail the format check under Q8 NUC.

5. Required next evidence
Current /mnt/sd/thermal_op/sensor_temp contents, particularly point1_K.bin,
point1_B.bin and saved Low/High RAWs if available. Source and host tests cannot
establish what is actually loaded on the board or explain the 4-DN hardware output.

Artifacts
report.json: host results, historical source location and current source hashes.
audit.c / audit.exe: host harness using the production NUC C implementation.
host_q8_K.bin / host_q8_B.bin: generated only for this audit; DO NOT deploy them.

Existing tests/test_nuc_q8.py passed for TY640 and ATTO640 profiles in this audit.

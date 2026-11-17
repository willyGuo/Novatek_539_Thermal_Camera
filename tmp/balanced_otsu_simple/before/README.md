# Tiffany Thermal

NT98539A thermal ISP 的 RGB LPF／CFA bypass 修改、SDK 補丁及模糊排查結果，
請見 [SDK ISP 修正與驗證](sdk_patches/README.md)。此修正需要同步重建對應 IQ 模組。

`Tiffany_thermal` 是 Novatek NS02302 Linux 平台上的 thermal sensor 整合程式，包含：

- Thermal liveview
- BPR／ISP DPC 壞點校正
- NUC 兩點式非均勻性校正
- AGC 前可設定 GFX 硬體右移 0～3 bits
- FFC shutter offset 校正
- 設定檔保存與開機載入
- GPIO shutter 控制

程式啟動時會同步完成 liveview 初始化，再進入互動式主選單。持續拉取與處理 RAW 的工作由 `raw_pipeline_thread` 執行；離開程式時會先停止並 join RAW thread，再依序釋放 HDAL、ISP 與影像 buffer 資源。

## 目錄與模組

目前 `Makefile` 實際編譯以下來源：

```text
Tiffany_thermal/
├─ main.c                         程式入口
├─ app.c                          app 初始化、主選單與結束流程
├─ Makefile
├─ Pipeline/
│  ├─ menu.c                     主選單
│  └─ liveview.c                 HDAL、ISP、VCAP、NUC、FFC 與輸出 pipeline
├─ Config/
│  └─ app_config.c               tiffany_thermal.cfg 讀寫
├─ Correction/
│  ├─ BPR/bpr.c                  DPC table 產生、載入與 runtime enable
│  ├─ NUC/nuc.c                  NUC K/B table 校正、載入與套用
│  └─ FFC/ffc.c                  shutter offset table 產生與套用
├─ Enhancement/
│  └─ Gamma/gamma.c              ISP Gamma 設定與 LUT 載入
└─ Driver/GPIO/
   └─ gpio_control.c             shutter GPIO 控制
```

根目錄的舊版 `video_liveview_with_vcap_thermal.c` 不在目前 `Makefile` 的 `SRC` 中，不是整合版程式的 runtime owner；目前請以 `Pipeline/liveview.c` 為準。

## Build

將目錄放在 Novatek SDK 的 HDAL samples 下，例如：

```sh
cd ~/ns02302_linux_sdk/code/hdal/samples/Tiffany_thermal
make clean
make
```

### TY640／ATTO640 編譯切換

同一份來源使用 `Config/hardware_config.h` 的 `THERMAL_SENSOR` define
選擇感測器，預設為 `THERMAL_SENSOR_TY640`。可直接將預設定義改成
`THERMAL_SENSOR_ATTO640`，或在 SDK build 時指定：

```sh
make clean
make THERMAL_SENSOR=ATTO640
# 切回 TY640
make clean
make THERMAL_SENSOR=TY640
```

Makefile 的參數會轉成 `-DTHERMAL_SENSOR=1`（TY640）或
`-DTHERMAL_SENSOR=2`（ATTO640），優先於 header 的預設。
也可由外部 build 系統直接傳入
`-DTHERMAL_SENSOR=THERMAL_SENSOR_ATTO640`。切換後必須 clean，避免沿用
另一個感測器編譯的 object；未知選項會在 Makefile／preprocessor 階段報錯。

| 項目 | TY640 | ATTO640 |
|---|---|---|
| VCAP driver | `nvt_sen_ty640` | `nvt_sen_atto640` |
| 有效 RAW／輸出尺寸 | 640×512 | 640×480 |
| RAW16／K／B 單檔大小 | 655,360 bytes | 614,400 bytes |
| VCAP 介面／route ID | TDIO／4 | TDIO／4 |
| TY640 外部 SPI／I²C 初始化 | 保留 | 略過；ATTO sensor I²C 由 SDK driver 管理 |
| Shutter `P_GPIO(0)` 電位 | Low 開／High 關 | High 開／Low 關 |
| 感測器／鏡頭溫度、TDC、FOS | 保留原實作 | 尚無對應協定，回報不支援 |
| NUC 選項 1 | 原有 Low／High 校正與溫度記錄 | Low／High 固定 K/B 校正，複製至五點；載入固定 point1，略過溫度切換 |

記憶體池、NUC／FFC table、CPU AGC、CLAHE／DDE、VideoProc、VPE、
UVC／RTSP 和快照／錄製檔名均跟隨選定尺寸；UVC 開啟時仍以實際輸出尺寸
更新描述資料。ATTO640 SDK driver 的 mode 1 宣告為 640×480、14-bit thermal，
沿用 RAW16 LSB container 與目前 RAW14 演算法。

ATTO 模式的溫度紀錄會留空並標記 `read_failed`，不填入假的溫度；
即使 CFG 要求 TDC 開啟，也會回報不支援並保持 runtime 關閉。
固定 NUC 載入仍要求完整 point1..point5 K/B（沿用既有檔案完整性檢查），
Blackbody Dynamic NUC 則保留既有五點 RAW/K/B 流程。
ATTO 的 NUC 選項 1 顯示 `Two-point NUC calibration`：先確認最新 BPR 表可載入，
再對 Low／High 各取 4 張平均，產生 Q8 K/B 並複製至 point1..point5。
同樣保存 Low／High 輸入 RAW，成功後自動開啟 NUC 並請求快照；
不讀取感測器溫度，也不以假溫度覆寫 CFG 內既有五點資訊。
校正表必須使用該感測器實際量測的資料，不可直接共用兩顆感測器的表。

ATTO 取像設定取自同事的 `original_SDK_ATTO640/atto640_261012/code/hdal/samples/Tiffany_thermal`，
沿用既有演算法與 buffer ownership 保護。
兩個原始 sample 都使用 P_GPIO(0) shutter（低開、高關）與
`/mnt/sd/isp_ty640_0.cfg` ISP reload 路徑，因此這兩項維持原值；
ISP 檔案內容必須換成對應感測器的 tuning，必要時可覆寫
`THERMAL_ISP_CONFIG_PATH`。Shutter 接線也必須符合該 GPIO／極性。
同事版本另外加入的 USB CDC echo 不屬於 sensor 取像設定，本次未加入。

define 只切換 application；板端仍需對應的 kernel sensor driver、sensor cfg、
device tree／pinmux 和 ISP tuning。ATTO SDK 已含 `sen_atto640` driver 與
`sen_atto640_538.cfg`（含 sensor ID 4），不能只換 application binary 就假設
原 TY640 BSP 已具備這些內容。兩種模式皆需 SDK 編譯及上板驗證出流、
快照尺寸、FFC 與校正表載入。

主機分支與初始化測試可執行 `python tests/test_sensor_profiles.py`，需要 GCC
或透過 `CC` 指定相容編譯器。測試涵蓋預設／兩種感測器的 driver 與尺寸、
初始化成功／失敗路徑、ATTO 不連結 TY640 peripheral 呼叫，以及未知 define
拒絕；不代表 SDK linking、kernel driver 或實機驗證。

`Correction/BPR/bpr.c` 只提供 BPR 功能，ISP lifecycle 由 `Pipeline/liveview.c` 統一管理。

目前 Windows checkout 沒有 SDK `make` 環境，最終編譯與上板測試需在 Novatek SDK Linux 環境執行。

## 執行

不帶參數時使用程式預設值：

```sh
./Tiffany_thermal
```

感測器由編譯時 define 選擇，`main(void)` 不解析 runtime sensor 參數。
預設值：

```text
sensor                     = THERMAL_SENSOR_TY640 (compile time)
VCAP ID                    = 4
fps cfg fallback           = 30
startup skip cfg fallback  = 30 frames
```

FPS 與啟動時跳過的 frame 數量不接受命令列參數，分別由 `/mnt/sd/tiffany_thermal.cfg` 的 `fps=` 與 `startup_skip_frames=` 載入。若 cfg 不存在、沒有對應欄位，或 `cfg_boot_load=0`，則兩者都使用程式預設值 `30`。

UVC 可透過同一個 CFG 切換 `fps=30`／`fps=60`，預設仍為 30。修改後重新啟動程式，或在 CFG boot load 啟用時執行 Liveview Restart；FPS 不是串流中的即時切換。VCAP 的 FPS 要求、UVC 的 MJPEG／H.264 相容描述表與 UVAC 初始串流資訊會使用相同設定；此程式實際輸出仍為 MJPEG。UVC 只宣告本次 CFG 選定的 FPS，電腦端也要選擇相同 FPS；重新開啟相機，必要時重新連接 USB，使主機讀取更新的描述表。

應用程式允許最高 60 FPS，不代表實體 Sensor／處理管線已達到 60 FPS。配套 ATTO640 driver 的 `mode_basic_param` 仍宣告預設 30 FPS，`sen_chg_fps_atto640()` 沒有實際切換操作；此修改不猜測 Sensor 時序或更動 driver。若設定 60 後輸入仍只有 30，需要確認 Sensor 實際輸出及配套 driver，再量測處理、編碼與 USB 的實際 FPS。

## 啟動流程

```text
main()
  -> app_init()
       -> liveview_init()
       -> liveview_start()
            -> 同步初始化 HDAL / ISP / thermal buffer
            -> 建立 raw_pipeline_thread
            -> 啟動 VCAP / VideoProc / VideoOut
            -> 返回 menu thread
  -> app_run()
       -> 顯示 Thermal menu
  -> app_deinit()
       -> 停止並 join raw_pipeline_thread
       -> 釋放 runtime 資源
```

Liveview 啟動期間由 `Pipeline/liveview.c` 負責：

- `hd_common` 與 common memory pool
- `hd_videocap`、`hd_videoproc`、`hd_videoout`
- `hd_gfx`
- `vendor_isp_init()`／`vendor_isp_uninit()`
- shutter GPIO
- NUC／FFC runtime buffer
- capture thread 與 VideoProc 輸出

## 主選單

```text
Thermal menu
 1. Liveview
 2. CFG Setting
 3. BPR
 4. NUC
 5. FFC
 6. TDC
 7. AGC
 8. FOS
 9. VideoProc / ISP
10. Image Pipeline Diagnostics
 q. Quit
```

在任一選單的 `>>` 提示直接按 Enter，不會返回上一層或結束程式；使用 `fgets()` 的選單會重畫目前選單，BPR 選單則維持等待輸入。輸入非法字串時會顯示 `Invalid option` 並留在原選單；子選單明確輸入 `0` 或 `q` 才返回上一層，最外層輸入 `q` 才結束程式。

各設定選單只更新 `/mnt/sd/tiffany_thermal.cfg`，不會立刻修改目前的 runtime。設定完成後，必須由使用者執行 `Liveview Restart` 或重新開機才會套用；程式儲存設定後也會印出相同提示。

## Liveview

Liveview 在程式啟動時已完成初始化，RAW pipeline 在工作 thread 中執行。選單用來查看或控制整條 pipeline：

```text
Liveview menu
 1. Status
 2. Liveview control
 3. Shutter control
 4. Snapshot RAW
 5. Recording RAW
 6. Show current YUV statistics
 0. Back
```

`Liveview control` 與 `Shutter control` 進入後再選擇實際動作：

```text
Liveview control
 1. Start
 2. Stop
 3. Restart
 0. Back

Shutter control
 1. Open
 2. Close
 0. Back
```

FPS、startup skip 與 runtime skip 都移到 `CFG Setting -> Liveview setting`。`runtime_skip_frames` 套用後，`raw_pipeline_thread` 每成功送出一張影像，就會丟棄指定數量的 VCAP frame；`0` 表示每張都處理。VCAP buffer 仍只由 RAW pipeline thread pull／release。

自動 FFC shutter-closed calibration 執行期間，`Shutter open` 與 `Shutter close` 指令會立即被拒絕，不會等待校正結束後再執行。

`Snapshot RAW` 會在下一張正常 frame 完成 TDC／NUC／FFC 及啟用時的 RAW temporal denoise 後、AGC 前取樣並寫入 `/mnt/sd/`。檔案只保留有效影像區 `640x512`，每個 pixel 為 little-endian 16-bit container：

- TDC／NUC／FFC／Shift 都未套用：`snapshot_NNNN_raw14_640x512.raw`
- 有套用 TDC、NUC、FFC 或 Shift：同時保存 `snapshot_NNNN_input_raw14_640x512.raw` 與 `snapshot_NNNN_corrected_raw16_640x512.raw`；input 為原始取樣，corrected 包含當張成功套用的 Shift
- Histogram P1/P99 AGC (CPU) 成功時：另外保存同一張 frame 在 VideoProc 前的 `snapshot_NNNN_agc_output_raw16_640x512.raw`

每個檔案大小為 `640 * 512 * 2 = 655360 bytes`。RAW pipeline thread 先複製同一張 frame 的各階段資料，再由背景 writer 寫入 SD 卡；快照當下仍會增加一次或多次 frame copy 的負擔。

### Timed Snapshot

在 `Image Pipeline Diagnostics` 選擇 `7. Run Timed Snapshot (every 1 minute, 32 snapshots)` 啟動。Liveview 必須先啟動；Run 後等待 60 秒擷取第一次，此後每隔 60 秒擷取一次，共成功保存 32 次後自動結束，正常情況約 32 分鐘。選單立即返回，可用 `8. Show Timed Snapshot status` 查看進度、`9. Stop Timed Snapshot` 提前停止、`0. Back` 離開選單繼續背景擷取。

每次 Run 建立獨立目錄 `/mnt/sd/thermal_snapshot/raw/timed_<時間戳>_<流水號>/`，不覆蓋既有結果。每次保存同一張 frame 的 `snapshot_0001_input_raw16_640x512.raw`；若套用 TDC／NUC／FFC，另存 `snapshot_0001_corrected_raw16_640x512.raw`；Histogram／Fieldscale／Weighted CPU AGC 成功時另存 `snapshot_0001_agc_output_raw16_640x512.raw`。編號由 `0001` 到 `0032`，每張 RAW 為 little-endian UINT16，655,360 bytes；ISP Tone Curve 不另存 AGC RAW。

同目錄的 `summary.csv` 可用 Excel 開啟，每次 snapshot 一列，包含次數、從 Run 起算的實際擷取時間 `elapsed_ms`、`sensor_temp_c`（攝氏，小數點後 4 位）、`sensor_temp_status`，以及 input／corrected／agc_output 各自的檔名、`min`、`max`、`mean`、`stddev`。溫度使用與 `Show Current Sensor Temperature` 相同的 `sensor_temp_read()`，於該次 RAW 複製完成後、交給背景 writer 前即時讀取，並印出 `Current Sensor Temperature`；這是擷取附近的感測器溫度，不是影像中的物體溫度，也不是與曝光硬體同步的讀值。讀取成功標記 `ok`，失敗時溫度留空、標記 `read_failed`，仍保存 RAW 與統計，不沿用上一次讀值。統計直接計算已複製的完整 640×512 RAW DN，包含極端值，不轉成 8-bit、不排除壞點；`stddev` 使用母體標準差（除以總 pixel 數）。未產生的 corrected／AGC 階段留空。

RAW 與 CSV 在背景 writer 寫入並同步到 SD 後才累計成功次數；CSV 每次追加一列，完成後為 32 列總表。手動校正、Step Test 或 writer 忙碌時延後至下一張可用影格，延後後不連續補拍，因此完成時間可能超過 32 分鐘。停止 Timed Snapshot 或 Stop／Restart Liveview 會停止後續排程，已排入的當次保存會完成，並保留 RAW 與部分總表；重新 Run 從新目錄重新計數。RAW 複製、存檔或 CSV 寫入失敗會停止並顯示錯誤，失敗次數不計入完成數。Timed Snapshot 與連續 `Recording RAW` 不可同時啟動。

RAW Statistics（包含開機第一張 RAW 的統計）已移除。VideoProc 不再開啟診斷 OUT_1；YUV Statistics 與 ISP Step Test 共用主路 OUT_0，在 VPE 鏡像、CLAHE、DDE 與偽色彩處理前讀取影像。手動執行診斷時，統計與存檔仍可能暫時增加顯示延遲。

`Show current YUV statistics` 會對下一張 VideoProc OUT_0 的 Y plane 顯示 min／max／mean、P1／P50／P99、unique value 數量、Y=0／Y=255 的 pixel 數量與比例，以及可能被 limited-range 顯示截斷的 Y<=16／Y>=235 比例，並在 `/mnt/sd/` 保存：

- `display_diag_NNNN_videoproc_y8_640x512.raw`：VideoProc 後、JPEG 前的 8-bit Y plane，固定為 `640 * 512 = 327680 bytes`
- `display_diag_NNNN_uvc_mjpeg_640x512.jpg`：UVC encoder 產生、送往 USB 前的單張 MJPEG bitstream；檔案大小依影像內容而變動

兩個檔案共用相同的 `NNNN` 診斷編號且不覆蓋既有檔案。OUT1 YUV 與 OUT0 encoder 由不同執行緒取得，因此是同一次請求附近的 frame，不保證為硬體鎖定的相同 timestamp。RTSP mode 只保存 Y8，沒有 JPEG 診斷檔。

即時 RAW pipeline：

```text
VCAP RAW
  -> optional NUC
  -> optional FFC
  -> VideoProc
  -> VideoOut
```

RAW→VideoProc 使用既有影像 buffer；逐幀 RAW staging 與額外 3 個 pool block 維持撤回。校正／CPU AGC 輸出送入 RAWALL 前，先登記該幀 timestamp／count；收到相同 timestamp 的 RAWALL OUT_0（timestamp 為 0 時才使用非零 count）後，RAW worker 才繼續下一幀，避免固定輸出 buffer 在下游完成前被下一幀覆寫。登記早於非阻塞 push，避免快速輸出造成漏接通知；push 失敗會取消登記。等待最多 500 ms，未確認完成時停止 pipeline，保留資源至 Stop 清理；Stop 會喚醒等待中的 RAW worker。直接使用 VCAP 時維持先 push 再 release，交由 SDK 的來源 buffer reference 管理。

啟動時印出 `RAW_INPUT_SYNC enabled`；第一筆及每完成 300 幀印出 `RAW_INPUT_SYNC ok`，包含 `completed`、`wait_error` 及 `max_wait_us`（送入前登記至對應 RAWALL 輸出被拉取的最長時間）。此同步會限制 RAW worker 的提前處理量，需上板比較 FPS、最大等待時間及殘影是否改善；主機同步測試不代表已證明原本殘影的根因。

VCAP、RAWALL 與 VPE 各自保存成功 pull 的原始 frame descriptor；release 失敗時保留該幀、停止後續取幀並要求停止 Liveview，由 Stop 重試。Stop 清理失敗時保留對應 handle 與狀態，須再次執行 Stop，成功前不允許 Start／Restart。開啟測試診斷輸出時，VCAP 與 RAWALL 每成功釋放 300 幀印出 `ownership` 計數，停止時另列出 VPE 與 UVC 計數；`pull`／`release` 為應用程式成功次數，`held` 為尚未釋放幀數，`release_error` 為失敗次數，不等同 kernel `/proc` 計數定義。

`Pipeline/frame_diagnostics.h` 的 `FRAME_DIAGNOSTICS_ENABLED` 預設為 `0`，暫停 `FRAME_TRACE`、正常 ownership 計數、RAW_INPUT_SYNC 成功計數與 Weighted 定期耗時輸出；buffer 釋放、RAW 輸入同步保護與錯誤訊息仍保留。需要恢復追蹤時，以 `-DFRAME_DIAGNOSTICS_ENABLED=1` 編譯。以下影格時序追蹤說明適用於開啟診斷輸出時。

UVC 在複製 cacheable encoder bitstream 前執行 DMA-from-device cache 同步。`UVAC_WaitStrmDone()` 失敗時停止送流，保留 MJPEG 傳輸 buffer，直到 Stop join worker 並關閉 UVAC 後才允許回收；編碼輸出的 release 失敗也保留 descriptor 供 Stop 重試。上板時需在出流期間連續擷取 `cat /proc/hdal/*/info`，對照應用程式計數及錯誤訊息，另驗證長時間播放、UVC 開關、Stop／Restart 與畫面異常。

影格時序追蹤使用 `FRAME_TRACE` 前綴，記錄 `VCAP`、`RAWALL_IN`、`RAWALL_OUT`、`VPE_OUT`（鏡像啟用時）、`ENC_IN`、`ENC_OUT`、`USB_SUBMIT` 與 `USB_DONE`。各階段第一幀及約每 300 幀印一次，各階段錯開輸出以降低 UART 集中印字的影響；時間戳倒退／重複、非零序號倒退、到達間隔超過 200 ms，或相對首幀的延遲增量超過 200 ms 時印出 event，異常輸出最多每階段每秒一行。累計數不受日誌限速影響。`wall` 是板端 monotonic 時間，`pts` 是 SDK frame timestamp，皆以微秒表示；`back`／`dup` 是時間戳倒退／重複次數，`cback` 是非零序號倒退次數。`lag_growth_us`／`maxlag_us` 是該階段相對首幀的延遲增量／最大增量，不是絕對端到端延遲；時間戳為 0 時累計 `zero`，不能據此判斷重播。FFC、手動校正、負載或 UART 印字也可能造成間隔增加，event 是調查線索。USB_DONE 只表示 UVAC 等待完成，不代表電腦螢幕已顯示。追蹤不改寫時間戳、不額外複製影像、不丟棄影格；新時間戳搭配舊像素仍需另查內容與 buffer 使用時序。

## CFG Setting 選單

主選單選擇 `2. CFG Setting` 後會顯示：

```text
CFG Setting
 1. Liveview setting
 2. BPR setting
 3. Shutter / FFC setting
 4. NUC setting
 5. TDC setting
 6. AGC setting (mutually exclusive)
 7. VideoProc / ISP setting
 8. Show current tiffany_thermal.cfg
 9. Generate tiffany_thermal.cfg
10. Use CFG on next boot/restart on/off
 0. Back
pending CFG: boot load on/off
```

所有手動調整的持久化參數都集中在這裡。各分類會同時顯示 runtime 與 pending CFG；修改時只寫入 `/mnt/sd/tiffany_thermal.cfg`，必須 Liveview Restart 或重新開機才套用。所有 on/off 項目都會再要求明確輸入 `0: off` 或 `1: on`，不會直接反轉目前設定。

- `Liveview setting`：FPS、startup skip frames、runtime skip frames。
- `BPR setting`：BPR enable 與 Bright/Dark threshold。
- `Shutter / FFC setting`：FFC enable、shutter interval（0 停止定時校正）、shutter close wait time 與啟動校正開關。
- `NUC setting`：NUC enable 與 Sensor Temperature table interval。五點 Sensor 溫度由 NUC calibration 自動寫入，不提供手動修改。
- `TDC setting`：TDC enable 與 update interval。
- `AGC setting`：Histogram P1/P99、Fieldscale、Weighted Mapping 三種 AGC 互斥。
- `VideoProc / ISP setting`：AF、WDR、DEFOG、3DNR 個別開關，以及全開／全關。
- `Show current tiffany_thermal.cfg`：直接顯示 SD 卡上的完整 cfg。
- `Generate tiffany_thermal.cfg`：將 pending CFG 寫入設定檔。
- `Use CFG on next boot/restart on/off`：控制 `cfg_boot_load=1/0`。

進入選單時顯示的 `pending CFG: boot load on/off` 是待套用的 `cfg_boot_load` 狀態。若 cfg 檔不存在，程式啟動時會以當下程式預設值建立一份新檔案。

## 設定檔

設定檔固定為：

```text
/mnt/sd/tiffany_thermal.cfg
```

專案內的預設參考檔位於 `Config/tiffany_thermal_default.cfg`，可複製到上述 target 路徑使用。

內容格式範例：

```ini
# Tiffany thermal user settings
cfg_boot_load=1
fps=30
output_mode=0
startup_skip_frames=30
runtime_skip_frames=0
nuc_correction=1
nuc_sensor_temp_interval_sec=1
nuc_sensor_temp_point1=0.0000
nuc_sensor_temp_point2=0.0000
nuc_sensor_temp_point3=0.0000
nuc_sensor_temp_point4=0.0000
nuc_sensor_temp_point5=0.0000
bpr_correction=1
bpr_threshold=15
ffc_correction=1
shutter_interval_sec=60
shutter_close_wait_ms=200
tdc_correction=1
tdc_interval_sec=60
agc_histogram_p1_p99_enable=0
agc_fieldscale_enable=0
videoproc_af_enable=1
videoproc_wdr_enable=1
videoproc_defog_enable=1
videoproc_3dnr_enable=1
```

欄位說明：

| Key | 說明 |
|---|---|
| `cfg_boot_load` | `1`：開機套用 cfg；`0`：使用程式預設值 |
| `fps` | VCAP FPS，預設 `30` |
| `startup_skip_frames` | 每次 Liveview Start／Restart 後先跳過的 frame 數，預設 `30` |
| `runtime_skip_frames` | 每成功送出一張影像後要跳過的 VCAP frame 數，預設 `0`；`0` 表示每張都處理 |
| `nuc_correction` | NUC runtime on/off |
| `nuc_sensor_temp_interval_sec` | Sensor Temperature NUC table 的檢查間隔 |
| `nuc_sensor_temp_point1` ... `point5` | 五點 Sensor Temperature NUC 的溫度點 |
| `bpr_correction` | ISP DPC/BPR runtime on/off |
| `bpr_threshold` | BPR calibration threshold，單位為 RAW8 DN，預設 `15` |
| `ffc_correction` | 是否套用有效 FFC 補償表，預設 `1`；CFG 選單修改立即生效。開啟時若沒有有效表格，先執行一次 FFC，成功後補償；間隔為 `0` 時仍可持續補償 |
| `ffc_shading_enable` | 0/1; default 0; requires calibrated shading table and FFC on |
| `ffc_startup_enable` | Liveview 啟動時是否先校正一次，預設 `1`；不受 interval 是否為 `0` 影響，也不會自動改動補償開關 |
| `shutter_interval_sec` | 定時 FFC 間隔秒數，範圍 `0..INT_MAX`，預設 `60`；`0` 停止定時校正，CFG 選單修改後立即生效 |
| `shutter_close_wait_ms` | 快門關閉後開始 FFC 前的等待時間，範圍 `1..INT_MAX`，預設 `200` ms；CFG 選單修改後下一次校正就使用新值 |
| `tdc_correction` | TDC runtime on/off |
| `tdc_interval_sec` | TDC 更新間隔 |
| `agc_histogram_p1_p99_enable` | Histogram P1/P99 CPU AGC on/off；三種 AGC 只能擇一開啟 |
| `agc_fieldscale_enable` | Fieldscale 8x8 Local Adaptive CPU AGC on/off；三種 AGC 只能擇一開啟 |
| `agc_weighted_enable` | Weighted Mapping CPU AGC on/off，預設 0；三種 AGC 只能擇一開啟 |
| `videoproc_af_enable` | VideoProc AF flag on/off，預設 `1` |
| `videoproc_wdr_enable` | VideoProc WDR flag on/off，預設 `1` |
| `videoproc_defog_enable` | VideoProc DEFOG flag on/off，預設 `1` |
| `videoproc_3dnr_enable` | VideoProc 3DNR 與 3DNR statistics flag on/off，預設 `1` |
如果檔案不存在，程式會建立預設 cfg。任何設定修改只寫入 cfg；使用者必須執行 `Liveview Restart` 或重新開機才會套用。

Histogram P1/P99 的設定選單新增 `5. Gain Limit`，CFG 為 `agc_histogram_p1_p99_gain_limit=0.0`：預設 0 關閉，可設 0.1..4.0，步進 0.1，單位為 8-bit Gray／原始 RAW DN。設定會儲存並於下一張 Histogram frame 套用，同時重置門檻平滑歷史。限制作用於平滑後的線性斜率，保留 Low／High DN 中點的中灰亮度；生效時不強制拉滿黑白，區間外依同一斜率延伸並限制在 RAW14 0..16383。全平影像保留既有 RAW14 複製行為。關閉時保留原本逐像素結果；Gain Limit 不影響既有 Smooth 權重設定。計算重用 histogram workspace 建立 LUT，不另配置影像記憶體。

## Weighted Mapping AGC（全畫面 1×1）

從 `CFG Setting → AGC setting → 5. Weighted Mapping AGC (CPU, 1x1) setting` 開啟並設定參數。Liveview 執行中選擇不同 AGC 或關閉 AGC 時，由 RAW worker 等待上一張 RAWALL 輸入完成後，在下一張影格交界重置 Smooth 並更新 ISP Tone Curve；不 Stop／Start Liveview，不關閉 UVC 或編碼器，也不重新執行 Startup FFC。錄製與 Timed Snapshot 保持原本排程，切換後的影像使用新 AGC。快速連續選擇以 worker 取出時最新的請求為準；工作記憶體缺失、ISP baseline 不可用或 Tone Curve 更新失敗時保留原 runtime 並印出錯誤。即使 `cfg_boot_load=0`，本次選擇仍會套用；Liveview 原本停止時不會因選擇 AGC 自動開始出流。UVC 連線不重建，切換瞬間是否有停格或亮度跳動仍需上板驗證。`AGC → 5. Weighted Mapping AGC` 可查看 runtime 狀態。

| CFG key | 預設 | 範圍／用途 |
|---|---|---|
| `agc_weighted_enable` | 0 | 開啟時停用其他 AGC |
| `agc_weighted_clip_limit` | 2.0 | 0 關閉；0.1..100.0，步進 0.1；Clip count = round(Clip × pixel count / Unique Levels)，最少 1 |
| `agc_weighted_redistribute` | 1 | 0／1；只向原本 pixel count > 0 的 DN bin 分配裁切餘量，維持 Clip count 上限，無容量可分配的餘量捨棄 |
| `agc_weighted_bright_weight` | 1.0 | 有限數值 ≥ 1，無固定上限；1 為 Off；double 儲存並保留小數 |
| `agc_weighted_bright_percentile` | 70.0 | 0..100，步進 0.1；從 Clip／Redistribution 後的直方圖重算亮部門檻 |
| `agc_weighted_gain_limit` | 2.0 | 0 關閉；0.1..4.0，步進 0.1；單位為 8-bit Gray／原始 RAW DN |
| `agc_weighted_temporal_previous_percent` | 90 | LUT／Classic Gain 補償的歷史權重 0..99%；平衡模式同時保護背景追蹤；0 關閉 Smooth 與追蹤 |
| `agc_weighted_balanced_background` | 0 | 0 保留 Classic；1 開啟平衡背景／高溫分段 LUT，不含 DDE |
| `agc_weighted_reset_defaults` | 0 | 設 1：載入完成後重設所有 Weighted Mapping 參數，成功儲存後此鍵回到 0 |

Weighted CFG 選單第 8 項為 Balanced Background 開關，第 9 項重設 Weighted Mapping 參數（包含關閉 Balanced Background），預設值統一使用 `AGC_WEIGHTED_DEFAULT_PARAMS`；不更動 AGC 開關與其他功能。已移除功能的舊 CFG 鍵會被忽略，後續儲存不再寫出。

Weighted Mapping 直接建立 `0..16383` 的 RAW14 LUT，輸入與輸出皆為 UINT16 容器中的 RAW14 數值，不再先量化成 256 階。AGC／2DNR 順序維持原狀。

Classic 先建立 RAW14 直方圖，依既有 Clip／Redistribution 規則處理，以 `W(i)=H(i)+H(i+1)` 計算區間權重。Bright Percentile 由處理後直方圖求得，亮部區間乘 Bright Weight，累積後直接正規化到 `0..16383`。Bright Weight 仍支援任意有限且 >=1 的值，極大值使用等比例縮放。純 Classic 的單一 DN 全平影像仍對應 0。

Bright Weight 門檻 `T_P` 採用累積比例嚴格超過 P 的第一個整數 DN，不做百分位插值；P=100% 時取最後一個保留像素的 DN。區間判斷簡化為 `k >= T_P`，在整數門檻下與 `k+0.5 >= T_P` 完全等價。這項整數門檻定義取代原本的插值百分位，因此部分影像的加權範圍會改變。Gain Limit 使用 `min(ΔS(k), G)`；輸入 LUT 單調不遞減，故與 `min(max(ΔS(k), 0), G)` 等價，無須額外限制差值下限。


Smooth 在 RAW14 DN 的 Q8 精度下混合 LUT，不平均影像像素。Classic Gain Limit 的 CFG 單位仍是 Gray/RAW DN，內部精確換算為 `gain_limit * 16383 / 255` 的原生輸出 DN 斜率。例如 2.0 約為 128.49 輸出 DN/輸入 DN。斜率限制與直方圖 pixel-count 加權平均亮度補償都保留 Q8 DN，補償也沿用 Smooth；限制後不會再拉滿全範圍。飽和與最終 DN 取整仍可能造成微小平均亮度誤差。

目前流程為 `Clip／Redistribution → Bright Weight → RAW14 LUT → Smooth → Gain Limit → 輸出`，Gain Limit 是最後的映射調整。尺寸、參數、AGC 模式、FFC／校正及 Restart 會重設 LUT 與亮度補償歷史。

Balanced Background（`agc_weighted_balanced_background=1`）沿用 Toolkit 的
`balanced_background_v2` 分段灰階曲線，SoC R4 擴充背景群選擇：依至少 12 個連續
空 DN bin 分群，每群至少 0.3% 且至少 2 個像素；有效群的最高／最低均值差至少 70 DN。
正常 Smooth 追蹤且有兩群以上時，優先選距離歷史中心 40 DN 內的最近群，同距離取較大群。
沒有合適歷史且有三群以上時，選像素最多的群；偶數像素群的中位數保留兩中間值的平均。
Smooth=0 與 FFC／開快門重建期間，每幀選群完全不參考歷史。
至少 90% 像素低於舊中心 40 DN 以上時，不讓舊暖群附近的小群阻止 R3 的快速恢復。
其餘情況使用原本低溫群／Otsu：兩群各至少 0.3%
且各至少 2 點、均值差至少 70 DN；優先取有至少 12 空 DN bin 分隔的低溫群，
否則用群間變異至少占全圖 85% 的 Otsu 分割；沒有可信分群時回退全圖中位數。
只有兩群且沒有有效歷史時，仍保留低溫背景優先；單靠直方圖無法保證區分冷物體、
冷背景與占多數的前景。三群最大群亦只是啟始估計，不保證最大群在物理上就是背景。
背景中心目標 112 Gray；
熱尾啟用後最多降低 20 Gray，背景斜率為 1.2 Gray/RAW DN（Gain Limit 更小時一併降低）。
中心以下以 `16 + (target-16)*exp(gain*(DN-center)/(target-16))` 平滑壓縮，
在中心保持亮度／斜率連續，暗端趨近 16 Gray，不再直接壓成 0。SoC 用兩次 exp
及逐 DN 遞推產生暗部 LUT，不逐像素計算 exp。
熱尾偵測條件為背景群中心 +40 DN 以上像素至少 0.3%，且 P99.95 超過該偵測門檻至少 30 DN；
面積達 1.5% 時完全啟用。高溫映射從背景群中心 +20 DN 起，獨立執行 Clip／Redistribution／
Bright Weight，分配剩餘灰階至 245 Gray。高溫百分位沿用 Toolkit 的插值門檻，
不更動 Classic 的整數逆 CDF 定義。這些 DN 是輸入資料域，不是溫度門檻或人手辨識。

SoC 保留原生 RAW14 LUT，不先量化成 Toolkit 的 8-bit LUT，因此比較 Toolkit 時
會有量化差異。合成 LUT 沿用 Q8 Smooth，最後固定起點限制斜率，不做全畫面平均亮度補償，
不再拉滿黑白，也不加入 DDE。開關／參數切換會重設歷史，CFG 選單儲存後於下一張
Weighted frame 生效，不需切換 AGC。現有設定檔缺少新鍵時預設關閉；兩份隨附 CFG 也預設 0。
分段映射沿用既有影像工作區，背景追蹤僅增加少量固定狀態，不做逐幀配置；
連續影片、FFC 過渡、熱物體占大部分畫面及板端 FPS 仍需實測。

平衡模式在 Smooth >0 時增加背景追蹤保護：分群不可信且相對上一個可靠中心仍有熱尾時，
保留原中心，不立即切到偏暖的全圖中位數。中心候選差超過 4 DN 時，需連續 3 幀的
候選值彼此在 4 DN 內才接受；接受後依 Smooth 權重追蹤，每幀最多移動 1 DN。
背景目標同樣使用 Smooth 權重，每幀最多改變 0.5 Gray，再經原本的 Q8 LUT Smooth。
Smooth=0 保留無歷史的 v2 行為；參數／尺寸／AGC／校正切換沿用既有重置。
這限制的是映射參數的變化，不保證移動物體或輸入 DN 本身改變時每個像素都只變 0.5 Gray。

FFC 成功開鏡、等待並丟棄過渡影格後，Balanced Background 會先重新建立中心、
目標灰階與 LUT，期間逐幀使用當前 RAW，不混入 FFC 前歷史或套用上述限速／LUT Smooth。
至少連續觀察 8 個非均勻影格，整段中心候選最大值減最小值不超過 2 DN、目標灰階
範圍不超過 0.5 Gray、P99.95−P0.05 至少 4 DN，才恢復正常防跳；最多觀察 90 幀。
這是處理影格數，並非固定秒數。持續每幀漂移 3 DN 不再被誤判穩定。
Shading、手動校正 generation 變更及手動開快門成功時亦會重新建立。
手動開快門只通知 RAW worker 重建 Weighted 與 RAW temporal 歷史，不改 FFC 表、不執行 FFC。
UI 執行緒不直接存取影像歷史。實體遮住鏡頭沒有 GPIO 通知，不在此事件處理範圍。
另有影像分布恢復條件，不依賴 GPIO：中心候選連續 3 幀彼此在 4 DN 內，
且比舊中心低至少 70 DN、至少 90% 像素低於舊中心 40 DN 以上時，視為舊中心
已失去目前畫面的支持，直接重建中心、目標與 LUT，再重新觀察穩定度。
這涵蓋帶雜訊快門畫面先通過穩定判斷、實際場景稍後才出現的情況；少量冷物體
不會觸發此條件。UART `Weighted BG R4` 會記錄請求、穩定／逾時及失效中心恢復事件。
Classic 映射公式、Smooth=0 與現有 CFG 數值不變。這可避免初始過渡畫面中心偏高時，
開鏡後仍以每幀 1 DN 追趕，造成背景長時間偏暗、對比緩慢恢復。
只有少量 Q8 狀態，沒有逐幀記憶體配置，也不平均前後影像像素。Toolkit 靜態 v2 不含此追蹤。

平衡模式主機檢查：`python tests/test_weighted_balanced.py --toolkit <Thermal_Image_Toolkit>`；
可加 `--raw-dir <RAW目錄> --output-dir <驗證輸出目錄>` 儲存主機 C 產生的 RAW14／PNG 與誤差報告。
測試涵蓋 50 組 native 參考曲線，另有 35 組 v2 測試（高溫面積 1..99%、跨越一半畫面、
Otsu 無空隙分群、暗部保護與少量極低值），以及極大 Bright Weight、Q8 Smooth、開關切換重設及非法值；
`--before-dir <修改前agc_weighted.c/.h目錄>` 可另外檢查關閉開關的結果與修改前 C 完全一致。

Weighted 工作區為 229,448 bytes（主機 GCC 對齊；背景追蹤比原版增加 16 bytes），開始出流前配置、停止時釋放。沿用既有 RAW snapshot、輸出 block 與 RAW_INPUT_SYNC。

主機測試：`python tests/test_weighted_raw14.py` 以獨立 NumPy 參考驗證 RAW14 LUT、16384 階 identity、Gain Limit、Smooth、重置、stride 與 in-place；`--raw-dir` 加入實際 RAW，`--output-dir` 輸出 JSON 報告。`python tests/test_weighted_pipeline.py` 以 mock HDAL 驗證兩種尺寸的實際管線函式。`python tests/test_weighted_config.py --sdk <SDK/code>` 驗證 CFG／選單／重置／儲存失敗回復、三種 AGC 互斥及忽略已移除的設定鍵。這些測試不代表 SDK 連結或板端效能已驗證。

`python tests/test_weighted_config.py --sdk ../ns2501_linux_sdk-main/code` 使用實際 CFG parser、load/save/update 與選單原始碼，驗證 TY640／ATTO640 的參數儲存讀回、reset 載入順序、消耗 reset flag、runtime 套用與儲存失敗 rollback。以上是主機測試，不代表 SDK 連結、板端 FPS 或實機影像已驗證。

## BPR／DPC

BPR 使用 Novatek DPC library 找出 bright/dark bad pixels，產生 ISP runtime 使用的 DPC table。

固定路徑：

```text
ISP cfg : /mnt/sd/isp_ty640_0.cfg
DPC bin : /mnt/sd/thermal_op/dpc_table.bin
```

BPR 選單：

```text
BPR menu
 1. Bright/Dark BPR calibration
 2. Reload BPR table
 3. BPR debug on/off
 0. Back
```

功能說明：

- `Bright/Dark BPR calibration`：依序取得 bright 與 dark calibration RAW，產生並保存 `dpc_table.bin`。
- `Reload BPR table`：從 SD 卡重新讀取 DPC table，直接套用到 ISP。
- `BPR debug on/off`：控制 DPC library log 與 `/mnt/sd/` RAW dump。

BPR enable 與 Bright/Dark threshold 已集中到 `CFG Setting -> BPR setting`；FFC interval 位於 `CFG Setting -> Shutter / FFC setting`。

`bpr_threshold` 在 CFG 選單儲存成功後立即更新執行中的參數，不需重開機，用於下一次 Bright/Dark BPR 校正；修改閾值不會自動重建既有 `dpc_table.bin`。BPR on/off 即時套用。

手動 `Bright/Dark BPR calibration` 開始前會先等待正在執行的自動 shutter cycle 完成，接著暫停 interval 關快門；成功或失敗離開時，若 `shutter_interval_sec>0` 且 FFC 開啟，從完整間隔重新計時；間隔為 `0` 則維持停止定時校正。

BPR calibration 產生 table；runtime 是否補點則由 `bpr_correction` 控制。TY640 RAW14 以 LSB 對齊方式存放在 RAW16 container，每個 pixel 佔 2 bytes；DPC table 產生流程會取有效資料的 bit 13..6 轉成 RAW8，供 calibration library 使用，但 liveview runtime RAW pipeline 仍維持 RAW16，並不因此改成 RAW8。

## NUC

NUC 是兩點式非均勻性校正。校正時依序取得 low-temperature 與 high-temperature blackbody RAW，計算每個 pixel 的 K/B table。

Sensor Temperature 五點表路徑：

```text
K table : /mnt/sd/thermal_op/sensor_temp/point1_K.bin ... point5_K.bin
B table : /mnt/sd/thermal_op/sensor_temp/point1_B.bin ... point5_B.bin
```

Blackbody Temperature 五點保存路徑：

```text
Reference RAW : /mnt/sd/thermal_op/blackbody_temp/point1_RAW.bin ... point5_RAW.bin
K table : /mnt/sd/thermal_op/blackbody_temp/point1_K.bin ... point5_K.bin
B table : /mnt/sd/thermal_op/blackbody_temp/point1_B.bin ... point5_B.bin
```

NUC 選單：

```text
NUC menu
 1. 5 point Sensor Temperature NUC calibration
 2. 5 point Blackbody Temperature NUC calibration (save RAW/K/B)
 3. Toggle per-pixel Blackbody Dynamic NUC
 4. Reload Sensor Temperature K/B table
 0. Back
```

NUC enable 與 Sensor Temperature table interval 已集中到 `CFG Setting -> NUC setting`。

`Sensor Temperature NUC calibration (capture once, copy to 5 points)` 只量測一次 Low／High，每個條件連續擷取 4 張 RAW，由 CPU 對 `640x512` 做平均，產生同一組 K/B 並複製到五點；五個 Sensor 溫度值也使用同一次量測結果。完整 K/B table 為 `640x512`，對應板端 VCAP 實際回傳的 active-only RAW frame。校正過程先將五點 K/B table 寫入 `.new` 暫存檔，全部成功且大小正確後才替換正式檔案；任一點失敗都會放棄本次結果，保留原本五點 table。啟用 NUC 前，point1 到 point5 的 K/B 檔案必須全部存在且大小正確。

Low／High 都取像成功後，在計算 K/B 前保存四張平均的輸入至 `/mnt/sd/thermal_op/sensor_temp/low_RAW_BPR{閾值}.bin` 與 `high_RAW_BPR{閾值}.bin`，例如 `low_RAW_BPR15.bin`、`high_RAW_BPR15.bin`。後綴使用開始 Low／High 取像前的 `bpr_threshold` 設定值，兩檔使用相同值；此值不代表既有 DPC table 建立時的閾值。格式為無檔頭 UINT16 RAW（板端 little-endian、640×512、每檔 655,360 bytes），未做 8-bit 映射。每檔先寫 `.new` 並驗證大小，再取代同名檔；相同閾值再次校正會覆寫，不同閾值分開保留，存檔失敗會中止校正。這兩檔記錄最近取像結果，即使後續 K/B 計算失敗仍保留，不與 K/B table 一起回復。兩檔分別更新，第二檔存檔失敗時可能分屬不同次取像，需以存檔成功訊息確認。

`5 point Blackbody Temperature NUC calibration (save RAW/K/B)` 必須由低溫到高溫依序量測五個黑體爐點，每點連續擷取 4 張 RAW 並平均。每點會保存該張 `pointN_RAW.bin` 參考影像，並建立一組 one-point offset table：所有 K 都是 Q8 的 `256`，B 為 `pixel_average - frame_average`，因此 GFX 公式會把校正點的均勻黑體影像拉回該張影像的全畫面平均值。五組 RAW/K/B 先寫入 `.new`，15 個檔案全部完成且大小正確後才替換 `blackbody_temp` 內的正式檔案；失敗時保留上一批結果。此校正選項本身只產生及保存檔案，不更新 Sensor Temperature CFG、不自動切換 runtime 模式。point1 到 point5 的實際黑體爐溫度值由操作者記錄，firmware 目前不保存溫度 metadata。

`Toggle per-pixel Blackbody Dynamic NUC` 開啟時會一次載入五組 K/B，並由五張 reference RAW 預先計算每個 Pixel 的四個相鄰中點。每張即時 RAW 送進 NUC 前，CPU 對 `640x512` 每個 Pixel 比較四個中點，選出 point1 到 point5，將該點同座標的 K/B 複製到目前的 DDR K/B table，再交給 `VENDOR_GRPH_CMD_Non_uni_correction`。等於中點時選較高 point。若某 Pixel 的五個 reference RAW 不是嚴格遞增，該 Pixel 改用五張 reference RAW 的全畫面平均中點，並在開啟時列出 fallback Pixel 數量。為了暫時進行板端效能測試，目前即使五個全畫面平均不是由低到高也只會印出警告並繼續啟用；此時只可使用 CPU／GFX／FPS 數據，校正畫質與 point distribution 不具判定意義。

Dynamic NUC 逐幀不讀 SD 卡。常駐 CPU heap 為五組 K/B `6,553,600` bytes，加四組 UINT16 threshold `2,621,440` bytes，共 `9,175,040` bytes；現有 GFX DDR 動態 K/B buffer 另為 `1,310,720` bytes。每 60 張會列出 mmap/cache、CPU 選表、K/B cache flush、三者合計的 `CPU added`、GFX 與 NUC 總耗時平均值、最大值、pipeline FPS，以及 P1 到 P5 的 Pixel 分布。`CPU added` 是相對原本固定 table NUC 新增的主要 CPU 成本；30 FPS 的單幀時間預算約為 `33.3 ms`。關閉 Dynamic NUC 時會釋放來源 table/threshold，並重新載入 Sensor Temperature table；Dynamic NUC 開啟期間不執行週期性的 Sensor Temperature table 更新。

手動 Bright/Dark BPR 校正前會自動關閉 BPR 與 NUC；校正成功後載入新表並開啟 BPR，NUC 保持關閉。接著執行 Sensor Temperature NUC 校正時，會先關閉 NUC、載入最新的 `dpc_table.bin` 並開啟 BPR；只有 NUC 校正、儲存與重新載入全部成功後，才自動開啟 NUC。開關同步存入 CFG，必要的開關切換或 BPR table 載入失敗時會中止校正。Blackbody 校正的使用方式維持原樣。

NUC 校正開始前會先等待正在執行的自動 shutter cycle 完成；接著暫停 interval 觸發的 FFC 關快門，讓 Low／High RAW 擷取期間保持快門開啟。校正成功或失敗離開時，若 FFC 開啟且間隔大於 `0`，從完整的 `shutter_interval_sec` 重新計時；`0` 則維持停止定時校正。

Sensor Temperature NUC（Low／High）校正成功、重新載入校正表並啟用 NUC 後，會自動要求一次 snapshot，不需手動操作。請求會等到成功套用 NUC 的影格，再依既有流程保存 input／corrected RAW；Histogram 或 Fieldscale CPU AGC 啟用且當張處理成功時，也會保存 AGC output RAW。背景 writer 忙碌時延後至可接收的影格；校正失敗不觸發，停止 Liveview 或開始下一次手動校正會取消尚未擷取的自動請求。不額外等待 AGC 時間平滑穩定，ISP Tone Curve AGC 不另存 AGC RAW。以 `snapshot saved:` 訊息確認各檔案寫入完成。

NUC runtime 與新校正的 K 恢復使用 v3.6.0 之前的 Q8 格式（FFC 仍使用自身的 Q8 格式）：

```text
NUC_GAIN_SHIFT = 8
K = stored_K / 256
identity stored_K = 256
valid stored_K = 1..32767 (maximum gain = 127.99609375)
```

讀寫原本的 `sensor_temp`／`blackbody_temp` 目錄，K 直接以 Q8 載入，不再轉成 Q14；B 維持 signed INT16 DN。保留校正表交易復原及整組完整性檢查。

既有 `sensor_temp_q14`／`blackbody_temp_q14` 目錄保留但不再載入，勿將 Q14 檔案直接複製到 Q8 目錄。五點 Q8 K 都會檢查 `1..32767`；校正計算超出範圍也會中止，保留上一組正式 table，避免 GFX 將過大的 K 當成負數。

兩點 NUC 會在覆寫 runtime K/B 前，逐點驗證轉換後的 GFX B 必須在 `-32768..32767`。任一點超出範圍即拒絕整次校正，保留原 K/B，不再截斷 B 後回報成功；Low／High 平均 RAW 仍會保存，供檢查失敗原因。

請使用原有 Q8 table 或重新校正。Q8 的增益解析度為 `1/256`，不會將 RAW 影像改成 8-bit。切換後應重新執行 FFC；若啟用 FFC Shading，須重新校正，舊 Q14 標記的 shading 檔不相容。整合後仍須重新編譯 Tiffany 並上板驗證影像。

每個 pixel 先以舊的線性形式算出 gain 與 output additive offset：

```text
K(x,y) = (high_average - low_average)
         / (high_raw(x,y) - low_raw(x,y))

old_B(x,y) = low_average - K(x,y) * low_raw(x,y)
```

接著把 `old_B` 轉成 GFX 原生公式使用的 input offset：

```text
new_B(x,y) = round(-(old_B(x,y) * 2^NUC_GAIN_SHIFT) / stored_K(x,y))

B.bin stores new_B
```

Runtime 由 `VENDOR_GRPH_CMD_Non_uni_correction` 使用：

```text
Image A = input RAW
Image B = NUC K table
Image C = NUC output
Image D = NUC B table
property = NUC_GAIN_SHIFT

signed_output = clamp_int16((stored_K * (input - new_B) + 8192) >> 14)
output_bits = (UINT16)signed_output
```

啟動時會嘗試載入選定目錄的五組 `pointN_K.bin` 與 `pointN_B.bin`。任一檔案不存在、大小不符或 K 超出上述範圍時，NUC table 會標記為未載入，並保持 NUC off。執行 calibration 或 reload 成功後才可啟用。

## TDC

TDC 選單：

```text
TDC menu
 1. Sensor TDC Calibration
 2. Lens TDC Calibration
 3. Reload
 4. Show Current Sensor Temperature
 5. Show Current Lens Temperature
 0. Back
```

選項 `4` 與 `5` 會分別透過 SPI 與 I2C 讀取當下的 Sensor／Lens 溫度並顯示攝氏值，不會修改 CFG、TDC table 或目前的 TDC enable 狀態。TDC enable 與 interval 已集中到 `CFG Setting -> TDC setting`。

TDC 與 NUC 使用相同的 RAW layout：板端 VCAP 實際回傳 active-only `640x512`，因此 active window 為 `(0,0,640,512)`。Sensor/Lens 校正的 Low 與 High 都擷取 4 張完整 RAW；CPU 對全張 `640x512` 做平均並計算 DCC/LC slope。

```text
DCC table        : 640 x 512 x float32 = 1,310,720 bytes
LC table         : 640 x 512 x float32 = 1,310,720 bytes
Correction table : 640 x 512 x int16   =   655,360 bytes
```

Runtime 每隔 `tdc_interval_sec` 由 CPU 根據 Sensor/Lens 溫度更新完整 correction table；每張 RAW 再由 GFX 對 `640x512` 執行 `output = input - correction`。舊的 `644x516` DCC/LC table 大小不符，啟動時會被拒絕，必須重新執行 Sensor 與 Lens TDC 校正。

## FFC

FFC 用 shutter 關閉時的 dark frame 建立每個 pixel 的 offset table。

FFC 選單只保留 runtime 狀態：

```text
FFC status
 1. Run FFC test
 2. Calibrate blackbody / shutter shading (32 frames each)
 0. Back
```

FFC enable、FFC interval 與 shutter close wait time 已集中到 `CFG Setting -> Shutter / FFC setting`。選單第 2 項為 `FFC interval (0 = periodic off)`。

在 CFG 選單切換 `ffc_correction` 不需重啟 Liveview。有有效表格時立即套用；沒有表格時會顯示等待校正，透過影像執行緒完成一次 FFC 與 GFX 檢查，成功且快門重新開啟後才補償。失敗時保持補償關閉，失敗的首次表格不會保留為有效表格；使用者可再次開啟以重試。等待校正時改為關閉補償，校正完成後仍維持關閉。手動執行 `Run FFC test` 本身不會自動改動補償開關。

`shutter_close_wait_ms` 修改後供下一次校正使用；若正在校正，會等待這次完成再套用。`ffc_startup_enable` 維持下次 Liveview 啟動時使用，修改當下不會觸發校正。Liveview 已停止時，這些修改只儲存 CFG，待下次啟動載入。

`ffc_startup_enable=1` 時，Liveview 啟動後先執行一次 FFC。`ffc_correction=1` 且 `shutter_interval_sec>0` 時，之後依完整間隔定期執行 shutter cycle；`shutter_interval_sec=0` 則停止定時校正，仍可執行開機／手動 FFC，並持續套用已有的有效補償表。

FFC 參考表會追蹤 NUC／TDC 開關與校正表版本，以及手動校正版本。上游變更後，舊 FFC 表先失效，不會套用到不同的校正輸出；FFC 開啟時由 RAW worker 重建一次，即使 `shutter_interval_sec=0` 也會執行。這不會修改 CFG 的定時間隔或補償開關。當張 NUC 準備失敗而略過 NUC 時，也略過依賴 NUC 的 FFC。

一般 FFC 的四張平均與 shading 的 32 張平均均使用 CPU `UINT32` 累加，避免 NUC／TDC 校正後的 UINT16 值超過 `16383` 時溢位。四張平均維持整數除法向下取整；32 張維持四捨五入。兩種取像期間都鎖住上游校正，溫度表只在開始前更新一次。640x512 累加暫存為 1,310,720 bytes，640x480 為 1,228,800 bytes，完成或失敗時釋放；原本的 16-bit GFX 累加函式與 zero buffer 已移除。

只要開機校正一次、之後持續補償，可設定：

```ini
ffc_correction=1
shutter_interval_sec=0
shutter_close_wait_ms=200
ffc_startup_enable=1
```

間隔大於 `0` 時的定時流程：

```text
interval 時間到
  -> 關閉 shutter
  -> 等待 shutter_close_wait_ms
  -> 清除排隊影格，再丟棄四張新過渡影格
  -> 取得四張 TDC／NUC 校正後、FFC 前的 RAW，以 UINT32 平均並更新 offset table
  -> 開啟 shutter
  -> 等待 shutter_close_wait_ms，清除排隊影格並丟棄四張新過渡影格
  -> 安排下一次 shutter cycle
```

有效區目前為 `640 x 512`，位於完整 RAW frame 的 active window。offset 公式：

```text
FFC_B(x,y) = shutter_raw(x,y) - shutter_mean
```

offset 以 signed 16-bit 二補數存放，直接寫入 `thermal_ffc_b_frame` 對應的 DDR buffer，然後呼叫 `hd_common_mem_flush_cache()`。它不會保存成檔案，也不需要 reload table。

FFC runtime 使用固定為 `1.0` 的 Q8 K table。`ffc_apply()` 使用：

```text
VENDOR_GRPH_CMD_Non_uni_correction
gain shift = 8

Image A = input RAW
Image B = FFC K table（固定 256，即 Q8 的 1.0）
Image C = FFC output
Image D = FFC offset table

Image C = (Image B * (Image A - signed(Image D))) >> 8
        = Image A - signed(Image D)
```

上板驗證應包含負 offset，例如：

```text
input           = 2000
B               = 10
expected output = 1990

input           = 2000
B               = -10
expected output = 2010
```

### 黑體／快門差異補償（Shading）

補償均勻黑體與內部快門之間的固定空間差異。預設 `ffc_shading_enable=0`，關閉時使用普通 FFC。此功能合併到 FFC 的 B 表，使用原本 unity K 與 GFX，需要 FFC 開啟且有有效 FFC 表，不增加逐幀 GFX pass。

使用步驟：

1. 完成 BPR／NUC 校正，開啟 NUC、關閉 Blackbody Dynamic NUC，相機與鏡頭先達到熱穩定。
2. 均勻黑體填滿整個視野，不含爐口邊緣或背景，保持相機不動。
3. 主選單 `5. FFC` → `2. Calibrate blackbody / shutter shading (32 frames each)`，確認就緒後輸入 `1`。自動開快門取黑體 32 張平均，再關快門取 32 張平均，最後重新開快門。兩組均取 TDC（若開啟）／NUC 後、FFC 前的 RAW，期間固定 NUC／TDC 溫度表。使用 UINT32 CPU 累加，避免 16-bit 累加溢位。
4. 顯示 `PASS` 後，進入 `CFG Setting` → `Shutter / FFC setting` → `5. Blackbody / shutter shading on/off` 開啟，開關保存為 `ffc_shading_enable=1`，下一張 FFC 輸出套用。校正成功本身不改開關。
5. 對準均勻黑體，切換 Shading 開／關，比較 corrected RAW 的中心／邊緣差異。

```text
O = 預先拍攝的黑體平均 RAW
S = 同次拍攝的關快門平均 RAW
C = (O - mean(O)) - (S - mean(S))

每次 FFC：
B_base = 當次關快門平均 RAW - mean(當次關快門平均 RAW)
B_final = B_base + C  （Shading 開啟且條件有效時）
output = input - B_final
```

各自扣除全畫面平均，避免把黑體／快門的整體溫差寫入 C。每次由未合併的 FFC base 重建，避免重複累加 C；開關或校正改變時重置 Weighted Mapping 的 LUT 歷史。

差異表保存於 `/mnt/sd/thermal_op/ffc_shading.bin`，包含 32-byte header（magic、版本、sensor profile、寬高、NUC shift、校正時 TDC 開關、checksum）及全畫面 signed INT16 C，皆為 little-endian。TY640 為 655,392 bytes，ATTO640 為 614,432 bytes。先寫 `.new`、flush/fsync，再 rename 替換；計算或存檔失敗保留前次差異表。Liveview 啟動時驗證 header、長度、checksum 後載入；一般 FFC base 仍只存在 RAM。

差異表無效、NUC 關閉、Dynamic NUC 開啟、TDC 開關與校正時不同，或合併 B 超出 signed INT16 範圍時，使用普通 FFC。選單分別顯示 requested、applied 與 table。取像或快門失敗時中止校正並嘗試開回快門。

此表代表當時鏡頭、NUC 校正與熱狀態的差異。重新校正或更換 NUC、BPR、鏡頭後應關閉 Shading 並重新校正；檔案不會自動辨識 NUC table 內容變更。單張固定表不保證涵蓋所有工作溫度。

Host regression（實際 SDK headers、模擬 GFX，非板端驗證）：

```sh
python tests/test_ffc_shading.py --sdk <SDK/code>
```

涵蓋 TY640／ATTO640 的正負號、去平均、重複 FFC 不累加、開關、NUC／TDC mode gate、存檔重載及毀損檔案拒絕；另擷取實際 RAW-worker 校正函式，模擬 32＋32 張取像、NUC 輸出來源、寬位元累加，以及取像／map／快門失敗後的影格釋放與快門恢復。仍需 SDK 編譯與板端驗證。


## VideoProc / ISP

主選單選擇 `9. VideoProc / ISP`，或選擇 `CFG Setting -> VideoProc / ISP setting`：

```text
VideoProc / ISP setting
 1. AF on/off
 2. WDR on/off
 3. DEFOG on/off
 4. 3DNR + 3DNR statistics on/off
 5. All on
 6. All off
 7. Gamma on/off
 0. Back
```

個別項目會要求明確輸入 `0: off` 或 `1: on`；`All on`／`All off` 會將四個 CFG 欄位一次交易式寫入。選單同時顯示目前 runtime 與 pending CFG，設定必須執行 `Liveview Restart` 才套用。3DNR 關閉後不會建立、設定或啟動 `HD_VIDEOPROC_0_OUT_4` reference path；開啟時會同時加入 `HD_VIDEOPROC_FUNC_3DNR` 與 `HD_VIDEOPROC_FUNC_3DNR_STA`。

## Gamma

Gamma 入口整合至 `VideoProc / ISP -> Gamma on/off`。完整 Gamma
參數由 Liveview 啟動時載入的 `/mnt/sd/isp_ty640_0.cfg` 提供，選單只修改
目前 ISP runtime 的 `enable`，不修改 mode、auto selection 或 LUT：

```text
Gamma
 1. Gamma on/off
 0. Back
```

選擇 `Gamma on/off` 後必須明確輸入 `0: off` 或 `1: on`，並立即透過
`IQT_ITEM_GAMMA_PARAM` 套用到目前 ISP。程式不會反向寫入 vendor IQ cfg；
Liveview Restart 或重新開機後，Gamma enable 會再次以
`/mnt/sd/isp_ty640_0.cfg` 的內容為準。原本的 `gamma.bin`、mode 與
auto selection 應用層覆蓋功能已移除。

## Shutter GPIO

Shutter 使用 Linux GPIO character-device API：

```text
gpiochip : /dev/gpiochip0
line     : P_GPIO(0)
low      : open shutter
high     : close shutter
```

GPIO 初始化後輸出為 low，讓 shutter 維持開啟。開啟或關閉 shutter 失敗時都會重新初始化 GPIO 並最多嘗試三次；三次仍失敗會設定 `shutter_fault`，並由外部 worker 停止 Liveview。整段 FFC shutter-closed calibration 會持有 calibration lock，期間所有手動 shutter 指令都會被阻擋。

## Runtime 檔案總表

| 路徑 | 用途 | 產生／載入時機 |
|---|---|---|
| `/mnt/sd/tiffany_thermal.cfg` | 使用者設定 | 設定時立即保存；Liveview Start／Restart 或開機時載入 |
| `/mnt/sd/isp_ty640_0.cfg` | ISP cfg，包含 Gamma 完整參數與預設 enable | Liveview 啟動時載入；Gamma menu 只暫時修改目前 runtime enable |
| `/mnt/sd/thermal_op/dpc_table.bin` | BPR/DPC table | Liveview 啟動時由 Tiffany 直接載入 ISP；Bright/Dark BPR 產生新表後，也可直接更新 ISP |
| `/mnt/sd/thermal_op/sensor_temp/point1_K.bin` ... `point5_K.bin` | NUC gain tables | Sensor Temperature 五點校正產生；依 Sensor 溫度選用 |
| `/mnt/sd/thermal_op/sensor_temp/point1_B.bin` ... `point5_B.bin` | NUC offset tables | Sensor Temperature 五點校正產生；依 Sensor 溫度選用 |
| FFC DDR buffer | FFC offset table | shutter frame 更新；不寫檔 |

## 建議上板檢查

1. 啟動後確認 `Liveview: running`。
2. 確認 `/mnt/sd/tiffany_thermal.cfg` 是否成功載入或建立。
3. 若啟用 NUC，確認 log 出現 Sensor Temperature K/B table loaded；否則 NUC 會保持 off。
4. 若啟用 FFC，確認依序出現 shutter close、`FFC dark frame mean`、`FFC table updated`、shutter open。
5. 驗證 FFC signed offset，至少確認 `2000 + (-10) = 1990`。
6. BPR calibration 後確認 `/mnt/sd/thermal_op/dpc_table.bin` 為 16384 bytes，並執行 reload BPR table。
7. 停止或離開程式時確認 `raw_pipeline_thread` 正常 join，並檢查 ISP/HDAL cleanup log。

## 已知注意事項

- `Pipeline/liveview.c` 才是目前整合版 liveview owner；不要誤改根目錄舊版 sample。
- NUC 與 FFC 是兩次獨立 GFX pass：`VCAP -> NUC -> FFC -> VideoProc`。
- BPR 的 `dpc_table.bin` 屬於 ISP DPC；NUC/FFC table 屬於 thermal RAW 校正，三者用途不同。
- FFC 的 `Non_uni_correction` signed offset 行為仍應以實機測試確認。
- 命令列 `?` help 的參數順序目前和 source 實際解析順序不一致。
- `hd_gfx_init()`／`hd_gfx_uninit()` 已納入 liveview start/stop；shutter GPIO fd 目前仍沒有公開的 deinit/close 流程。
"# Novatek_539_Thermal_Camera" 


## Image Count Recording 溫度紀錄與三段測試

進入 `Liveview -> Recording RAW -> Image Count Recording`，在 `Select RAW type` 選擇：

- `1. Original RAW`、`2. Corrected RAW`、`3. AGC output RAW`：各保存指定張數。
- `4. Original -> Corrected -> Original (BPR off)`：依序保存 N 張 Original、N 張 Corrected，再關閉 BPR、排除切換前的影格後保存 N 張 Original。總共 3N 張；前兩段沿用錄製前的 BPR 狀態。

每張均在同目錄的 `summary.csv` 記錄時間、sensor 溫度、BPR 開關、階段及 RAW 檔名。時間取板端系統時鐘，以 UTC 毫秒格式保存；溫度讀取失敗時留白並標記 `read_failed`。第三段檔名使用 `original_bpr_off`。

三段測試完成、手動停止、Liveview 停止或存檔失敗後，會在最後一張排程檔案寫完後恢復錄製前的 BPR 狀態，不改寫 BPR CFG。ISP 恢復失敗會顯示錯誤並保留待恢復狀態。錄製期間請先停止三段測試，再從 CFG 改變 BPR。

主機回歸測試：`python tests/test_image_count_recording.py <gcc 路徑>`。實際 BPR 切換後的影像與 SD 寫入仍需上板驗證。

### RAW Preprocessing

`CFG Setting -> 14. RAW Preprocessing setting` 提供 CPU RAW 時域降噪的開關、差異門檻、平均強度與重設選項。
流程為 `TDC -> NUC -> FFC -> RAW temporal denoise -> corrected RAW snapshot -> CPU AGC -> RAWALL -> YUV420`。
RAW 輸入使用原本的 RAWALL 路徑，不再經過獨立的 RAW14→U12 轉換與緩衝區。

### RAW14 時間降噪（CPU，AGC 前）

處理順序為 `TDC -> NUC -> FFC -> RAW temporal denoise -> Corrected RAW / CPU AGC`。
Original RAW 與校正取像維持未經此濾波的資料；開啟後錄下的 **Corrected RAW 包含時間降噪結果**。
此功能預設關閉，可在 `CFG Setting -> 14. RAW Preprocessing setting` 即時開關，不需重啟。

```ini
raw_temporal_denoise_enable=0
raw_temporal_denoise_threshold_dn=12
raw_temporal_denoise_history_shift=2
```

`history_shift` 為 1、2、3 時，前次濾波結果的權重分別為 50%、75%、87.5%。
與前次濾波結果的差異不超過門檻時才混合，否則直接使用目前像素。
中等候選設定為 12 DN / 2；小幅真實移動仍可能被平均掉，必須檢查拖影。
內部使用 UINT32 Q8 歷史，輸出保留原生 RAW DN，不做 RAW shift、遮罩、截斷或空間模糊。
感測器名義值域為 RAW14，但 NUC／FFC 校正後可能超過 `16383`；時間降噪接受完整 UINT16 `0..65535`，不再因單一校正像素超過 `16383` 而停止 Liveview。`12 DN` 仍代表原生 DN 差值，平均權重不變；後續 AGC 的 RAW14 映射規則也不變。
參數變更、停用、重啟、FFC/校正狀態變更、時間戳倒退/重複或中斷超過 250 ms 會重設歷史。

新增一個 RAW 輸出 common-pool block；啟用後才配置 Q8 歷史（640x480 約 1.2 MB），停止時釋放。
已移除第一張及每 300 張的週期耗時輸出與計時累積，不影響 RAW temporal denoise 濾波。
第一張另印輸入格式、尺寸、stride、DN 最小／最大值及大於 `16383` 的像素數。失敗時會在通用停止訊息前，區分輸出 buffer、輸入格式／尺寸、RAW 複製、參數／layout、歷史記憶體配置或 cache flush 錯誤；格式與記憶體錯誤仍安全停止，不假裝已完成濾波。
主機數值與 CFG 測試不能證明 SDK 建置、板端 FPS/CPU 使用率或無拖影。


### DDE_1106：8-bit 顯示端整合

`Enhancement/DDE/dde_dt_opt.c` 採用同事 `DDE_1106` 的新核心，取代舊核心；
同事提供的 `liveview.c` 僅作為參考，未覆蓋現有 pipeline。
處理位置保持 `VideoProc → 選用 VPE mirror → CLAHE → DDE → 偽色彩 → 編碼/UVC`。
DDE 直接讀寫 YUV420 的 8-bit Y plane（0..255），stride 單位是 byte；
UV、行尾 padding 與上游 RAW／AGC 資料不經 DDE 改寫，也不先轉成 RAW14。

新核心保留 `DDE_DIR_MODE=1`、`DDE_DIR_PAIRED_SIGMA=1`、`DDE_STRIP_FUSE=1`：
每次 pass 交替正向／反向，相鄰兩次共用 sigma／係數 LUT，條帶融合降低整幀讀寫。
`dde_num_iters` 仍接受 1..8，但現在代表交替方向的 pass 數，並非舊版每次都做雙向；
奇數次包含一個未配對方向，與舊核心的相同次數不保證相同視覺效果。
差值 LUT 依 Y8 改為 256 項，`DDE_USING_MAPPING=0`，不額外做百分位拉伸。

沿用既有 CFG 與 DDE 選單，修改後於下一個顯示影格套用：
`dde_enable`、`dde_sigma_s`（pixels）、`dde_sigma_r`（Y8 DN）、
`dde_gain_percent`（100=原圖）、`dde_num_iters`、`dde_base_scale`。
初始預設為關閉、8/16/150%/2；
DDE 獨立選單的 Standard/Fast preset 為 21/51/200%，pass 數為 2/1。
本機 `Config/tiffany_thermal.cfg` 與 Simple display Standard mode 均改為 2 次 pass；
S、R、Gain 保留 21、51、200%。板端既有 CFG 若仍為 1，須改成 `dde_num_iters=2` 才會執行正反兩趟。

工作區依解析度配置，顯示 worker 結束後釋放；預設融合路徑
640×480 為 1,293,664 bytes，640×512 為 1,379,840 bytes。
主機驗證命令：

```text
python tests/test_dde_1106.py --reference ../DDE_1106
python tests/test_dde_frame.py
```

數值測試涵蓋原公式 baseline 與原始同事核心的精確對照、簡化公式的輸出誤差、獨立遞迴公式、
融合與非融合一致性、小圖／非整條帶寬度、640×480／640×512、參數拒絕及工作區邊界。
HDAL mock 測試使用正式影格封裝，檢查 Y8、UV／padding、開關、尺寸變更與錯誤清理。
以上是 host 驗證；Novatek SDK 編譯、板端 FPS、畫質及快取／UVC 實機行為仍待確認。

遞迴公式現在預設啟用 `DDE_RECUR_LERP=1`：
`s + a * (previous - s)`，減少相較原公式 `(1-a)*s + a*previous` 的乘法數。
融合與非融合路徑均共用此設定，沒有略過反向 pass、降採樣或重用前幀影像。
沒有啟用 `fast-math`；浮點捨入順序改變，因此不保證與原公式逐像素相同。
目前 300 組主機測試中，比較 2,805,980 個像素，18 個像素不同、最大差異 1 Gray。
這是測試集合內的結果，不是對所有影像與編譯器的誤差上界保證。
可用 `-DDDE_RECUR_LERP=0` 編譯原公式的 A/B 版本。

主機核心 A/B 測速：

```text
python tests/test_dde_1106.py --reference ../DDE_1106 --benchmark --report tmp/dde_simd4_two_pass_report.json
```

報告記錄原公式 1／2 次、scalar 簡化公式 2 次與 SIMD4 2 次的 median 核心耗時、來源 SHA256 與編譯器；
不含影像重設複製、HDAL map/cache、編碼及 UVC，不能換算成板端 FPS 保證。

針對板端回報 1 pass 約 30 FPS、2 passes 約 15 FPS，新增 `DDE_SIMD4`：
SDK 啟用 ARM NEON／AArch64 時自動選用四路向量運算，其他平台預設 scalar。
四路 lane 分別處理獨立的列／欄，保留各鏈上的正反向遞迴順序；
最後的細節重組、限幅與整數轉換也使用四路運算，邊界不足四項時走 scalar。
採用 GNU vector extensions；不新增跳幀、降採樣或前幀影像重用。
可用 `-DDDE_SIMD4=0` 建立上一輪 scalar 簡化公式版本以便板端 A/B。
Host 測試強制執行相同四路程式，300 組輸出與 scalar 逐像素一致；
x86 測速沒有呈現改善，不能當成 NEON 效能證據或 30 FPS 保證。


半解析度 Base 實驗模式：`CFG → 10. DDE setting → 8. Base resolution`，
`1` 為全解析度，`2` 為 Base 的寬／高各一半。CFG 為 `dde_base_scale=1/2`，
預設與本機 CFG 保持 `1`；Simple display Standard 亦固定回到全解析度。
既有 CFG 缺少此鍵時，初始 runtime 預設仍為全解析度。
選單修改會儲存並於下一個顯示影格套用；無額外耗時統計或重設統計選項。

模式 2 以 2×2 box average 產生 Y8 guide，奇數邊界使用實際存在的像素平均。
Sigma S 依工作解析度除以 2（原參數仍以全尺寸像素表示），Sigma R 與 Gain 不變。
每幀仍完成所設的正反方向 passes；Base 保留 float 精度，依 box 的實際中心做
雙線性放大，再計算 `Base + Gain × (原尺寸 Y8 − Base)`，最後才限幅／轉回 Y8。
重用兩列已水平放大的 Base，減少重複插值。沒有跳幀、前幀影像重用或最終輸出縮小。
這是近似的另一種細節分離方式，並不保證與全解析度畫質相同；
尤其高反差邊緣、小熱點與細線須比較光暈及細節變化。

所需工作區：640×480 全解析度 1,293,664 bytes、半解析度 435,504 bytes；
640×512 分別為 1,379,840／463,616 bytes。切換模式時工作區重用，已配置容量保留至 Stop。
半解析度模式需要既有 fused/no-mapping build；診斷用的非融合／mapping build 會拒絕模式 2。

`python tests/test_dde_half_base.py` 驗證獨立 NumPy 參考、float Base、奇數邊界、
gain=1 identity、SIMD/scalar 一致性、舊 full 模式、padding、工作區 guard 及無效輸入。
加 `--output-dir <資料夾> --image <PNG>` 可重播既有 Y8 PNG、產生比較圖與 `analysis.json`；
重複 `--image` 可一次處理多張。圖像來源若已經過 DDE／ISP，這只是再次離線處理的比較，
不能視為實機 DDE 前輸入、感測器品質或板端 FPS 證據。

### Simple display setting：Standard mode

`CFG Setting → 2. Simple display setting` 只保留 `1. Standard mode` 與 `0. Back`。
已移除此選單的 Fast mode 與 AGC only；CLAHE／DDE 各自設定選單維持原功能。
Standard mode 採用 `01_Report/20261111/tiffany_thermal.cfg` 本次讀取的設定快照：

| 項目 | Standard mode 設定 |
|---|---|
| AGC | Weighted Mapping 開啟，其餘兩種 AGC 關閉 |
| Weighted Mapping | Clip 2.0、Redistribution 1、Bright Weight 2、P80、Gain Limit 0.5、Smooth 50% |
| CLAHE | 關閉，Tile 32×32 pixels、Clip 4 |
| DDE | 開啟，Sigma S 21、Sigma R 51、Gain 200%、Directional passes 2 |

一併對齊該 CFG 中未啟用的 Histogram／Fieldscale 參數：Histogram 為 P1/P99、
Gain Limit 1.0、Smooth 90%；Fieldscale 為 grid 8、LES distance 2、threshold 800、
message distance 1、iterations 1、Smooth 90%。
相對原始 CFG 快照，DDE pass 數因本次兩趟優化需求改成 2，其餘值維持上述設定。
此預設組合不變更 RAW temporal denoise、NUC／FFC、ISP、FPS 或輸出設定。
按下 Standard 後先儲存 `/mnt/sd/tiffany_thermal.cfg`，成功後套用 AGC／CLAHE／DDE；
寫檔失敗保留先前設定。這是程式內固定快照，板端不會讀取電腦上的報告路徑。

`tests/fixtures/standard_display.cfg` 保存對照值。
`python tests/test_weighted_config.py --sdk ../ns2501_linux_sdk-main/code`
驗證兩種感測器的實際 Standard 選單、CFG 儲存重載、runtime 套用與失敗回復；
可加 `--standard-cfg <原始 CFG 路徑>` 直接與指定檔案比對。
SDK 編譯與板端效果仍待驗證。

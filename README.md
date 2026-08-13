# Tiffany Thermal

`Tiffany_thermal` 是 Novatek NS02302 Linux 平台上的 thermal sensor 整合程式，包含：

- Thermal liveview
- BPR／ISP DPC 壞點校正
- NUC 兩點式非均勻性校正
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
├─ Algorithm/
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

`Algorithm/BPR/bpr.c` 只提供 BPR 功能，ISP lifecycle 由 `Pipeline/liveview.c` 統一管理。

目前 Windows checkout 沒有 SDK `make` 環境，最終編譯與上板測試需在 Novatek SDK Linux 環境執行。

## 執行

不帶參數時使用程式預設值：

```sh
./Tiffany_thermal
```

目前 source 實際解析的參數順序為：

```sh
./Tiffany_thermal <sensor>
```

預設值：

```text
sensor                     = SEN_SEL_TY_640
VCAP ID                    = 4
fps cfg fallback           = 30
startup skip cfg fallback  = 30 frames
```

FPS 與啟動時跳過的 frame 數量不接受命令列參數，分別由 `/mnt/sd/tiffany_thermal.cfg` 的 `fps=` 與 `startup_skip_frames=` 載入。若 cfg 不存在、沒有對應欄位，或 `cfg_boot_load=0`，則兩者都使用程式預設值 `30`。

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
 7. Gamma
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
 5. Show current RAW statistics
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

自動 FFC 或 Runtime BPR shutter-closed calibration 執行期間，`Shutter open` 與 `Shutter close` 指令會立即被拒絕，不會等待校正結束後再執行。

`Snapshot RAW` 會在下一張正常 frame 完成 NUC／FFC 後寫入 `/mnt/sd/`。檔案只保留有效影像區 `640x512`，每個 pixel 為 little-endian 16-bit container：

- NUC／FFC 都未套用：`snapshot_NNNN_raw14_640x512.raw`
- 有套用 NUC 或 FFC：同時保存 `snapshot_NNNN_input_raw14_640x512.raw` 與 `snapshot_NNNN_corrected_raw16_640x512.raw`

每個檔案大小為 `640 * 512 * 2 = 655360 bytes`。寫檔由 RAW pipeline thread 同步完成，因此快照當下可能短暫影響即時輸出幀率。

`Show current RAW statistics` 會要求 RAW pipeline 對下一張 VCAP 輸入 frame 計算並顯示格式、尺寸、stride、最低值、最高值、平均值與 pixel 數量。統計對象是套用 TDC／NUC／FFC 前的 Sensor RAW14；選單執行緒不會自行 pull 或 release VCAP buffer。

即時 RAW pipeline：

```text
VCAP RAW
  -> optional NUC
  -> optional FFC
  -> VideoProc
  -> VideoOut
```

NUC 與 FFC 都關閉時，VCAP frame 直接送入 VideoProc。任一校正啟用時，capture loop 會使用校正後的 output frame，並依 buffer ownership 正確釋放原始 VCAP frame。

## CFG Setting 選單

主選單選擇 `2. CFG Setting` 後會顯示：

```text
CFG Setting
 1. Liveview setting
 2. BPR setting
 3. Shutter / FFC setting
 4. NUC setting
 5. TDC setting
 6. Gamma setting
 7. Show current tiffany_thermal.cfg
 8. Generate tiffany_thermal.cfg
 9. Use CFG on next boot/restart on/off
 0. Back
pending CFG: boot load on/off
```

所有手動調整的持久化參數都集中在這裡。各分類會同時顯示 runtime 與 pending CFG；修改時只寫入 `/mnt/sd/tiffany_thermal.cfg`，必須 Liveview Restart 或重新開機才套用。所有 on/off 項目都會再要求明確輸入 `0: off` 或 `1: on`，不會直接反轉目前設定。

- `Liveview setting`：FPS、startup skip frames、runtime skip frames。
- `BPR setting`：BPR enable、Bright/Dark threshold、Runtime BPR enable 與 threshold。
- `Shutter / FFC setting`：FFC enable、FFC/Runtime BPR 共用 shutter interval、shutter close wait time。
- `NUC setting`：NUC enable 與 Sensor Temperature table interval。五點 Sensor 溫度由 NUC calibration 自動寫入，不提供手動修改。
- `TDC setting`：TDC enable 與 update interval。
- `Gamma setting`：Gamma enable、Gamma bin、mode 與 auto selection。
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
runtime_bpr_enable=0
runtime_bpr_threshold=15
ffc_correction=1
shutter_interval_sec=60
shutter_close_wait_ms=200
tdc_correction=1
tdc_interval_sec=60
gamma_correction=1
gamma_mode=0
gamma_auto_sel=2
gamma_load_bin=0
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
| `bpr_threshold` | BPR calibration threshold，預設 `15%` |
| `runtime_bpr_enable` | Runtime BPR on/off，預設 `0`；可獨立依共用 shutter interval 執行 |
| `runtime_bpr_threshold` | Runtime BPR calibration threshold，預設 `15%` |
| `ffc_correction` | FFC runtime on/off，程式預設為 `1`；啟動後等待第一個 interval 到期才校正 |
| `shutter_interval_sec` | FFC 與 Runtime BPR 共用的關快門間隔，接受任意大於 `0` 的秒數，預設 `60` |
| `shutter_close_wait_ms` | 快門關閉後開始 FFC／Runtime BPR 前的等待時間，預設 `200` ms |
| `tdc_correction` | TDC runtime on/off |
| `tdc_interval_sec` | TDC 更新間隔 |
| `gamma_correction` | Gamma on/off |
| `gamma_mode` | Gamma mode |
| `gamma_auto_sel` | Gamma auto set 選擇 |
| `gamma_load_bin` | 是否載入自訂 `gamma.bin` |

如果檔案不存在，程式會建立預設 cfg。任何設定修改只寫入 cfg；使用者必須執行 `Liveview Restart` 或重新開機才會套用。

## BPR／DPC

BPR 使用 Novatek DPC library 找出 bright/dark bad pixels，產生 ISP runtime 使用的 DPC table。

固定路徑：

```text
ISP cfg : /mnt/app/isp/isp_rtd6122c_0.cfg
DPC bin : /mnt/app/isp/dpc_table.bin
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
- `Reload BPR table`：重新載入 ISP cfg，使 cfg 內指定的 DPC table 生效。
- `BPR debug on/off`：控制 DPC library log 與 `/mnt/sd/` RAW dump。

BPR/Runtime BPR enable 與 threshold 已集中到 `CFG Setting -> BPR setting`；共用 shutter interval 位於 `CFG Setting -> Shutter / FFC setting`。

手動 `Bright/Dark BPR calibration` 開始前會先等待正在執行的自動 shutter cycle 完成，接著暫停 interval 關快門；成功或失敗離開時都會恢復，並從完整的 `shutter_interval_sec` 重新計時。`Runtime BPR` 不會暫停 interval，因為它本身就是共用 shutter cycle 的一部分。

BPR calibration 產生 table；runtime 是否補點則由 `bpr_correction` 控制。RAW16 每個 pixel 以 2 bytes 儲存；在 DPC table 產生流程中會取 RAW16 的 bit 15..8 轉成 RAW8，供 calibration library 使用，但 liveview runtime RAW pipeline 仍維持 RAW16，並不因此改成 RAW8。

## NUC

NUC 是兩點式非均勻性校正。校正時依序取得 low-temperature 與 high-temperature blackbody RAW，計算每個 pixel 的 K/B table。

Sensor Temperature 五點表路徑：

```text
K table : /mnt/sd/thermal_op/sensor_temp/point1_K.bin ... point5_K.bin
B table : /mnt/sd/thermal_op/sensor_temp/point1_B.bin ... point5_B.bin
```

NUC 選單：

```text
NUC menu
 1. 5 point Sensor Temperature NUC calibration
 2. Reload Sensor Temperature K/B table
 0. Back
```

NUC enable 與 Sensor Temperature table interval 已集中到 `CFG Setting -> NUC setting`。

`5 point Sensor Temperature NUC calibration` 量測五個 Sensor 溫度點。每個溫度點的 Low 與 High 都連續擷取 4 張 RAW，由 CPU 對 `640x512` 做平均，再產生獨立的 K/B table；runtime 依目前 Sensor 溫度選擇最接近的 table。完整 K/B table 為 `640x512`，對應板端 VCAP 實際回傳的 active-only RAW frame。校正過程先將五點 K/B table 寫入 `.new` 暫存檔，全部成功且大小正確後才替換正式檔案；任一點失敗都會放棄本次結果，保留原本五點 table。啟用 NUC 前，point1 到 point5 的 K/B 檔案必須全部存在且大小正確。

NUC 校正開始前會先等待正在執行的自動 shutter cycle 完成；接著暫停 interval 觸發的 FFC／Runtime BPR 關快門，讓 Low／High RAW 擷取期間保持快門開啟。校正成功或失敗離開時都會恢復自動 shutter cycle，並從完整的 `shutter_interval_sec` 重新計時。

K 使用 Q8 格式：

```text
NUC_GAIN_SHIFT = 8
K = stored_K / 256
```

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

output = (stored_K * (input - new_B)) >> NUC_GAIN_SHIFT
```

啟動時會嘗試載入 `K.bin` 與 `B.bin`。任一檔案不存在或大小不符時，NUC table 會標記為未載入，並保持 NUC off。執行 calibration 或 reload 成功後才可啟用。

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
 0. Back
```

FFC enable、共用 shutter interval 與 shutter close wait time 已集中到 `CFG Setting -> Shutter / FFC setting`。

FFC 或 Runtime BPR 任一功能啟用後，會先等待一個完整的 `shutter_interval_sec`，再執行第一次共用 shutter cycle；之後依相同 interval 定期執行：

```text
interval 時間到
  -> 關閉 shutter
  -> 等待 shutter_close_wait_ms
  -> FFC 啟用時取得四張 VCAP RAW 並更新 offset table
  -> Runtime BPR 啟用時執行 BPR calibration 並 reload ISP cfg
  -> 開啟 shutter
  -> 安排下一次 shutter cycle
```

有效區目前為 `640 x 512`，位於完整 RAW frame 的 active window。offset 公式：

```text
FFC_offset(x,y) = shutter_mean - shutter_raw(x,y)
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

Image C = (Image A * Image B >> 8) + Image D
        = Image A + Image D
```

上板驗證應包含負 offset，例如：

```text
input          = 2000
offset         = -10
expected output = 1990
```

## Gamma

Gamma 選單只保留 runtime 狀態：

```text
Gamma status
 0. Back
```

Gamma enable、是否使用 `gamma.bin`、mode 與 auto selection 都集中到 `CFG Setting -> Gamma setting`。設定寫入 cfg 後，必須 Liveview Restart 或重新開機才套用。

## Shutter GPIO

Shutter 使用 Linux GPIO character-device API：

```text
gpiochip : /dev/gpiochip0
line     : P_GPIO(0)
low      : open shutter
high     : close shutter
```

GPIO 初始化後輸出為 low，讓 shutter 維持開啟。開啟或關閉 shutter 失敗時都會重新初始化 GPIO 並最多嘗試三次；三次仍失敗會設定 `shutter_fault`，並由外部 worker 停止 Liveview。FFC 與 Runtime BPR 共用同一個自動 shutter cycle；整段 shutter-closed calibration 會持有 calibration lock，期間所有手動 shutter 指令都會被阻擋。

## Runtime 檔案總表

| 路徑 | 用途 | 產生／載入時機 |
|---|---|---|
| `/mnt/sd/tiffany_thermal.cfg` | 使用者設定 | 設定時立即保存；Liveview Start／Restart 或開機時載入 |
| `/mnt/app/isp/isp_rtd6122c_0.cfg` | ISP cfg | Liveview/BPR reload |
| `/mnt/app/isp/dpc_table.bin` | BPR/DPC table | Bright/Dark BPR 或共用 shutter cycle 內的 Runtime BPR 產生；ISP cfg 載入 |
| `/mnt/sd/thermal_op/sensor_temp/point1_K.bin` ... `point5_K.bin` | NUC gain tables | Sensor Temperature 五點校正產生；依 Sensor 溫度選用 |
| `/mnt/sd/thermal_op/sensor_temp/point1_B.bin` ... `point5_B.bin` | NUC offset tables | Sensor Temperature 五點校正產生；依 Sensor 溫度選用 |
| FFC DDR buffer | FFC offset table | shutter frame 更新；不寫檔 |

## 建議上板檢查

1. 啟動後確認 `Liveview: running`。
2. 確認 `/mnt/sd/tiffany_thermal.cfg` 是否成功載入或建立。
3. 若啟用 NUC，確認 log 出現 Sensor Temperature K/B table loaded；否則 NUC 會保持 off。
4. 若啟用 FFC，確認依序出現 shutter close、`FFC dark frame mean`、`FFC table updated`、shutter open。
5. 驗證 FFC signed offset，至少確認 `2000 + (-10) = 1990`。
6. BPR calibration 後確認 `/mnt/app/isp/dpc_table.bin` 存在，並執行 reload BPR table。
7. 停止或離開程式時確認 `raw_pipeline_thread` 正常 join，並檢查 ISP/HDAL cleanup log。

## 已知注意事項

- `Pipeline/liveview.c` 才是目前整合版 liveview owner；不要誤改根目錄舊版 sample。
- NUC 與 FFC 是兩次獨立 GFX pass：`VCAP -> NUC -> FFC -> VideoProc`。
- BPR 的 `dpc_table.bin` 屬於 ISP DPC；NUC/FFC table 屬於 thermal RAW 校正，三者用途不同。
- FFC 的 `Non_uni_correction` signed offset 行為仍應以實機測試確認。
- 命令列 `?` help 的參數順序目前和 source 實際解析順序不一致。
- `hd_gfx_init()`／`hd_gfx_uninit()` 已納入 liveview start/stop；shutter GPIO fd 目前仍沒有公開的 deinit/close 流程。
"# Novatek_539_Thermal_Camera" 

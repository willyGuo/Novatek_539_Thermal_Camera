# 539A 獨立 IFE／2DNR RAW14 精度測試

目的：確認 `RAW14 數值 → RAW16 LSB → IFE → RAW16 LSB` 是否保留每 1 DN 差異，再測 2DNR。此目錄不加入 Tiffany 的 Makefile，不改正式 NUC／FFC／AGC 管線。

目前狀態：已通過 Windows GCC 主機設定檢查及分析工具測試；**尚未編譯 Linux kernel module、連結配套 SDK、部署或取得板端輸出**。這是一份待 SDK／板端驗證的診斷實作，不能視為已確認硬體支援 RAW14。

## 測試設定

- 配套：`original_SDK_ATTO640/atto640_261012/code` 的 539A 驅動。
- 固定 640×480、每像素兩個 byte、小端序；輸入和輸出為 `RAW16 | LSB | RGGB_R`。
- RGGB 標記用於 IFE 格式介面；輸入內容是單色數值，NR 使用相同的各色通道預設參數，關閉 RGB-IR fill。
- 使用標準 RAW D2D，`thermal_mode_en=0`；thermal mode 在驅動中要求 F-curve，因此不適合作為第一階段的無映射精度測試。
- 旁路測試關閉 filter、F-curve、WDR、gain、vignette 等可改動 DN 的功能；不做右移、左移、normalize 或自動補償。
- NR 開啟時，filter 參數在建置時從配套 SDK 的 `iq_539a/iq_flow/iq_common_param.c::ctl_ife_filter_init` 產生，記錄來源 SHA256。僅覆寫 enable、motion map off、RGB-IR fill off。這是診斷基準，**不是現有 ISP CFG 的調校值**。

## SDK 與 kernel 建置

IFE 的 `kdrv_ife_open/close/set` 在這份原始碼沒有 `EXPORT_SYMBOL`。外部診斷模組需要先套用專用補丁，再重建含 IFE 的配套媒體驅動及其 `Module.symvers`。補丁不改處理演算法，只匯出三個函式，驗證結束後可以移除。未對 Windows SDK 自動套用補丁。

在使用相同 kernel／SDK 的 Linux 建置環境中：

```sh
cd /path/to/atto640_261012/code
git apply --check /path/to/Tiffany_thermal/sdk_patches/0002-nt98539a-ife-probe-exports.patch
git apply /path/to/Tiffany_thermal/sdk_patches/0002-nt98539a-ife-probe-exports.patch
# 按既有 SDK 流程重建含 kdrv_ife 的媒體驅動，產生配套 Module.symvers。

cd /path/to/Tiffany_thermal/tools/ife_raw14_probe
make SDK_CODE=/path/to/atto640_261012/code \
  KERNEL_DIR=/path/to/prepared-matching-kernel \
  ARCH=arm64 CROSS_COMPILE=/path/to/matching-cross-prefix- \
  KBUILD_EXTRA_SYMBOLS="/path/to/media/Module.symvers /path/to/kwrap/Module.symvers"
```

kernel 目錄必須已使用板端相同設定完成準備。架構、工具鏈、媒體及 kwrap 的 symvers 路徑以實際 SDK 為準；不要使用其他 SoC SDK。`modpost` 若報未定義符號，須先解決配套驅動建置，不可略過錯誤或強制載入。

## 板端操作

**先停止 Tiffany 與所有 ISP／VideoProc 使用者，再測試。** IFE 是共用硬體；原始碼中的 `kdrv_ife_open()` 沒有提供可依賴的互斥擁有權。`exclusive=1` 是操作者聲明已停止使用者，並不是自動偵測保護。不要在 UVC／RTSP／Liveview 仍執行時載入或觸發。

先載入配套媒體及 kwrap 驅動，再載入此診斷模組。實際部署與測試尚未執行。

```sh
insmod ./ife_raw14_probe.ko exclusive=1
cat /proc/ife_raw14_probe/status
sh ./run_probe.sh /mnt/sd/ife_raw14_probe_001
```

`run_probe.sh` 會建立新目錄，已存在就停止，不覆寫先前結果。測試依序為：

1. `bypass_ramp`：覆蓋所有 0..16383 DN，逐像素完全相等才通過。
2. `bypass_levels`：檢查 0/1/2/3、4095/4096、8191/8192、12287/12288、16382/16383 邊界。
3. `denoise_levels`：檢查開啟降噪後的平坦區及邊界。
4. `bypass_noise`／`denoise_noise`：相同的合成雜訊輸入，約 8192±32 DN。

前兩項任一失敗便停止，模組也會拒絕開啟 NR。每組保存 input、output、status；RAW 每檔 614400 bytes。模組載入時不觸發硬體，只有寫入 status 才執行一幀。

硬體錯誤或兩秒逾時不會算成輸出成功。若 reset 也失敗，模組會保留 DDR 及自身參考，避免 DMA 使用已釋放的記憶體；此時需重開機，不要強制卸載。正常完成後：

```sh
rmmod ife_raw14_probe
# 重新啟動 Tiffany，使正式管線重新初始化 IFE。
```

## 主機分析

將結果目錄帶回主機後：

```sh
python tools/ife_raw14_probe/analyze.py /path/to/ife_raw14_probe_001
```

產生 `analysis.json`，不覆寫既有報告。包含 SHA256、輸入 pattern 核對、逐像素差異、最大誤差、低位元分布、唯一值數、候選截斷／位移模式，以及合成平坦區的 SD 比例。旁路失敗留下的部分結果也可分析，缺少必要測試不會判定通過。

判讀：

- 旁路必須逐像素一致；若出現 ×4、÷4 或低 12-bit 回繞，保存原始證據並停止，不靠補 shift 宣稱通過。
- 旁路通過只證明該設定下的傳輸；還要查看 NR 開啟時平坦區是否量化、是否有實際降噪作用。NR-on 若完全等於輸入，不代表降噪成功。
- 合成雜訊 SD 下降不等於真實感測器 temporal noise 改善。還需真實同場景 corrected RAW、細節與目標 FPS 測試。
- `elapsed_ns` 是單幀提交到完成的時間，不是持續串流 FPS。
- 此測試沒有驗證與正式 VideoProc 同時使用 IFE 的資源排程，不可據此直接合併到正式 AGC。

## 已完成的主機檢查

```sh
python tests/test_ife_raw14_probe.py --sdk /path/to/atto640_261012/code
```

以配套 SDK 的真實型別編譯 config；驗證 RAW16 LSB、D2D、獨立緩衝區、功能關閉／開啟及完整 DN 覆蓋。分析工具測試刻意餵入 `RAW14 >> 2` 與截短檔案，確認會拒絕。這些檢查不包含 kernel module 的目標編譯或硬體執行。

來源：`kdrv_ife.c` 的 D2D 檢查、DMA 輸出與 bitsel 設定；`kwrap/mem.h` 和 `comm/nvt_dmasys/dma_proc.c` 的連續非快取記憶體 API；`kdrv_ipp_utility_539a.h` 的 callback、frame 與 mode 定義。未使用未核實的 HDAL pipe 數值。

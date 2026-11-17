Balanced Background（`agc_weighted_balanced_background=1`）改為 Otsu R5 簡化模式：

1. 以原始 RAW Histogram 做一次 Otsu，最大化兩群的群間變異；同分取最低分界 DN。
2. 分界兩側平均差至少 70 RAW DN 才啟用高溫段。Otsu 仍可為狹窄分布找出 T，
   但這個單一條件避免把小幅雜訊拆成兩個增強區；常數影像沒有有效雙群。
3. 有效分群時取低溫群中位數 C，否則取全圖中位數。C 固定對應 96 Gray。
   背景斜率 g=min(1.2, Gain Limit)，Gain Limit=0 時 g=1.2，單位 Gray/RAW DN。
4. 背景為 clamp(96+g*(DN-C),16,160)。高溫段從背景曲線在 T 的灰階 YT，
   線性銜接至目前最高 DN 對應的 245 Gray，超出時飽和。未啟用高溫段時，
   整張使用 clamp(96+g*(DN-C),16,245)，不另外拉伸窄幅雜訊。
5. 保留原生 RAW14 LUT（Gray 僅為說明單位），再做一次既有 Q8 LUT Smooth，
   最後一次 Gain Limit；不做全圖平均亮度補償，最後才對像素查表。

此模式只使用 Gain Limit、Smooth；Clip／Redistribution／Bright Weight／P
保留給 Classic，切換回 Classic 時仍使用原設定。調整這些未生效參數不會重置
Otsu 模式的 LUT 歷史。CFG 選單／狀態顯示會標示，持久化鍵與開關保持不變。

已移除多群選擇、歷史中心匹配、熱源比例目標、指數暗部、高溫區重複 Clip／
Bright Weight／局部 Gain Limit、中心限速、暖機與額外恢復狀態。沒有逐幀配置。
FFC、Shading、手動校正與手動開快門成功時，只重置既有 LUT／Gain 歷史。
手動快門通知不修改 FFC 表、不執行 FFC；無 GPIO 通知的實體遮鏡仍受設定的
LUT Smooth 影響，不保證立即恢復。UART 第一個平衡影格標示 `Balanced Otsu R5`。

單靠 Otsu 無法保證較低溫群就是真正背景；極端值可能影響高溫端點，線性高溫
細節也可能弱於原 Weighted Mapping。最終 Gain Limit 可能讓最高輸出低於 245 Gray。
Toolkit 的靜態 v2 尚未改為此算法，不能再把兩者視為相同參考。

主機檢查：`python tests/test_weighted_balanced.py`，可加
`--raw-dir <RAW目錄> --output-dir <結果目錄>` 產生 RAW14／PNG 與數值參考報告；
`--before-dir <修改前agc_weighted.c/.h目錄>` 可驗證 Classic 保持逐位元一致。
測試包含 Otsu 與分段映射的獨立 NumPy 參考、均勻／窄幅雜訊、熱源占比 1..99%、
Smooth=0/50/90/99、開鏡延遲與校正重置、原圖保存、in-place 與未生效參數。

Weighted 工作區為 229,432 bytes（主機 GCC 對齊），開始出流前配置、停止時釋放。
沿用既有 RAW snapshot、輸出 block 與 RAW_INPUT_SYNC。主機驗證不代表 SDK link、
上板畫質或板端 FPS 已通過。

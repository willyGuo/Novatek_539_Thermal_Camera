/* Ported from DDE_1106: native Y8 I/O, byte stride, 256-entry difference LUT.
 * Retains the supplied directional/paired-sigma/strip-fusion defaults.
 * Called after CLAHE; no RAW14 conversion or second tone mapping.
 */
/* =====================================================================
 * dde_dt_opt.c  ─  Domain Transform Filter 為基礎的 DDE 細節增強
 *
 * 原生 Y8 plane，line_stride 單位為 byte。介面見 dde_dt_opt.h。
 *
 * 水平與垂直濾波皆採 tile-based 改寫，兩者共用同一組分塊暫存區：
 *   水平 pass 使用 a_t、ybrow             (列本身連續，不需轉置)
 *   垂直 pass 使用 gtile/dst_t、a_t、src_t、ybrow  (需轉置以消除跨列存取)
 * 兩個 pass 不同時作用，故可安全共用。
 * ===================================================================== */

#include "dde_dt_opt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 百分位數直方圖的 bin 數。bin 邊界會自適應到實際資料的 [min, max]，
 * 因此 4096 bin 提供 span 的 1/4096 相對精度，對 1%/99% 百分位已足夠。 */
#define PERCENTILE_BINS 4096

/* 遞迴係數查表大小。來源為 8-bit 整數影像，相鄰像素的絕對差值
 * 上限為 255，故 256 項可完整覆蓋且無需內插。 */
#define DIFF_LUT_SIZE 256

/* ---------------------------------------------------------------------
 * 效能說明
 * ---------------------------------------------------------------------
 * 水平與垂直濾波皆採 tile-based 改寫 (見 DDE_VTILE)。兩者解決的瓶頸
 * 不完全相同：
 *
 *   水平 pass：列本身即連續，無跨列存取問題，因此「不」轉置。
 *              改寫只為消除遞迴的 loop-carried dependency ——
 *              同時推進 TILE 列，各列互相掩蓋彼此的 FMA 延遲。
 *
 *   垂直 pass：同時有跨列存取與遞迴延遲兩個問題，因此先把 TILE 欄
 *              轉置進連續的 tile-major 暫存區，再同時推進 TILE 條鏈。
 *
 * 改寫僅變更迴圈巢狀次序，未更動任何浮點運算式或其結合順序，
 * 亦未在列與列 (或欄與欄) 之間交換任何資料，因此輸出與逐列/逐欄版本
 * 逐位元完全相同。已於 -O1 / -Os / -O2 / -O2 -ffp-contract=off /
 * -O3 -march=native 五種組態下驗證輸出影像與 base 層皆零差異。
 *
 * 實測 (x86-64 2.1 GHz、gcc -O2、640x512、num_iters=3、DDE_VTILE=16)：
 *
 *     水平濾波   6.718 ms  ->   3.722 ms    1.80x
 *     垂直濾波  10.663 ms  ->   4.690 ms    2.27x
 *     整幀      18.825 ms  ->  10.030 ms    1.88x
 *
 *     -O3 -march=native 下整幀 20.987 ms -> 7.384 ms  (2.84x)
 * -------------------------------------------------------------------*/
/* ---------------------------------------------------------------------
 * 分塊大小 (水平 pass 為列數，垂直 pass 為欄數)
 *
 * 可於編譯期以 -DDDE_VTILE=n 覆寫。以下為 640x512 / num_iters=3 /
 * gcc -O2 的整幀實測：
 *
 *      4  -> 1.65x   暫存區 +27 KB
 *      8  -> 1.81x   暫存區 +47 KB
 *     16  -> 1.89x   暫存區 +108 KB    <-- 預設，收益/記憶體比最佳
 *     24  -> 1.90x   暫存區 +168 KB
 *     32  -> 1.91x   暫存區 +228 KB
 *
 * 垂直 pass 的收益在 8 左右即飽和 (8 條獨立鏈已足以填滿典型核心的
 * 注意：主函式內會在 stack 上配置 3 個長度為 DDE_VTILE 的指標陣列。
 * -------------------------------------------------------------------*/
#ifndef DDE_VTILE
#define DDE_VTILE 16
#endif


/* ---------------------------------------------------------------------
 * 遞迴方向模式  DDE_DIR_MODE
 * ---------------------------------------------------------------------
 *   0 = 雙向 (預設)。每次迭代對每軸各做一次正向與一次反向遞迴，
 *       再就地平均。等效核為零相位對稱核，與原始版本逐位元相同。
 *
 *   1 = 單向交替。每次迭代對每軸只做「一個」方向的遞迴，
 *       偶數次迭代 (iter = 0,2,4...) 走正向 (左->右 / 上->下)，
 *       奇數次迭代走反向 (右->左 / 下->上)。
 *
 * ── 單向模式的物理意義 ──
 * 單向 IIR  y[i] = (1-a)x[i] + a*y[i-1]  的等效核為單邊指數
 * (1-a)a^k，質心位於 a/(1-a) 個像素處，並非零相位。
 * sigma_s = 20.5、平坦區 (d = 0) 時 a = exp(-sqrt(2)/20.5) = 0.9333，
 * 亦即 base 層會整體位移 a/(1-a) = 14.0 像素。
 *
 * DDE 取 detail = I - B，B 一旦帶相位，detail 便不再是 band-pass，
 * 而成為 14 像素尺度的方向性差分 —— 影像呈現浮雕 (emboss)，
 * 每條熱邊在右下側拖出寬約 14 像素的亮暗尾巴，再乘上 gain 後更明顯。
 *
 * 因此 DDE_DIR_MODE = 1 且 num_iters 為奇數時，最後必有一個方向未被
 * 抵消，輸出不可用。單向模式只有在正反兩趟「使用相同的 a」時相位才會
 * 完全抵消 (即 filtfilt)，這正是 DDE_DIR_PAIRED_SIGMA 的用途。
 * -------------------------------------------------------------------*/
#ifndef DDE_DIR_MODE
#define DDE_DIR_MODE 1
#endif

/* ---------------------------------------------------------------------
 * 配對 sigma 排程  DDE_DIR_PAIRED_SIGMA  (僅在 DDE_DIR_MODE = 1 時有效)
 * ---------------------------------------------------------------------
 *   0 = 沿用標準多尺度排程，每次迭代 sigma 減半。
 *       此時正向 (大 sigma) 與反向 (小 sigma) 的位移量不同，相位不會
 *       抵消。num_iters = 2、sigma_s = 20.5 時：
 *           iter0 正向  sigma = 18.34  a = 0.9258  位移 +12.48 px
 *           iter1 反向  sigma =  9.17  a = 0.8571  位移  -6.00 px
 *       淨位移仍有 +6.5 px，且等效核為左右衰減常數不同的歪斜雙邊指數。
 *
 *   1 = 相鄰兩次迭代共用同一個 sigma，構成 (正向, 反向) 配對。
 *       同一個 a 的正反串接即 filtfilt，等效核為
 *           h(x) = (1-a)/(1+a) * a^|x|
 *       嚴格零相位，其變異數為雙向平均版的 2/(1+a) 倍 (a = 0.933 時
 *       僅差 3.4%)，兩者畫質實質等價。
 *
 *       排程尺度數 = ceil(num_iters / 2)，故：
 *           DIR_MODE=1 + PAIRED + num_iters=2  ~= DIR_MODE=0 + num_iters=1
 *           DIR_MODE=1 + PAIRED + num_iters=4  ~= DIR_MODE=0 + num_iters=2
 *       但前者的遞迴 pass 數與後者相同，卻多付一倍的 LUT 建表、係數
 *       計算 (A-2/B-2)、轉置 (B-1) 與寫回 (B-5)，整體「更慢」。
 *       此開關的價值在於驗證上述結論，而非效能改善。
 *
 * 配對模式下奇數次迭代與前一次的 sigma 相同，LUT 可直接沿用不重建。
 * -------------------------------------------------------------------*/
#ifndef DDE_DIR_PAIRED_SIGMA
#define DDE_DIR_PAIRED_SIGMA 1
#endif

#if DDE_DIR_MODE == 1 && DDE_DIR_PAIRED_SIGMA
#define DDE_SCHED_N(n)   (((n) + 1) / 2)
#define DDE_SCHED_I(i)   ((i) >> 1)
#define DDE_LUT_REUSE(i) (((i) & 1) != 0)   /* 奇數次沿用前次 LUT */
#else
#define DDE_SCHED_N(n)   (n)
#define DDE_SCHED_I(i)   (i)
#define DDE_LUT_REUSE(i) (0)
#endif


/* ---------------------------------------------------------------------
 * 除錯開關：略過 Step 4 的自適應映射
 *
 * 設為 1 時 scale = 1、offset = 0，Step 5 只做夾擠與四捨五入，
 * 亦即直接輸出 Step 3 的 I_comb 絕對 DN 值。用於單獨檢視
 * DTF 濾波與細節增益的效果。
 *
 * 注意：這不是「關掉 Step 5」——float 轉 uint16 的夾擠是型別安全所必需，
 * 無法省略。I_comb 若落在 [0, out_max] 之外仍會被截掉。
 * -------------------------------------------------------------------*/
#ifndef DDE_USING_MAPPING
#define DDE_USING_MAPPING 0
#endif

/* ---------------------------------------------------------------------
 * 線性拉伸倍率上限。
 *
 * 熱像的原始動態範圍遠小於 14-bit 容器 (實測感測器輸出約
 * [3603, 8878]，僅佔 16383 的 32%)，因此 Step 5 的映射「必須」能夠
 * 擴張，否則畫面會集中在輸出範圍底部而呈現全黑。
 *
 * 但 win/span 在場景近乎均勻時 (例如對著白牆) 會趨近無窮大，
 * 把感測器雜訊放大到填滿整個輸出範圍。此上限即為防護：
 * 8.0 代表「原始 span 至少要佔輸出範圍的 1/8，才做滿幅拉伸」。
 *
 * 此值直接決定 AGC 的積極程度，建議對著數種典型場景實機調整：
 *   太大 -> 均勻場景出現明顯顆粒雜訊
 *   太小 -> 低對比場景拉不開，畫面偏灰
 * 可於編譯期以 -DDDE_MAX_STRETCH=n.0f 覆寫。
 * -------------------------------------------------------------------*/
#ifndef DDE_MAX_STRETCH
#define DDE_MAX_STRETCH 2.5f
#endif

#if DDE_USING_MAPPING
/* =====================================================================
 * 內部輔助：直方圖百分位數
 *
 * 使用外部傳入的 hist_buf 而非區域陣列，避免在 RTOS 的小 stack 上
 * 配置 16 KB 造成溢位。
 * ===================================================================== */
static void compute_percentiles(const float *data, int length,
                                float min_val, float max_val,
                                float p_low, float p_high,
                                float *v_min, float *v_max,
                                int *hist_buf)
{
    /*float min_val = data[0];
    float max_val = data[0];

    // 1. 找出極值
    for (int i = 1; i < length; i++) {
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }*/

    float range = max_val - min_val;
    if (range <= 0.0001f) {
        /* 近乎均勻的畫面，直接回傳極值，由呼叫端退化為純平移 */
        *v_min = min_val;
        *v_max = max_val;
        return;
    }

    memset(hist_buf, 0, PERCENTILE_BINS * sizeof(int));

    /* 2. 建立直方圖 */
    for (int i = 0; i < length; i++) {
        int bin = (int)(((data[i] - min_val) / range) * (PERCENTILE_BINS - 1));
        if (bin < 0) bin = 0;
        if (bin >= PERCENTILE_BINS) bin = PERCENTILE_BINS - 1;
        hist_buf[bin]++;
    }

    int target_low  = (int)(((float)length * p_low)  / 100.0f);
    int target_high = (int)(((float)length * p_high) / 100.0f);

    /* 3. 低百分位 */
    int accum = 0;
    *v_min = min_val;
    for (int i = 0; i < PERCENTILE_BINS; i++) {
        accum += hist_buf[i];
        if (accum >= target_low) {
            *v_min = min_val + ((float)i / (PERCENTILE_BINS - 1)) * range;
            break;
        }
    }

    /* 4. 高百分位 */
    accum = 0;
    *v_max = max_val;
    for (int i = 0; i < PERCENTILE_BINS; i++) {
        accum += hist_buf[i];
        if (accum >= target_high) {
            *v_max = min_val + ((float)i / (PERCENTILE_BINS - 1)) * range;
            break;
        }
    }
}
#endif /* DDE_USING_MAPPING */

/* =====================================================================
 * 【OPT-6】條帶融合路徑 (Strip-fused)
 * =====================================================================
 * 適用條件：DDE_DIR_MODE == 1 (單向) 且 DDE_USING_MAPPING == 0。
 * 可於編譯期以 -DDDE_STRIP_FUSE=0 關閉，退回原本的分離式 pass。
 *
 * ── 為什麼只有單向模式能這樣做 ──
 * 原本的流程是「水平 pass 跑完整幀 -> 寫 out_h -> 垂直 pass 再整幀讀回」，
 * 因為雙向水平遞迴的第 x 欄輸出同時依賴左右兩側，必須整列算完才能確定。
 *
 * 單向則不然：正向水平遞迴 Bf(x) 只依賴 x 左側。若把影像切成寬 VTILE 的
 * 直條、由左至右處理，則處理第 k 條時，所有列的遞迴都「恰好」推進到該條
 * 的右緣 —— 這一條的水平結果已經是最終值，可以直接轉置進 src_t 做垂直
 * 遞迴，然後輸出。每列只需保留一個跨條帶的遞迴狀態 hcar[y]。
 * 反向則把條帶順序改為由右至左，對稱成立。
 *
 * 結果：out_h 整張消失 (省 1.25 MB 的寫入與讀回)，A-2/A-3/B-1/B-2
 * 四個全幀 pass 合併成對 p_img 的單一條帶掃描，Step 3 與 Step 5 也併入
 * 條帶輸出。n = 1 時整幀只剩「讀 p_img 一次、寫 p_img 一次」。
 *
 * ── in-place 的唯一危險點：條帶邊界的 guide ──
 * 最後一次迭代會把結果直接寫回 p_img。處理下一條時，跨條帶那條邊的係數
 * a 需要「上一條最靠邊那一欄的原始值」，但它已被覆寫。因此每列另存
 * gcar[y] = 該欄的原始 guide 值，在覆寫之前讀取。條帶內部的水平邊、
 * 垂直邊與 Step 3 所需的原始值 I，都在本條輸出之前讀取，不受影響。
 *
 * ── 逐位元一致性 ──
 * 遞迴式、係數索引、邊界條件與原版完全相同 (x86 -O2 下逐位元驗證)。
 * 注意：ARM 上 gcc 預設 -ffp-contract=fast，編譯器可能在不同的程式
 * 形狀下選擇不同的 FMA 融合方式，導致與舊版在最末位元上有差異，
 * 這不是錯誤。若需逐位元比對，兩版皆以 -ffp-contract=off 編譯。
 * ===================================================================== */
#ifndef DDE_STRIP_FUSE
#define DDE_STRIP_FUSE 1
#endif

#define DDE_FUSED ((DDE_DIR_MODE == 1) && !DDE_USING_MAPPING && DDE_STRIP_FUSE)

/* 水平遞迴在條帶內是每列一條長度 VTILE 的串列鏈。同時推進 DDE_HROWS
 * 列，讓各列互相掩蓋浮點延遲 (A53 為 in-order 核心，無法靠亂序執行)。 */
#ifndef DDE_HROWS
#define DDE_HROWS 8
#endif

/* ---------------------------------------------------------------------
 * 遞迴式的代數形式  DDE_RECUR_LERP
 *
 *   0       ： B(x) = (1 - a) * I(x) + a * B(x-1)
 *   1 (預設)： B(x) = I(x) + a * ( B(x-1) - I(x) )
 *
 * 兩者代數上恆等，後者即 Gastal 原始 MATLAB 實作 (RF.m) 的寫法。
 * 前者需要 1 次減法 (1-a)、2 次乘法、1 次加法；後者只需 1 次減法與
 * 1 次乘加 (可融合為單一 FMA)，且迴圈攜帶的依賴鏈從「乘 -> 加」
 * 變成「減 -> FMA」。
 *
 * 代價是捨入順序不同，輸出不再與前者逐位元相同 (差異為浮點最末位元
 * 等級，經 gain 放大與四捨五入後可能出現像素差異)。目前預設啟用，
 * 可用 -DDDE_RECUR_LERP=0 建立原公式的 A/B 版本；SoC FPS 收益仍須實測。
 * -------------------------------------------------------------------*/
#ifndef DDE_RECUR_LERP
#define DDE_RECUR_LERP 1
#endif
#if DDE_RECUR_LERP
#define DDE_REC(a, s, prev) ((s) + (a) * ((prev) - (s)))
#else
#define DDE_REC(a, s, prev) ((1.0f - (a)) * (s) + (a) * (prev))
#endif

/* Explicit four-lane recurrences. ARM builds select NEON via SDK flags;
 * the same GNU vector code can be forced on x86 for numerical testing.
 * Keep a scalar build (-DDDE_SIMD4=0) for target A/B measurements. */
#ifndef DDE_SIMD4
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#define DDE_SIMD4 1
#else
#define DDE_SIMD4 0
#endif
#endif

#if DDE_SIMD4 && DDE_FUSED
typedef float dde_f4 __attribute__((vector_size(16)));
typedef int32_t dde_i4 __attribute__((vector_size(16)));

static inline dde_f4 dde_load4(const float *p)
{
    dde_f4 value;
    /* Workspace/tile tails need not be 16-byte aligned. */
    __builtin_memcpy(&value, p, sizeof(value));
    return value;
}

static inline void dde_store4(float *p, dde_f4 value)
{
    __builtin_memcpy(p, &value, sizeof(value));
}

static inline dde_f4 dde_select4(dde_i4 mask, dde_f4 yes, dde_f4 no)
{
    return (dde_f4)((mask & (dde_i4)yes) | (~mask & (dde_i4)no));
}

static inline void dde_vertical_row(float *current, const float *previous,
    const float *coefficients, uint32_t count)
{
    uint32_t t = 0;
    for (; t + 4U <= count; t += 4U) {
        dde_f4 s = dde_load4(current + t);
        dde_f4 a = dde_load4(coefficients + t);
        dde_f4 p = dde_load4(previous + t);
        dde_store4(current + t, DDE_REC(a, s, p));
    }
    for (; t < count; t++) current[t] = DDE_REC(coefficients[t], current[t], previous[t]);
}
#endif

#if DDE_FUSED
/* first / fwd 以編譯期常數傳入 (見下方 dt_fused_iter 的分派)，
 * always_inline 讓編譯器為 4 種組合各產生一份無分支的特化版本，
 * 內層迴圈不再有「來源是 p_img 還是 base」與「正向或反向」的判斷。 */
static inline __attribute__((always_inline)) void dt_fused_iter_impl(
    uint8_t       *p_img,
    uint32_t        W,
    uint32_t        H,
    uint32_t        stride,
    float          *base,     /* 非首次迭代的來源、非末次迭代的目的地 */
    int             first,    /* 來源為 p_img (而非 base) */
    int             last,     /* 輸出直接寫回 p_img (含 Step 3 + Step 5) */
    int             fwd,      /* 1: 左->右 / 上->下 ；0: 右->左 / 下->上 */
    float           gain,
    float           f_black,
    float           f_max,
    const float    *lut,
    float          *a_t,      /* H * VTILE */
    float          *src_t,    /* H * VTILE */
    float          *hcar,     /* H：跨條帶的水平遞迴狀態 */
    uint8_t       *gcar)     /* H：跨條帶的原始 guide 值 */
{
    const uint32_t VT     = DDE_VTILE;
    const uint32_t nstrip = (W + VT - 1u) / VT;

    for (uint32_t k = 0; k < nstrip; k++) {
        const uint32_t s    = fwd ? k : (nstrip - 1u - k);
        const uint32_t c0   = s * VT;
        uint32_t       tw   = W - c0;
        if (tw > VT) tw = VT;
        /* 本條是否含遞迴起點：正向為最左欄，反向為最右欄 */
        const int      edge = fwd ? (c0 == 0u) : (c0 + tw == W);

        /* ── F-1. 水平遞迴 (直接轉置寫入 src_t) + 垂直係數 ────────── */
        for (uint32_t y0 = 0; y0 < H; y0 += DDE_HROWS) {
            uint32_t rn = H - y0;
            if (rn > DDE_HROWS) rn = DDE_HROWS;

            const uint8_t *gr[DDE_HROWS];
            const float    *sr[DDE_HROWS];
            float           h [DDE_HROWS];
            int             gp[DDE_HROWS];

            for (uint32_t r = 0; r < rn; r++) {
                gr[r] = p_img + (size_t)(y0 + r) * stride + c0;
                sr[r] = base  + (size_t)(y0 + r) * W + c0;
            }

            uint32_t n0 = 0;   /* 已處理的條帶內欄數 */
            if (edge) {
                /* 遞迴起點：Bf(0) = I(0) 或 Bb(W-1) = I(W-1) */
                const uint32_t tt = fwd ? 0u : (tw - 1u);
                for (uint32_t r = 0; r < rn; r++) {
                    const int   g  = gr[r][tt];
                    const float sv = first ? (float)g : sr[r][tt];
                    h [r] = sv;
                    gp[r] = g;
                    src_t[(size_t)(y0 + r) * VT + tt] = sv;
                }
                n0 = 1;
            } else {
                for (uint32_t r = 0; r < rn; r++) {
                    h [r] = hcar[y0 + r];
                    gp[r] = (int)gcar[y0 + r];
                }
            }

#if DDE_SIMD4
            dde_f4 h4[(DDE_HROWS + 3) / 4];
            for (uint32_t r = 0; r + 4U <= rn; r += 4U)
                h4[r / 4U] = dde_load4(h + r);
#endif
            for (uint32_t n = n0; n < tw; n++) {
                const uint32_t tt = fwd ? n : (tw - 1u - n);
                uint32_t r = 0;
#if DDE_SIMD4
                /* Each lane follows its own row; no recurrence dependency is
                 * reordered within a row. Four guide/LUT reads feed one SIMD op. */
                for (; r + 4U <= rn; r += 4U) {
                    float samples[4], coefficients[4];
                    for (uint32_t lane = 0; lane < 4U; lane++) {
                        uint32_t row = r + lane;
                        int g = gr[row][tt];
                        int d = g - gp[row];
                        if (d < 0) d = -d;
                        coefficients[lane] = lut[d]; /* Y8 difference is 0..255. */
                        samples[lane] = first ? (float)g : sr[row][tt];
                        gp[row] = g;
                    }
                    dde_f4 a = dde_load4(coefficients);
                    dde_f4 sv = dde_load4(samples);
                    dde_f4 value = DDE_REC(a, sv, h4[r / 4U]);
                    h4[r / 4U] = value;
                    for (uint32_t lane = 0; lane < 4U; lane++)
                        src_t[(size_t)(y0 + r + lane) * VT + tt] = value[lane];
                }
#endif
                for (; r < rn; r++) {
                    const int   g  = gr[r][tt];
                    const float sv = first ? (float)g : sr[r][tt];
                    int d = g - gp[r];
                    if (d < 0) d = -d;
                    if (d > (DIFF_LUT_SIZE - 1)) d = DIFF_LUT_SIZE - 1;
                    const float a  = lut[d];
                    const float hv = DDE_REC(a, sv, h[r]);
                    h [r] = hv;
                    gp[r] = g;
                    src_t[(size_t)(y0 + r) * VT + tt] = hv;
                }
            }

#if DDE_SIMD4
            for (uint32_t r = 0; r + 4U <= rn; r += 4U)
                dde_store4(h + r, h4[r / 4U]);
#endif
            for (uint32_t r = 0; r < rn; r++) {
                hcar[y0 + r] = h[r];
                gcar[y0 + r] = (uint8_t)gp[r];   /* 覆寫前的原始值 */
            }

            /* 垂直係數：邊 (y, y+1)。兩列此刻都還在 L1，且尚未被覆寫。 */
            for (uint32_t r = 0; r < rn; r++) {
                const uint32_t y = y0 + r;
                if (y + 1u >= H) break;
                const uint8_t *p0 = gr[r];
                const uint8_t *p1 = p0 + stride;
                float *at = a_t + (size_t)y * VT;
                for (uint32_t t = 0; t < tw; t++) {
                    int d = (int)p1[t] - (int)p0[t];
                    if (d < 0) d = -d;
                    if (d > (DIFF_LUT_SIZE - 1)) d = DIFF_LUT_SIZE - 1;
                    at[t] = lut[d];
                }
            }
        }

        /* ── F-2. 垂直遞迴，就地於 src_t (與 OPT-4 相同) ────────── */
        if (fwd) {
            for (uint32_t i = 1; i < H; i++) {
                const float *at = a_t   + (size_t)(i - 1u) * VT;
                const float *sp = src_t + (size_t)(i - 1u) * VT;
                float       *sc = src_t + (size_t)i * VT;
#if DDE_SIMD4
                dde_vertical_row(sc, sp, at, tw);
#else
                for (uint32_t t = 0; t < tw; t++) sc[t] = DDE_REC(at[t], sc[t], sp[t]);
#endif
            }
        } else {
            for (int i = (int)H - 2; i >= 0; i--) {
                const float *at = a_t   + (size_t)i * VT;
                const float *sn = src_t + (size_t)(i + 1) * VT;
                float       *sc = src_t + (size_t)i * VT;
#if DDE_SIMD4
                dde_vertical_row(sc, sn, at, tw);
#else
                for (uint32_t t = 0; t < tw; t++) sc[t] = DDE_REC(at[t], sc[t], sn[t]);
#endif
            }
        }

        /* ── F-3. 輸出 ────────────────────────────────────────────
         * 末次迭代：Step 3 細節重組 + Step 5 夾擠四捨五入，直接寫回 p_img。
         * 未啟用映射時 scale = 1、offset = 0，故 val = v，與原版相同。
         * 非末次迭代：寫入 base，供下一次迭代當來源 (就地亦安全，
         * 下一條只讀自己的欄)。 */
        if (last) {
            for (uint32_t y = 0; y < H; y++) {
                const float *dc = src_t + (size_t)y * VT;
                uint8_t    *pr = p_img + (size_t)y * stride + c0;
                uint32_t t = 0;
#if DDE_SIMD4
                const dde_f4 black4 = {f_black, f_black, f_black, f_black};
                const dde_f4 max4 = {f_max, f_max, f_max, f_max};
                for (; t + 4U <= tw; t += 4U) {
                    dde_i4 original = {pr[t], pr[t+1], pr[t+2], pr[t+3]};
                    dde_f4 source = __builtin_convertvector(original, dde_f4);
                    dde_f4 b = dde_load4(dc + t);
                    dde_f4 value = b + (source - b) * gain;
                    value = dde_select4(value > black4, value, black4);
                    value = dde_select4(value > max4, max4, value);
                    dde_i4 rounded = __builtin_convertvector(value + 0.5f, dde_i4);
                    for (uint32_t lane = 0; lane < 4U; lane++) pr[t+lane] = (uint8_t)rounded[lane];
                }
#endif
                for (; t < tw; t++) {
                    const float b = dc[t];
                    float val = b + ((float)pr[t] - b) * gain;
                    if (!(val > f_black)) val = f_black;
                    if (val > f_max)      val = f_max;
                    pr[t] = (uint8_t)(val + 0.5f);
                }
            }
        } else {
            for (uint32_t y = 0; y < H; y++) {
                const float *dc = src_t + (size_t)y * VT;
                float       *br = base  + (size_t)y * W + c0;
                for (uint32_t t = 0; t < tw; t++) br[t] = dc[t];
            }
        }
    }
}

static void dt_fused_iter(
    uint8_t *p_img, uint32_t W, uint32_t H, uint32_t stride, float *base,
    int first, int last, int fwd, float gain, float f_black, float f_max,
    const float *lut, float *a_t, float *src_t, float *hcar, uint8_t *gcar)
{
#define DDE_FI(F, D) dt_fused_iter_impl(p_img, W, H, stride, base, F, last, D, \
                                        gain, f_black, f_max, lut, a_t, src_t, hcar, gcar)
    if (first) { if (fwd) DDE_FI(1, 1); else DDE_FI(1, 0); }
    else       { if (fwd) DDE_FI(0, 1); else DDE_FI(0, 0); }
#undef DDE_FI
}
#endif /* DDE_FUSED */

const char *dde_dt_backend_name(void)
{
#if DDE_SIMD4 && DDE_FUSED
    return "SIMD4 (four independent rows/columns)";
#else
    return "scalar";
#endif
}

/* =====================================================================
 * 取得工作緩衝區大小
 * ===================================================================== */
#if DDE_FUSED
/* write_output=0 retains the float base in workspace; guide remains unchanged. */
static void dde_run_fused(uint8_t *p_img, uint32_t width, uint32_t height,
    uint32_t line_stride, float sigma_s, float sigma_r, float gain, int num_iters,
    uint8_t out_max, uint8_t out_black, int write_output, void *pWorkBuf)
{
        /* ── 條帶融合路徑 (OPT-6) ─────────────────────────────── */
        const size_t npx = (size_t)width * height;
        float    *base     = (float *)pWorkBuf;
        float    *diff_lut = base     + npx;
        float    *a_t      = diff_lut + DIFF_LUT_SIZE;
        float    *src_t    = a_t      + (size_t)height * DDE_VTILE;
        float    *hcar     = src_t    + (size_t)height * DDE_VTILE;
        uint8_t *gcar     = (uint8_t *)(hcar + height);

        const float f_black = (float)out_black;
        const float f_max   = (float)out_max;

        const int   sched_n = DDE_SCHED_N(num_iters);
        const float denom   = sqrtf(powf(4.0f, (float)sched_n) - 1.0f);

        for (int iter = 0; iter < num_iters; iter++) {
            const int sched_i = DDE_SCHED_I(iter);
            float sigma_s_iter = sigma_s * sqrtf(3.0f)
                               * powf(2.0f, (float)(sched_n - sched_i - 1)) / denom;
            float factor        = (sigma_s_iter / sigma_r) * (sigma_s_iter / sigma_r);
            float sqrt2_over_ss = (float)(sqrt(2.0) / sigma_s_iter);

            if (!DDE_LUT_REUSE(iter)) {
                for (int d = 0; d < DIFF_LUT_SIZE; d++) {
                    float val = 1.0f + factor * (float)d * (float)d;
                    diff_lut[d] = expf(-sqrt2_over_ss * sqrtf(val));
                }
            }

            dt_fused_iter(p_img, width, height, line_stride, base,
                          iter == 0, write_output && iter == num_iters - 1, (iter & 1) == 0,
                          gain, f_black, f_max,
                          diff_lut, a_t, src_t, hcar, gcar);
        }
}

typedef struct { uint32_t lo, hi; float weight; } DDE_SCALE_AXIS;

static void dde_half_axis(DDE_SCALE_AXIS *axis, uint32_t full, uint32_t small)
{
    for (uint32_t x = 0; x < full; x++) {
        if (small == 1) { axis[x] = (DDE_SCALE_AXIS){0, 0, 0}; continue; }
        int lo = (int)((float)x * 0.5f - 0.25f);
        if (lo < 0) lo = 0;
        if (lo >= (int)small - 1) lo = (int)small - 2;
        uint32_t end = ((uint32_t)lo + 2U) * 2U;
        if (end > full) end = full;
        float first = (float)lo * 2.0f + 0.5f;
        float second = ((float)((uint32_t)lo + 1U) * 2.0f + end - 1.0f) * 0.5f;
        float weight = ((float)x - first) / (second - first);
        if (weight < 0) weight = 0;
        if (weight > 1) weight = 1;
        axis[x] = (DDE_SCALE_AXIS){(uint32_t)lo, (uint32_t)lo + 1U, weight};
    }
}
#endif

uint32_t dde_dt_get_workbuf_size(uint32_t width, uint32_t height)
{
    if (!width || !height || width > 4096 || height > 4096) return 0;
    uint32_t num_pixels = width * height;
#if DDE_FUSED
    /* 條帶融合路徑 (OPT-6)：
     *   base     : W*H          (僅 num_iters >= 2 時使用，n = 1 時閒置)
     *   diff_lut : 256
     *   a_t      : H * VTILE    (只剩垂直方向需要分塊)
     *   src_t    : H * VTILE
     *   hcar     : H            (float)
     *   gcar     : H            (uint8_t，以 float 單位向上取整)
     * out_h 整張不再需要。640x512 / VTILE=8 時約 1.40 MB。 */
    (void)width;
    uint32_t floats_needed = num_pixels
                           + DIFF_LUT_SIZE
                           + (height * DDE_VTILE * 2u)
                           + height
                           + (height + 3u) / 4u;
    return floats_needed * (uint32_t)sizeof(float);
#else
    uint32_t max_dim    = (width > height) ? width : height;
    uint32_t floats_needed = (num_pixels * 2)
                           + DIFF_LUT_SIZE
                           + (max_dim * DDE_VTILE * 3)
                           + DDE_VTILE;
    if (num_pixels < PERCENTILE_BINS) floats_needed += PERCENTILE_BINS - num_pixels;
    return floats_needed * (uint32_t)sizeof(float);
#endif
}


/* =====================================================================
 * DDE 主函式
 * ===================================================================== */
int DDE_DT_Optimized(
    uint8_t *p_img,
    uint32_t  width,
    uint32_t  height,
    uint32_t  line_stride,
    float     sigma_s,
    float     sigma_r,
    float     gain,
    int       num_iters,
    uint8_t  out_max,
    uint8_t  out_black,
    void     *pWorkBuf)
{
    /* ── 參數驗證 ───────────────────────────────────────────────────
     *
     * sigma_r == 0 會使 factor = (sigma_s/sigma_r)^2 = +Inf，
     * 而 d == 0 (平坦區的相鄰像素，熱像中極常見) 使
     *     val = 1 + Inf * 0 = NaN
     * 進而 diff_lut[0] = NaN。任一組相鄰相等像素即可讓 NaN 沿著遞迴鏈
     * 汙染整幅影像。由於 NaN 與所有比較運算皆為 false，它會直接穿透
     * Step 5 的上下限夾擠，最終 (uint8_t)NaN 為未定義行為 —— 在 x86 上
     * 得到 0，在 ARM 上可能是任意值。sigma_s == 0 同理。
     *
     * 於入口攔截，per-pixel 零成本。否定形式 !(x > 0) 同時攔截 NaN 輸入。
     * 任何非零回傳都在影像被改動之前發生，p_img 保持原狀。
     * ------------------------------------------------------------- */
    if (!p_img || !pWorkBuf || width == 0 || height == 0)  return -1;
    if (!dde_dt_get_workbuf_size(width, height) || line_stride < width ||
        (uint64_t)line_stride * height > UINT32_MAX) return -1;
    if (!isfinite(sigma_s) || !isfinite(sigma_r) || !isfinite(gain) ||
        sigma_s < 0.01f || sigma_s > 4096.0f || sigma_r < 0.01f ||
        sigma_r > 255.0f || gain > 10.0f) return -3;
    if (out_max <= out_black)                              return -2;
    if (!(sigma_s > 0.0f) || !(sigma_r > 0.0f))            return -3;
    if (num_iters < 1 || num_iters > 8)                    return -4;
    if (!(gain >= 0.0f))                                   return -5;

#if DDE_FUSED
    dde_run_fused(p_img, width, height, line_stride, sigma_s, sigma_r, gain,
        num_iters, out_max, out_black, 1, pWorkBuf);
    return 0;
#else

    const int num_pixels = (int)(width * height);
    const int max_dim    = (int)((width > height) ? width : height);

    /* ── Step 0: 記憶體池劃分 ─────────────────────────────────────
     * 單一連續配置，無動態記憶體、無大型 static 陣列。
     * ------------------------------------------------------------- */
    float *base       = (float *)pWorkBuf;                      /* W * H            */
    float *out_h      = base     + num_pixels;                  /* W * H            */
    float *diff_lut   = out_h    + (num_pixels < PERCENTILE_BINS ? PERCENTILE_BINS : num_pixels);                  /* DIFF_LUT_SIZE    */





    /* 分塊暫存區，索引方式為 tile-major: [ i * DDE_VTILE + t ]
     *
     * 【OPT-2】原本的 gtile (p_img 的 float 轉置副本) 已完全移除。
     * B-2 改為直接從 p_img 的相鄰兩列以整數運算求差值，不再需要先把
     * guide 轉置成 float 再相減，因此省下一整趟 gtile 的寫入與讀取。
     * 原屬 gtile 的空間改分配給 dst_t，緩衝區總大小不變。 */
    float *dst_t      = diff_lut + DIFF_LUT_SIZE;               /* max_dim * VTILE  */
    float *a_t        = dst_t + (size_t)max_dim * DDE_VTILE;    /* max_dim * VTILE  */
    float *src_t      = a_t   + (size_t)max_dim * DDE_VTILE;    /* max_dim * VTILE  */
    float *ybrow      = src_t + (size_t)max_dim * DDE_VTILE;    /* DDE_VTILE        */
#if DDE_DIR_MODE == 1
    (void)ybrow;   /* 單向模式不需要保留反向鏈的當前值 */
    (void)dst_t;   /* 單向模式就地遞迴，不需要獨立的目的地暫存區 */
#endif

    /* ── Step 1: 已移除 ───────────────────────────────────────────
     * 【OPT-1】原本先把帶跨距的 p_img 展平成連續 float 平面 base，
     * 再讓第 0 次迭代的水平 pass 去讀它。但那趟寫入 (1.25 MB) 加上
     * 隨後的讀取純屬搬運：水平 pass 本來就已經持有 prow[] 指標。
     *
     * 改為第 0 次迭代直接以 (float)prow[t][i] 為來源，base 僅在
     * num_iters >= 2 時由 B-5 寫入、供後續迭代讀取。轉型結果與原本
     * 存入 base 再讀出完全相同，輸出逐位元一致。
     * ------------------------------------------------------------- */

#if DDE_USING_MAPPING
    /* 【OPT-5】min/max 改在最後一次迭代的 B-5 中就地累積，
     * 故需提升到迭代迴圈之外宣告。 */
    float scan_min =  INFINITY;
    float scan_max = -INFINITY;
#endif

    /* ── Step 2: Domain Transform 迭代濾波 ─────────────────────────
     * sched_n 為「等效尺度數」。雙向模式下等於 num_iters；
     * 單向配對模式下每 2 次單向迭代構成 1 個尺度，故為 ceil(num_iters/2)。
     * ------------------------------------------------------------- */
    const int   sched_n = DDE_SCHED_N(num_iters);
    const float denom   = sqrtf(powf(4.0f, (float)sched_n) - 1.0f);

    for (int iter = 0; iter < num_iters; iter++) {

#if DDE_DIR_MODE == 1
        /* 單向交替：偶數次正向 (左->右 / 上->下)，奇數次反向。 */
        const int dir_fwd = ((iter & 1) == 0);
#endif

        const int first_iter = (iter == 0);
        const int last_iter  = (iter == num_iters - 1);

        /* 多尺度排程：讓 N 個尺度的變異數疊加後逼近單一高斯 */
        const int sched_i = DDE_SCHED_I(iter);
        float sigma_s_iter = sigma_s * sqrtf(3.0f)
                           * powf(2.0f, (float)(sched_n - sched_i - 1)) / denom; //sigma_s

        /* ── 建立本次迭代的係數 LUT ──
         * a(d) = exp( -sqrt(2)/sigma_s * sqrt( 1 + (sigma_s/sigma_r)^2 * d^2 ) )
         * 16384 次 expf+sqrtf 遠低於對 W*H*2 個像素各算一次。 */
        float factor        = (sigma_s_iter / sigma_r) * (sigma_s_iter / sigma_r);  //(sigma_s/sigma_r)^2
        float sqrt2_over_ss = (float)(sqrt(2.0) / sigma_s_iter);                    //sqrt(2)/sigma_s

        /* 配對模式下奇數次迭代的 sigma 與前一次相同，LUT 內容不變，
         * 直接沿用可省下 16384 次 expf + sqrtf。 */
        if (!DDE_LUT_REUSE(iter)) {
            for (int d = 0; d < DIFF_LUT_SIZE; d++) {
                float val = 1.0f + factor * (float)d * (float)d; //( 1 + (sigma_s/sigma_r)^2 * d^2 )
                diff_lut[d] = expf(-sqrt2_over_ss * sqrtf(val)); //a(d) = exp( -sqrt(2)/sigma_s * sqrt( 1 + (sigma_s/sigma_r)^2 * d^2 ) )
            }
        }

        /* ── A. 水平濾波 (tile-based，每次處理 DDE_VTILE 列) ──
         *
         * 水平方向的列本身即連續，沒有跨列存取問題，因此不需要轉置。
         * 但遞迴仍受 loop-carried dependency 拖累：yf[i] 必須等 yf[i-1]
         * 的乘加結果出爐，關鍵路徑 = width x FMA 延遲，流水線大半閒置。
         *
         * 改為同時推進 DDE_VTILE 列：第 t 列在等結果時，其餘列的運算可
         * 填進流水線。每個 i 只有 VTILE 條 cache line 同時處於熱狀態
         * (VTILE=16 時 p_img 佔 1 KB)，且隨 i 前進被完整消耗，
         * 記憶體行為優於逐列版本。
         *
         * 曾實作過轉置版 (把列搬進 tile-major 再算，讓 t 維度連續以利向量化)，
         * 實測整幀 1.68x，反而輸給本版的 1.84x —— 轉置的兩趟全幀搬運成本
         * 超過向量化帶來的好處。水平方向不需要轉置。
         *
         * yf 直接寫進 out_h，反向掃描時就地合成平均；Bb 為算出即用，
         * 只需保留 VTILE 個當前值 (ybrow)。因此不需要額外緩衝區。
         * ----------------------------------------------------------- */
        for (uint32_t r0 = 0; r0 < height; r0 += DDE_VTILE) {
            uint32_t th = height - r0;
            if (th > DDE_VTILE) th = DDE_VTILE;

            /* A-1. 各列的起始指標 (VTILE 個指標，配置於 stack) */
            const uint8_t *prow[DDE_VTILE];
            const float    *srow[DDE_VTILE];
            float          *drow[DDE_VTILE];
            for (uint32_t t = 0; t < th; t++) {
                prow[t] = p_img + (size_t)(r0 + t) * line_stride;
                srow[t] = base  + (size_t)(r0 + t) * width;
                drow[t] = out_h + (size_t)(r0 + t) * width;
            }

            /* A-2. 遞迴係數 a_t[i][t]。guide 恆取自原圖 p_img。
             *
             * 【OPT-3】差值改以整數運算。來源為 uint8_t，差值必為
             * [-255, 255] 的整數，原本的
             *     (int)fabsf((float)a - (float)b)
             * 要做 2 次 int->float 轉型、1 次浮點減、1 次 fabsf、
             * 1 次 float->int 截斷；整數版只需 1 次減法與 1 次取負。
             * 兩者結果完全相同 (float 可精確表示 2^24 以內的整數)。 */
            for (uint32_t i = 0; i + 1 < width; i++) {
                float *at = a_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < th; t++) {
                    int d = (int)prow[t][i + 1] - (int)prow[t][i];
                    if (d < 0) d = -d;
                    if (d > (DIFF_LUT_SIZE - 1)) d = DIFF_LUT_SIZE - 1;
                    at[t] = diff_lut[d];
                }
            }

#if DDE_DIR_MODE == 0
            /* A-3. 正向遞迴，Bf 直接寫入 out_h。
             * first_iter 時來源為 p_img (OPT-1)，其後為 base。 */
            if (first_iter) {
                for (uint32_t t = 0; t < th; t++) drow[t][0] = (float)prow[t][0];
                for (uint32_t i = 1; i < width; i++) {
                    const float *at = a_t + (size_t)(i - 1) * DDE_VTILE;
                    for (uint32_t t = 0; t < th; t++) {
                        drow[t][i] = DDE_REC(at[t], (float)prow[t][i], drow[t][i - 1]);
                    }
                }
                for (uint32_t t = 0; t < th; t++) {
                    ybrow[t] = (float)prow[t][width - 1];
                    drow[t][width - 1] = (drow[t][width - 1] + ybrow[t]) * 0.5f;
                }
                for (int i = (int)width - 2; i >= 0; i--) {
                    const float *at = a_t + (size_t)i * DDE_VTILE;
                    for (uint32_t t = 0; t < th; t++) {
                        ybrow[t] = DDE_REC(at[t], (float)prow[t][i], ybrow[t]);
                        drow[t][i] = (drow[t][i] + ybrow[t]) * 0.5f;
                    }
                }
            } else {
                for (uint32_t t = 0; t < th; t++) drow[t][0] = srow[t][0];
                for (uint32_t i = 1; i < width; i++) {
                    const float *at = a_t + (size_t)(i - 1) * DDE_VTILE;
                    for (uint32_t t = 0; t < th; t++) {
                        drow[t][i] = DDE_REC(at[t], srow[t][i], drow[t][i - 1]);
                    }
                }
                /* A-4. 反向遞迴 + 就地雙向平均 */
                for (uint32_t t = 0; t < th; t++) {
                    ybrow[t] = srow[t][width - 1];
                    drow[t][width - 1] = (drow[t][width - 1] + ybrow[t]) * 0.5f;
                }
                for (int i = (int)width - 2; i >= 0; i--) {
                    const float *at = a_t + (size_t)i * DDE_VTILE;
                    for (uint32_t t = 0; t < th; t++) {
                        ybrow[t] = DDE_REC(at[t], srow[t][i], ybrow[t]);
                        drow[t][i] = (drow[t][i] + ybrow[t]) * 0.5f;
                    }
                }
            }
#else
            /* A-3/A-4 單向版本。
             * 兩個分支各自只掃一趟，不需要 ybrow，也沒有 0.5f 的合成乘法。
             * srow (base) 與 drow (out_h) 為不同緩衝區，反向就地寫入安全。
             *
             * 係數索引約定與雙向版完全一致：a_t[i] 是像素 i 與 i+1 之間的
             * 邊係數，故正向在位置 i 取 a_t[i-1]，反向在位置 i 取 a_t[i]。*/
            /* first_iter 時來源為 p_img (OPT-1)，其後為 base。
             * 判斷提到內層迴圈之外，per-pixel 零成本。 */
            if (dir_fwd) {
                if (first_iter) {
                    for (uint32_t t = 0; t < th; t++) drow[t][0] = (float)prow[t][0];
                    for (uint32_t i = 1; i < width; i++) {
                        const float *at = a_t + (size_t)(i - 1) * DDE_VTILE;
                        for (uint32_t t = 0; t < th; t++) {
                            drow[t][i] = DDE_REC(at[t], (float)prow[t][i], drow[t][i - 1]);
                        }
                    }
                } else {
                    for (uint32_t t = 0; t < th; t++) drow[t][0] = srow[t][0];
                    for (uint32_t i = 1; i < width; i++) {
                        const float *at = a_t + (size_t)(i - 1) * DDE_VTILE;
                        for (uint32_t t = 0; t < th; t++) {
                            drow[t][i] = DDE_REC(at[t], srow[t][i], drow[t][i - 1]);
                        }
                    }
                }
            } else {
                if (first_iter) {
                    for (uint32_t t = 0; t < th; t++) drow[t][width - 1] = (float)prow[t][width - 1];
                    for (int i = (int)width - 2; i >= 0; i--) {
                        const float *at = a_t + (size_t)i * DDE_VTILE;
                        for (uint32_t t = 0; t < th; t++) {
                            drow[t][i] = DDE_REC(at[t], (float)prow[t][i], drow[t][i + 1]);
                        }
                    }
                } else {
                    for (uint32_t t = 0; t < th; t++) drow[t][width - 1] = srow[t][width - 1];
                    for (int i = (int)width - 2; i >= 0; i--) {
                        const float *at = a_t + (size_t)i * DDE_VTILE;
                        for (uint32_t t = 0; t < th; t++) {
                            drow[t][i] = DDE_REC(at[t], srow[t][i], drow[t][i + 1]);
                        }
                    }
                }
            }
#endif /* DDE_DIR_MODE */
        }

        /* ── B. 垂直濾波 (tile-based，每次處理 DDE_VTILE 欄) ──
         *
         * 逐欄處理會踩到兩個瓶頸：
         *  (a) p_img / out_h / base 皆為跨列存取，每次前進一列就跳一整條
         *      掃描線，每個存取落在不同 cache line。回頭重用同一條 line 要
         *      等 height 列之後，工作集早已超過 L1 而被逐出。
         *  (b) 遞迴式 yf[i] 依賴 yf[i-1]，關鍵路徑 = N x FMA 延遲，
         *      流水線大部分時間閒置且無法向量化。
         *
         * 分塊同時解決兩者：先把 TILE 欄轉置進連續的 tile-major 暫存區
         * (3 * height * TILE * 4 bytes，height=512、TILE=16 時約 98 KB，可常駐 L2)，
         * 之後 TILE 條互相獨立的遞迴同時推進，彼此掩蓋 FMA 延遲，
         * 且內層迴圈的 TILE 維度可被自動向量化。
         *
         * 每一欄的浮點運算式與結合順序皆與逐欄版本相同，欄與欄之間無資料
         * 交換，故輸出逐位元完全一致 (水平 pass 同理，逐列亦互不相干)。
         * ----------------------------------------------------------- */
        for (uint32_t c0 = 0; c0 < width; c0 += DDE_VTILE) {

            uint32_t tw = width - c0;
            if (tw > DDE_VTILE) tw = DDE_VTILE;

            /* B-1. 轉置蒐集 (僅 out_h)。讀取端每列取 TILE 個相鄰元素，
             *      命中同一條 cache line；寫入端為連續的 tile 暫存區。 */
            for (uint32_t i = 0; i < height; i++) {
                const float *sr = out_h + (size_t)i * width + c0;
                float *st = src_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) st[t] = sr[t];
            }

            /* B-2. 遞迴係數 a_t[i][t]，語意與水平 pass 的 A-2 完全相同。
             *
             * 【OPT-2】原本必須先把 guide 轉置成 float 的 gtile 才能相減；
             * 現在直接在 p_img 上取相鄰兩列同一欄區段。兩列各自的 TILE 個
             * 元素本來就連續 (TILE=16 時各 32 bytes，同一條 cache line)，
             * 跨列只前進一個 line_stride，硬體 prefetch 容易辨識，
             * 因此不轉置也沒有存取懲罰，卻省下 gtile 的整趟寫入與讀取。
             * 差值同樣改為整數運算 (OPT-3)。 */
            for (uint32_t i = 0; i + 1 < height; i++) {
                const uint8_t *p0 = p_img + (size_t)i * line_stride + c0;
                const uint8_t *p1 = p0 + line_stride;
                float *at = a_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    int d = (int)p1[t] - (int)p0[t];
                    if (d < 0) d = -d;
                    if (d > (DIFF_LUT_SIZE - 1)) d = DIFF_LUT_SIZE - 1;
                    at[t] = diff_lut[d];
                }
            }

#if DDE_DIR_MODE == 0
            /* B-3. 正向遞迴。TILE 條獨立的鏈同時推進。
             *      dst_t 指向 gtile (guide 已失效)，與 src_t 不重疊。 */
            {
                const float *s0 = src_t;
                float *d0 = dst_t;
                for (uint32_t t = 0; t < tw; t++) d0[t] = s0[t];
            }
            for (uint32_t i = 1; i < height; i++) {
                const float *at = a_t   + (size_t)(i - 1) * DDE_VTILE;
                const float *st = src_t + (size_t)i * DDE_VTILE;
                const float *dp = dst_t + (size_t)(i - 1) * DDE_VTILE;
                float       *dc = dst_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    dc[t] = DDE_REC(at[t], st[t], dp[t]);
                }
            }

            /* B-4. 反向遞迴 + 就地雙向平均。
             *      Bb 為算出即用，只需保留當前列的 TILE 個值 (ybrow)；
             *      Bf 則已存放於 dst_t 中，兩者於同一次迴圈合成。 */
            {
                size_t off = (size_t)(height - 1) * DDE_VTILE;
                const float *st = src_t + off;
                float       *dc = dst_t + off;
                for (uint32_t t = 0; t < tw; t++) {
                    ybrow[t] = st[t];
                    dc[t] = (dc[t] + ybrow[t]) * 0.5f;
                }
            }
            for (int i = (int)height - 2; i >= 0; i--) {
                const float *at = a_t   + (size_t)i * DDE_VTILE;
                const float *st = src_t + (size_t)i * DDE_VTILE;
                float       *dc = dst_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    ybrow[t] = DDE_REC(at[t], st[t], ybrow[t]);
                    dc[t] = (dc[t] + ybrow[t]) * 0.5f;
                }
            }
#else
            /* B-3/B-4 單向版本。dst_t 仍指向 gtile，與 src_t 不重疊，
             * 兩個方向皆可安全就地寫入。ybrow 在此模式下未被使用。 */
            /* 【OPT-4】單向遞迴可就地進行。遞迴式在位置 i 只用到
             * src_t[i] 與「已經算好的」src_t[i-1] (正向) 或 src_t[i+1]
             * (反向)，兩者的讀取順序與寫入順序一致，因此把結果直接寫回
             * src_t 不會破壞尚未使用的輸入。這省下 dst_t 的整趟寫入，
             * 以及開頭那次邊界複製。雙向模式做不到，因為 A-4/B-4 需要
             * 同時持有 Bf 與 Bb。 */
            if (dir_fwd) {
                for (uint32_t i = 1; i < height; i++) {
                    const float *at = a_t   + (size_t)(i - 1) * DDE_VTILE;
                    const float *sp = src_t + (size_t)(i - 1) * DDE_VTILE;
                    float       *sc = src_t + (size_t)i * DDE_VTILE;
                    for (uint32_t t = 0; t < tw; t++) {
                        sc[t] = DDE_REC(at[t], sc[t], sp[t]);
                    }
                }
            } else {
                for (int i = (int)height - 2; i >= 0; i--) {
                    const float *at = a_t   + (size_t)i * DDE_VTILE;
                    const float *sn = src_t + (size_t)(i + 1) * DDE_VTILE;
                    float       *sc = src_t + (size_t)i * DDE_VTILE;
                    for (uint32_t t = 0; t < tw; t++) {
                        sc[t] = DDE_REC(at[t], sc[t], sn[t]);
                    }
                }
            }
#endif /* DDE_DIR_MODE */

            /* B-5. 寫回。
             *
             * 【OPT-5】最後一次迭代直接在此完成 Step 3 的細節重組：
             *     detail  = I - B
             *     I_comb  = B + gain * detail
             * B 此刻就在暫存區的暫存器裡，原始值 I 也只是同一列的
             * p_img 讀取，因此省下 Step 3 那趟「重讀 1.25 MB base +
             * 跨距重讀 p_img + 再寫回 base」的全幀掃描。
             * num_iters >= 2 時，非最後一次迭代仍寫入純粹的 B。 */
#if DDE_DIR_MODE == 1
            const float *vout = src_t;      /* 就地遞迴，結果留在 src_t */
#else
            const float *vout = dst_t;
#endif
            if (last_iter) {
                for (uint32_t i = 0; i < height; i++) {
                    const float    *dc = vout  + (size_t)i * DDE_VTILE;
                    const uint8_t *pr = p_img + (size_t)i * line_stride + c0;
                    float *br = base + (size_t)i * width + c0;
                    for (uint32_t t = 0; t < tw; t++) {
                        float b = dc[t];
                        float v = b + ((float)pr[t] - b) * gain;
                        br[t] = v;
#if DDE_USING_MAPPING
                        if (v < scan_min) scan_min = v;
                        if (v > scan_max) scan_max = v;
#endif
                    }
                }
            } else {
                for (uint32_t i = 0; i < height; i++) {
                    const float *dc = vout + (size_t)i * DDE_VTILE;
                    float *br = base + (size_t)i * width + c0;
                    for (uint32_t t = 0; t < tw; t++) br[t] = dc[t];
                }
            }
        }
    }

    /* ── Step 3: 細節萃取與增益重組 ───────────────────────────────
     *   detail = I - B ,  I_comb = B + gain * detail
     * 結果就地覆寫 base，不另外配置緩衝區。
     * min/max 在同一趟迴圈中累積：base[idx] 已在暫存器裡，多兩次比較
     * 的成本可忽略，卻能替 Step 4 省下一整趟 1.25 MB 的 DRAM 掃描
     * (實測約佔 DDE 總時間的 7%)。
     *
     * 以 +/-INFINITY 初始化，與 "min = data[0]" 起手在數學上等價：
     * 任何有限值皆滿足 v < +INF 與 v > -INF，故第一個元素必然被寫入。
     * 輸入含 NaN 的情形已由函式入口的參數驗證排除。
     * ------------------------------------------------------------- */
    /* 【OPT-5】本區段已融合進 B-5，此處不再有獨立的全幀掃描。
     * base[] 在最後一次迭代結束時即已持有 I_comb，
     * scan_min / scan_max 亦已於該處累積完畢。 */
    /*for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t idx = (size_t)y * width + x;
            float original = (float)p_img[(size_t)y * line_stride + x];
            float detail   = original - base[idx];
            base[idx] = base[idx] + detail * gain;
        }
    }*/

    /* ── Step 4: 百分位數與線性映射參數 ──────────────────────────
     *
     * 借用已閒置的 out_h 當作 16 KB 直方圖緩衝區。此處不會越界：
     * diff_lut / tile 暫存區在此刻生命週期均已結束。
     *
     * 別名合法性：pWorkBuf 來自 hd_common_mem_alloc，是無宣告型別的配置
     * 儲存空間。依 C11 6.5p6，有效型別由最後一次寫入的左值型別決定，
     * 寫 int 讀 int 為良好定義，不需要 -fno-strict-aliasing。
     *
     * 映射規則 (單一連續公式)：
     *     win    = out_max - out_black
     *     scale  = min(DDE_MAX_STRETCH, win / span)
     *     pad    = (win - span * scale) / 2          未用滿的範圍平均分到兩端
     *     offset = out_black + pad - v_min * scale
     *
     * 端點保證 (兩種情形都成立，v_max 絕不被上限截掉)：
     *     span >  win/MAX_STRETCH : scale = win/span，pad = 0
     *                               -> v_min = out_black，v_max = out_max
     *     span <= win/MAX_STRETCH : scale = MAX_STRETCH，pad > 0
     *                               -> 映射結果置中於 [out_black, out_max]
     *
     * 兩者在 span = win/MAX_STRETCH 處 pad 連續趨近 0，無分支跳變。
     * 置中的用意：均勻場景 (例如對著白牆) 的 span 極小，拉伸被上限夾住後
     * 若仍把 v_min 錨在 out_black，畫面會塌到範圍底部而偏黑；置中則呈現
     * 中灰，符合預期。
     *
     * 【重要】scale 必須允許大於 1。
     * 早期版本寫成 scale = min(1, win/span)，理由是「不放大以免拉伸雜訊」，
     * 但那與「把 v_min 錨定到 out_black」互相矛盾：熱像的原始動態範圍
     * (實測約 5275 DN) 遠小於輸出視窗 (16383)，於是 scale 恆為 1，
     * 影像沒有被拉伸卻仍被減掉約 4500 DN 的黑電平，整幅平移到範圍底部，
     * 平均亮度僅 11%，下游 ISP 渲染出來即為全黑。
     * gain 較大時 (>= 20) 細節增益把 span 撐過 16383 才會誤打誤撞正常，
     * 因此該缺陷只在小 gain 下顯現。
     *
     * out_black 必須是 ISP 端約定的「固定」黑電平，不可用逐幀算出的 v_min。
     * 後者會隨場景漂移，而下游 ISP 的 OB 減除與 CLAHE 的 bin 分配都是固定的，
     * 游移的黑電平會表現為畫面亮度呼吸 (pumping)。
     * ------------------------------------------------------------- */

#if DDE_USING_MAPPING
        float v_min, v_max;
        int *hist_buf = (int *)out_h;

        compute_percentiles(base, num_pixels, scan_min, scan_max, 1.0f, 99.0f, &v_min, &v_max, hist_buf);

        const float f_out_max   = (float)out_max;
        const float f_out_black = (float)out_black;

        float win = f_out_max - f_out_black;
        if (win < 1.0f) win = 1.0f;             /* 防呆，正常不會觸發 */

        float span  = v_max - v_min;
        float scale = DDE_MAX_STRETCH;
        if (span > 1e-3f) {
            scale = win / span;
            if (scale > DDE_MAX_STRETCH) {      /* 僅設上限，不禁止擴張 */
                scale = DDE_MAX_STRETCH;
            }
        }

        /* 未用滿的輸出範圍平均分配到兩端 (置中)。
         * 未觸及上限時 span*scale == win，置中項為 0，行為與純錨定完全相同，
         * 因此公式在 scale 觸頂的臨界點是連續的，不會有畫面跳變。 */
        float pad    = (win - span * scale) * 0.5f;
        if (pad < 0.0f) pad = 0.0f;             /* 數值防護，理論上不會發生 */
        float offset = f_out_black + pad - v_min * scale;

#else
        // ── 除錯路徑：不做百分位、不做映射 ──
        const float f_out_max   = (float)out_max;
        const float f_out_black = out_black;          //夾擠下限固定為 0，保留原始 DN
        const float scale  = 1.0f;
        const float offset = 0.0f;

#endif /* DDE_USING_MAPPING */


    /* ── Step 5: 寫回原影像 ─────────────────────────────────────── */
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            float val = base[(size_t)y * width + x] * scale + offset;

            /* 僅百分位視窗外的離群值 (約 2%) 會被夾擠，此為刻意的
             * percentile clipping。
             *
             * 使用否定形式 !(val > lo) 而非 (val < lo)：對 NaN 而言前者為
             * true、後者為 false，可確保任何殘餘 NaN 被導向 out_black，
             * 而非造成 (uint8_t) 轉型的未定義行為。比較次數相同，零成本。 */
            if (!(val > f_out_black)) val = f_out_black;
            if (val > f_out_max)      val = f_out_max;

            /* 四捨五入取代直接截斷，消除 -0.5 LSB 的系統性偏壓 */
            p_img[(size_t)y * line_stride + x] = (uint8_t)(val + 0.5f);
        }
    }

    return 0;
#endif /* DDE_FUSED */
}

uint32_t dde_dt_scaled_workbuf_size(uint32_t width, uint32_t height, uint32_t scale)
{
    if (scale == 1) return dde_dt_get_workbuf_size(width, height);
#if DDE_FUSED
    if (scale != 2 || !dde_dt_get_workbuf_size(width, height)) return 0;
    uint32_t w = (width + 1U) / 2U, h = (height + 1U) / 2U;
    uint32_t guide_bytes = (w * h + 3U) & ~3U;
    return guide_bytes + dde_dt_get_workbuf_size(w, h) +
        (width + height) * (uint32_t)sizeof(DDE_SCALE_AXIS) + width * 2U * (uint32_t)sizeof(float);
#else
    return 0;
#endif
}

int DDE_DT_Optimized_Scaled(
    uint8_t *p_img, uint32_t width, uint32_t height, uint32_t stride,
    float sigma_s, float sigma_r, float gain, int num_iters,
    uint8_t out_max, uint8_t out_black, uint32_t scale, void *work)
{
    if (scale == 1) return DDE_DT_Optimized(p_img, width, height, stride,
        sigma_s, sigma_r, gain, num_iters, out_max, out_black, work);
    if (scale != 2) return -6;
#if DDE_FUSED
    if (!p_img || !work || !dde_dt_scaled_workbuf_size(width, height, scale) ||
        stride < width || (uint64_t)stride * height > UINT32_MAX) return -1;
    if (out_max <= out_black) return -2;
    if (!isfinite(sigma_s) || !isfinite(sigma_r) || !isfinite(gain) ||
        sigma_s < 0.01f || sigma_s > 4096.0f || sigma_r < 0.01f ||
        sigma_r > 255.0f || gain > 10.0f) return -3;
    if (num_iters < 1 || num_iters > 8) return -4;
    if (!(gain >= 0.0f)) return -5;
    uint32_t w = (width + 1U) / 2U, h = (height + 1U) / 2U;
    uint32_t guide_bytes = (w * h + 3U) & ~3U;
    uint8_t *guide = (uint8_t *)work;
    void *core_work = guide + guide_bytes;
    float *base = (float *)core_work;
    DDE_SCALE_AXIS *ax = (DDE_SCALE_AXIS *)((uint8_t *)core_work + dde_dt_get_workbuf_size(w, h));
    DDE_SCALE_AXIS *ay = ax + width;
    float *row0 = (float *)(ay + height);
    float *row1 = row0 + width;
    uint32_t row0_index = UINT32_MAX, row1_index = UINT32_MAX;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sum = 0, count = 0;
            for (uint32_t dy = 0; dy < 2 && y*2+dy < height; dy++)
                for (uint32_t dx = 0; dx < 2 && x*2+dx < width; dx++) {
                    sum += p_img[(size_t)(y*2+dy) * stride + x*2+dx];
                    count++;
                }
            guide[y*w+x] = (uint8_t)((sum + count/2U) / count);
        }
    }
    /* Every pass is completed at reduced resolution. Keep float precision until
     * full-resolution detail recombination; do not quantize the base to Y8. */
    dde_run_fused(guide, w, h, w, sigma_s * 0.5f, sigma_r, 0.0f,
        num_iters, out_max, out_black, 0, core_work);
    dde_half_axis(ax, width, w);
    dde_half_axis(ay, height, h);
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *pixels = p_img + (size_t)y * stride;
        /* Expand each low-resolution row horizontally only once, then reuse
         * two contiguous rows for vertical interpolation and SIMD output. */
        if (row1_index == ay[y].lo) {
            float *swap = row0; row0 = row1; row1 = swap;
            uint32_t index = row0_index; row0_index = row1_index; row1_index = index;
        }
        if (row0_index != ay[y].lo) {
            const float *small = base + (size_t)ay[y].lo * w;
            for (uint32_t x = 0; x < width; x++)
                row0[x] = small[ax[x].lo] + (small[ax[x].hi] - small[ax[x].lo]) * ax[x].weight;
            row0_index = ay[y].lo;
        }
        if (ay[y].hi != row0_index && row1_index != ay[y].hi) {
            const float *small = base + (size_t)ay[y].hi * w;
            for (uint32_t x = 0; x < width; x++)
                row1[x] = small[ax[x].lo] + (small[ax[x].hi] - small[ax[x].lo]) * ax[x].weight;
            row1_index = ay[y].hi;
        }
        const float *top = row0;
        const float *bottom = ay[y].hi == row0_index ? row0 : row1;
        float fy = ay[y].weight;
        uint32_t x = 0;
#if DDE_SIMD4
        const dde_f4 black4 = {out_black, out_black, out_black, out_black};
        const dde_f4 max4 = {out_max, out_max, out_max, out_max};
        for (; x + 4U <= width; x += 4U) {
            dde_f4 t = dde_load4(top + x);
            dde_f4 b = dde_load4(bottom + x);
            dde_f4 smooth = t + (b - t) * fy;
            dde_i4 original = {pixels[x], pixels[x+1], pixels[x+2], pixels[x+3]};
            dde_f4 value = smooth + (__builtin_convertvector(original, dde_f4) - smooth) * gain;
            value = dde_select4(value > black4, value, black4);
            value = dde_select4(value > max4, max4, value);
            dde_i4 rounded = __builtin_convertvector(value + 0.5f, dde_i4);
            for (uint32_t lane = 0; lane < 4U; lane++) pixels[x+lane] = (uint8_t)rounded[lane];
        }
#endif
        for (; x < width; x++) {
            float t = top[x];
            float b = bottom[x];
            float smooth = t + (b - t) * fy;
            float value = smooth + ((float)pixels[x] - smooth) * gain;
            if (!(value > out_black)) value = out_black;
            if (value > out_max) value = out_max;
            pixels[x] = (uint8_t)(value + 0.5f);
        }
    }
    return 0;
#else
    return -6;
#endif
}

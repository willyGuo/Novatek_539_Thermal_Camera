#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// 限制裁剪並重新分配直方圖像素
void clipHistogram(uint32_t* hist, uint32_t nrBins, uint32_t clipLimit, uint32_t nrPixels) {
    uint32_t excess = 0;
    
    // 1. 計算超出剪切強度的像素總數并剪切
    for (uint32_t i = 0; i < nrBins; i++) {
        if (hist[i] > clipLimit) {
            excess += (hist[i] - clipLimit);
            hist[i] = clipLimit;
        }
    }

    // 2. 將多餘的像素平均分配到所有 Bin 中
    uint32_t binIncr = excess / nrBins;
    uint32_t upperLimit = clipLimit - binIncr;

    for (uint32_t i = 0; i < nrBins; i++) {
        if (hist[i] <= upperLimit) {
            hist[i] += binIncr;
            excess -= binIncr;
        } else if (hist[i] < clipLimit) {
            excess -= (clipLimit - hist[i]);
            hist[i] = clipLimit;
        }
    }

    // 3. 如果還有剩餘像素（因除不盡），隨機/均勻地散佈到直方圖中
    while (excess > 0) {
        for (uint32_t i = 0; i < nrBins && excess > 0; i++) {
            if (hist[i] < clipLimit) {
                hist[i]++;
                excess--;
            }
        }
    }
}

// 根據直方圖計算累積分布函數 (CDF) 對應的查找表 (LUT)
void makeLut(uint32_t* hist, uint32_t nrBins, uint16_t minVal, uint16_t maxVal, uint32_t nrPixels, uint16_t* lut) {
    uint32_t sum = 0;
    float scale = (float)(maxVal - minVal) / nrPixels;

    for (uint32_t i = 0; i < nrBins; i++) {
        sum += hist[i];
        uint32_t val = minVal + (uint32_t)(sum * scale);
        lut[i] = (val > maxVal) ? maxVal : val;
    }
}

// CLAHE 主函數
// src: 輸入圖像, dst: 輸出圖像, width: 寬, height: 高
// gridX/gridY: 水平和垂直方向的分塊數量 (例如 8, 8)
// clipLimitRatio: 剪切係數上限 (常見範圍 1.0 ~ 4.0，2.0 較常用)
int clahe(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t gridX, uint32_t gridY, uint32_t clipLimit) 
{
    
    if (!src || !dst || width == 0 || height == 0 || gridX == 0 || gridY == 0) 
    {
        printf("src = 0x%p, dst = 0x%p, width = %u, height = %u, gridX = %u, gridY =%d\r\n",(void *)src, (void *)dst, width, height, gridX, gridY);
        return -1;
    }
    uint32_t nrBins = 256;
    uint32_t tileSizeX = width / gridX;
    uint32_t tileSizeY = height / gridY;
    uint32_t nrPixelsInTile = tileSizeX * tileSizeY;

    // 計算實際的直方圖剪切像素閾值
    //uint32_t clipLimit = (uint32_t)(clipLimitRatio * ((float)nrPixelsInTile / nrBins));
    if (clipLimit < 1) clipLimit = 1;

    // 分配記憶體儲存每個網格塊的查找表 (LUT)
    uint16_t* luts = (uint16_t*)malloc(gridX * gridY * nrBins * sizeof(uint16_t));
    uint32_t* tileHist = (uint32_t*)malloc(nrBins * sizeof(uint32_t));

    if (!luts || !tileHist) {
        free(luts); free(tileHist);
        printf("malloc(luts) or malloc(tileHist) are not successful!!!\r\n\r\n");
        return -2;
    }

    // 第一階段：計算所有分塊的局部直方圖、裁剪並產生對應的映射表 (LUT)
    for (uint32_t gy = 0; gy < gridY; gy++) {
        for (uint32_t gx = 0; gx < gridX; gx++) {
            memset(tileHist, 0, nrBins * sizeof(uint32_t));

            // 計算當前 Tile 在圖像中的起始像素坐標
            uint32_t startX = gx * tileSizeX;
            uint32_t startY = gy * tileSizeY;

            // 統計直方圖
            for (uint32_t y = 0; y < tileSizeY; y++) {
                for (uint32_t x = 0; x < tileSizeX; x++) {
                    uint16_t pixelVal = src[(startY + y) * width + (startX + x)];
                    tileHist[pixelVal]++;
                }
            }

            // 限制直方圖對比度
            clipHistogram(tileHist, nrBins, clipLimit, nrPixelsInTile);

            // 生成映射 LUT
            uint16_t* currentLut = &luts[(gy * gridX + gx) * nrBins];
            makeLut(tileHist, nrBins, 0, 255, nrPixelsInTile, currentLut);
        }
    }

    // 第二階段：遍歷所有像素，使用雙線性插值 (Bilinear Interpolation) 消除區塊邊界感
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t pixelVal = src[y * width + x];

            // 尋找像素在網格中心坐標系中的相對位置
            float tx = ((float)x - (float)tileSizeX / 2.0f) / (float)tileSizeX;
            float ty = ((float)y - (float)tileSizeY / 2.0f) / (float)tileSizeY;

            // 獲取左上角網格索引
            int gx0 = (int)tx;
            int gy0 = (int)ty;

            // 處理邊界越界情況
            if (gx0 < 0) gx0 = 0;
            if (gx0 >= (int)gridX - 1) gx0 = gridX - 2;
            if (gy0 < 0) gy0 = 0;
            if (gy0 >= (int)gridY - 1) gy0 = gridY - 2;

            // 計算插值權重比例系数 [0, 1]
            float fx = tx - gx0;
            float fy = ty - gy0;
            if(fx < 0) fx = 0; 
            if(fx > 1) fx = 1;
            if(fy < 0) fy = 0;
            if(fy > 1) fy = 1;
            // 取得相鄰四個網格塊的映射值
            uint16_t v00 = luts[((gy0)     * gridX + (gx0))     * nrBins + pixelVal];
            uint16_t v10 = luts[((gy0)     * gridX + (gx0 + 1)) * nrBins + pixelVal];
            uint16_t v01 = luts[((gy0 + 1) * gridX + (gx0))     * nrBins + pixelVal];
            uint16_t v11 = luts[((gy0 + 1) * gridX + (gx0 + 1)) * nrBins + pixelVal];

            // 雙線性插值公式
            float res = (1.0f - fx) * (1.0f - fy) * v00 +
                        fx         * (1.0f - fy) * v10 +
                        (1.0f - fx) * fy         * v01 +
                        fx         * fy         * v11;

            dst[y * width + x] = (uint8_t)(res + 0.5f); // 四捨五入
        }
    }

    free(tileHist);
    free(luts);
    return 0;
}

uint8_t claheDisable = 0;
uint32_t clipLimit = 8; 
uint32_t gridX = 20, gridY = 16;
int clahe_apply(const uint8_t *input, uint8_t *output, uint32_t width, uint32_t height)
{
    int status;
    if(claheDisable == 0)
        status = clahe(input, output, width, height, gridX, gridY, clipLimit);
    else
        status = 1;

    return status;
}

static int read_clahe_option(void)
{
	char buf[32];
	char *end;
	unsigned long value;

	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == NULL) {
		return -1;
	}
	if (buf[0] == 'q' || buf[0] == 'Q') {
		return -1;
	}

	value = strtoul(buf, &end, 0);
	if (end == buf) {
		if (buf[0] != '\n' && buf[0] != '\r') {
			printf("Invalid option\n");
		}
		return -2;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}
	if (*end != '\0' || value > 0x7fffffff) {
		printf("Invalid option\n");
		return -2;
	}

	return (int)value;
}

int clahe_Set_Clip_Limit_menu(void)
{
	int option;
	int running = 1;
	int ret = 0;

    while (running) 
   {
		printf("----------------------------------------\n");
		printf("Please Input CLAHE Clip Limit Value (4 - 128) ......)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
//		printf("selected runtime AGC: %s\n", agc_mode_name(agc_mode_get()));
//		printf("Settings: CFG Setting -> AGC setting\n");

		option = read_clahe_option();
		if(option > 128)
            clipLimit = 128;
        else if(option <= 0)
            running = 0;
        else if(option < 4)
            clipLimit = 4;
        else
            clipLimit = option;

        printf("CLAHE clip limit is %d\r\n", clipLimit);	
    }

	return ret;           
}

int clahe_Set_Tile_Size_menu(void)
{
	int option;
	int running = 1;
	int ret = 0;

while (running) {
		printf("----------------------------------------\n");
		printf("CLAHE Tile Size Setting menu)\n");
		printf(" 1. Tile Size = (8x8)\n");
		printf(" 2. Tile Size = (16*16)\n");
        printf(" 3. Tile Size = (32x32)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
//		printf("selected runtime AGC: %s\n", agc_mode_name(agc_mode_get()));
//		printf("Settings: CFG Setting -> AGC setting\n");

		option = read_clahe_option();
		switch (option) {
		case 1:
            gridX = 80, gridY = 64;
            printf("CLAHE Tile Size is (8x8)\r\n");			
			break;
		case 2:
            gridX = 40, gridY = 32;
            printf("CLAHE Tile Size is (16x16)\r\n");	
			break;
        case 3 :
            gridX = 20, gridY = 16;
            printf("CLAHE Tile Size is (32x32)\r\n");
            break;           
		case 0:
		case -1:
			running = 0;
			break;

		case -2 :
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;       
}

int clahe_Enable_Disable_Select_menu(void)
{
	int option;
	int running = 1;
	int ret = 0;

while (running) {
		printf("----------------------------------------\n");
		printf("CLAHE Tile Size Setting menu)\n");
		printf(" 1. CLAHE : On\n");
		printf(" 2. CLAHE : off\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		option = read_clahe_option();
		switch (option) {
		case 1:	
            claheDisable = 0;
            printf("CLAHE : On\r\n");
			break;
		case 2:
            claheDisable = 1;
            printf("CLAHE : off\n");
			break;
		case 0:
		case -1:
			running = 0;
			break;

		case -2 :
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;       
}

int clahe_clip_limit_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("CLAHE menu)\n");
		printf(" 1. Increase Clip Limit\n");
		printf(" 2. Decrease Clip Limit\n");
        printf(" 3. Set Clip Limit\n");
        printf(" 4. Set Tile Size\n");
        printf(" 5. CLAHE on/off\n");       
        printf(" 6. Show CLAHE configuration\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
//		printf("selected runtime AGC: %s\n", agc_mode_name(agc_mode_get()));
//		printf("Settings: CFG Setting -> AGC setting\n");

		option = read_clahe_option();
		switch (option) {
		case 1:
            if(clipLimit < 64)
                clipLimit++;

            printf("CLAHE clip limit is %d\r\n", clipLimit);			
			break;
		case 2:
            if(clipLimit > 0)
                clipLimit--;

            printf("CLAHE clip limit is %d\r\n", clipLimit);		
			break;
        case 3 :
            clahe_Set_Clip_Limit_menu();
            break;    
        case 4 :
            clahe_Set_Tile_Size_menu();
            break;
        case 5 :
            clahe_Enable_Disable_Select_menu();
            break; 
        case 6 :
            if(claheDisable == 0)
            {
                printf("CLAHE enable!!!\r\n");
                printf("The CLAHE tile size is (%dx%d).\r\n", (uint32_t)(640/gridX), (uint32_t)(512/gridY));
                printf("The CLAHE clip limit is %d.\r\n", clipLimit);
            }
            else
                printf("CLAHE disable!!!\r\n");
            break;            
		case 0:
		case -1:
			running = 0;
			break;

		case -2 :
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;    
}


#ifdef USE_CLAHE_MAIN
int clahe_main() {
    // 範例：創建一個 16x16 像素的簡易灰階漸層圖像測試
    uint32_t w = 640, h = 512;
    uint8_t* inputImg = (uint8_t*)malloc(w * h);
    uint8_t* outputImg = (uint8_t*)malloc(w * h);

    for (uint32_t i = 0; i < w * h; i++) {
        inputImg[i] = i % 256; 
    }

    // 運行 CLAHE：圖像切成 2x2 網格，對比度剪切系數設為 2.0
    int status = clahe(inputImg, outputImg, w, h, 2, 2, 2.0f);

    if (status == 0) {
        printf("CLAHE 處理成功！第一行輸出像素值:\n");
        for (uint32_t i = 0; i < w; i++) {
            printf("%d ", outputImg[i]);
        }
        printf("\n");
    } else {
        printf("處理失敗，錯誤碼: %d\n", status);
    }

    free(inputImg);
    free(outputImg);
    return 0;
}
#endif
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <string.h>


#define Temp_I2C_ADDRESS         0x49  // tmp102 I2C 地址
#define I2C_M_RD 0x0001

static const char *i2c_device = "/dev/i2c-2";
static int fd = -1;

/* //struct i2c_rdwr_ioctl_data和struct i2c_msg，和內核一致
struct i2c_msg
        {
                unsigned short addr;
                unsigned short flags;
        #define I2C_M_TEN 0x0010
        #define I2C_M_RD 0x0001
                unsigned short len;
                unsigned char *buf;
        };


struct i2c_rdwr_ioctl_data
        {
                struct i2c_msg *msgs;
                int nmsgs;
        // nmsgs這個數量决定了有多少開始信號，對於“單開始時序”，取1
        };
*/


// 1. 封裝 I2C 多位元組讀取函數 (Repeated Start)
int i2c_read(uint8_t reg_addr, uint8_t *read_data, uint16_t len) {
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data msg_data;

    if (fd < 0) {
        fprintf(stderr, "I2C device is not initialized\n");
        return -1;
    }
	
	// 第一個訊息：向設備發送要讀取的起始暫存器位址 (寫入模式)
    msgs[0].addr  = Temp_I2C_ADDRESS;
    msgs[0].flags = 0;				// Write mode
    msgs[0].len   = 1;				// Write 要發送的位元數
    msgs[0].buf   = &reg_addr;
	
	
    // 第二個訊息：從該暫存器開始連續讀取 len 個位元組
    msgs[1].addr  = Temp_I2C_ADDRESS;
    msgs[1].flags = I2C_M_RD;		// Read mode = 1
    msgs[1].len   = len;			// Read 要接收的位元數
    msgs[1].buf   = read_data;

    msg_data.msgs  = msgs;
    msg_data.nmsgs = 2; 			// 結合這兩個訊息

    if (ioctl(fd, I2C_RDWR, &msg_data) < 0) {
        perror("I2C_RDWR 連續讀取失敗");
        return -1;
    }
	
	
	printf("讀取成功！讀到的數據為: ");
	for(int i = 0; i < len; i++) {
		printf("0x%02X ", read_data[i]);
	}
	printf("\n");
	
    return 0;
}

// 2. 封裝 I2C 多位元組寫入函數
int i2c_write(uint8_t reg_addr, uint8_t *write_data, uint16_t len) {
    struct i2c_msg msg;
    struct i2c_rdwr_ioctl_data msg_data;

    if (fd < 0) {
        fprintf(stderr, "I2C device is not initialized\n");
        return -1;
    }
    
    // 建立一個緩衝區，大小為：欲寫入的資料長度(要包含 register address)
    uint8_t *buf = (uint8_t *)malloc(len);
    if (buf == NULL) {
        perror("記憶體配置失敗");
        return -1;
    }

    buf[0] = reg_addr;             // 第一個位元組是暫存器位址
    memcpy(&buf[1], write_data, len);    // 放入要傳送的資料
	

    msg.addr  = Temp_I2C_ADDRESS;
    msg.flags = 0;                 // write
    msg.len   = 1 + len;           	   // 總共要發送的總位元組數
    msg.buf   = buf;

    msg_data.msgs  = &msg;
    msg_data.nmsgs = 1;				// nmsgs 這個數量决定了有多少開始信號,一般 write 時為 1 ；read 時為 2

    if (ioctl(fd, I2C_RDWR, &msg_data) < 0) {
        perror("I2C_RDWR 連續寫入失敗");
        free(buf);
        return -1;
    }
	
	// printf("寫入成功！數據為: ");
	// for(int i = 0; i < (len+1); i++) {
	// 	printf("0x%02X ", write_data[i]);
	// }
	// printf("\n");

    free(buf);
    return 0;
}

int I2C_0_Init(void) {
    if (fd >= 0) {
        return 0;
    }


    // 打開 I2C 控制器設備檔案
    fd = open(i2c_device, O_RDWR);
    if (fd < 0) {
        perror("無法打開 I2C 設備檔案");
        return -1;
    }
	
    printf("成功打開 %s\n", i2c_device);
    printf("---------------------------------------\n");
	
    return 0;
}

void I2C_0_Deinit(void) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

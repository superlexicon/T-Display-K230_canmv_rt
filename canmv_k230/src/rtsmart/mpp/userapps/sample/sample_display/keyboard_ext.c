#include <math.h>
//#include <vector>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
//#include <rtthread.h>
//#include <rtdevice.h>
#include <poll.h>
#include "fpioa/rt_fpioa.h"
#include "keyboard_ext.h"
//using namespace std;
#include<lvgl.h>
/* ioctl */
#define	GPIO_DM_OUTPUT           _IOW('G', 0, int)
#define	GPIO_DM_INPUT            _IOW('G', 1, int)
#define	GPIO_DM_INPUT_PULL_UP    _IOW('G', 2, int)
#define	GPIO_DM_INPUT_PULL_DOWN  _IOW('G', 3, int)
#define	GPIO_WRITE_LOW           _IOW('G', 4, int)
#define	GPIO_WRITE_HIGH          _IOW('G', 5, int)

#define	GPIO_PE_RISING           _IOW('G', 7, int)
#define	GPIO_PE_FALLING          _IOW('G', 8, int)
#define	GPIO_PE_BOTH             _IOW('G', 9, int)
#define	GPIO_PE_HIGH             _IOW('G', 10, int)
#define	GPIO_PE_LOW              _IOW('G', 11, int)

#define RT_I2C_DEV_CTRL_10BIT (0x800 + 0x01)
#define RT_I2C_DEV_CTRL_TIMEOUT (0x800 + 0x03)
#define RT_I2C_DEV_CTRL_RW (0x800 + 0x04)
#define RT_I2C_DEV_CTRL_CLK (0x800 + 0x05)

typedef struct {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;

} i2c_msg_t;

typedef struct {
    i2c_msg_t *msgs;
    size_t number;
} i2c_priv_data_t;

#define	KD_PWM_CMD_ENABLE           _IOW('P', 0, int)
#define	KD_PWM_CMD_DISABLE          _IOW('P', 1, int)
#define	KD_PWM_CMD_SET              _IOW('P', 2, int)
#define	KD_PWM_CMD_GET              _IOW('P', 3, int)
typedef struct
{
    unsigned int channel; /* 0-n */
    unsigned int period;  /* unit:ns 1ns~4.29s:1Ghz~0.23hz */
    unsigned int pulse;   /* unit:ns (pulse<=period) */
} pwm_config_t;
int flag_press_fn=0;
static int flag_press_shift=0;
int flag_press_caps_lock=0;
static int fd_keyboard=-1;
static int s_fd_i2c4=-1;
static bool s_kb_connected=false;
static bool s_bq25896_adc_started=false;
static int g_kb_backlight_duty = 10;

static int ensure_i2c4_open(void)
{
    if (s_fd_i2c4 > 0) {
        return s_fd_i2c4;
    }
    fpioa_set_function(46, IIC4_SCL, -1, -1, -1, -1, -1, -1, -1);
    fpioa_set_function(47, IIC4_SDA, -1, -1, -1, -1, -1, -1, -1);
    s_fd_i2c4 = open("/dev/i2c4", O_RDWR);
    if (s_fd_i2c4 > 0) {
        uint32_t speed = 100000;
        ioctl(s_fd_i2c4, RT_I2C_DEV_CTRL_CLK, &speed);
    }
    return s_fd_i2c4;
}

static int i2c4_read_bytes(int fd, uint8_t dev_addr, uint8_t reg_addr, uint8_t *reg_data, int length)
{
    if (fd <= 0) return -1;
    i2c_msg_t msgs[2] = {
        {.addr = dev_addr, .flags = 0, .len = 1, .buf = &reg_addr},
        {.addr = dev_addr, .flags = 1, .len = (uint16_t)length, .buf = reg_data},
    };
    i2c_priv_data_t privdata = {msgs, 2};
    return (ioctl(fd, RT_I2C_DEV_CTRL_RW, &privdata) == 0) ? 0 : -1;
}

static int i2c4_write_reg_byte(int fd, uint8_t dev_addr, uint8_t reg_addr, uint8_t reg_data)
{
    if (fd <= 0) return -1;
    uint8_t buff[2] = {reg_addr, reg_data};
    i2c_msg_t msgs[1] = {
        {.addr = dev_addr, .flags = 0, .len = 2, .buf = buff},
    };
    i2c_priv_data_t privdata = {msgs, 1};
    return (ioctl(fd, RT_I2C_DEV_CTRL_RW, &privdata) == 0) ? 0 : -1;
}

static int tca8418_read_reg(int fd, uint8_t reg_addr,
        uint8_t *reg_data, int length)
{
    int ret=0;
        i2c_msg_t msgs[2]=
        {
        {
        .addr=0x34, 
        .flags=0, 
        .len=1, 
        .buf=&reg_addr
        },
        {
        .addr=0x34, 
        .flags=1, 
        .len=(uint16_t)length, 
        .buf=reg_data
        }
        };
        i2c_priv_data_t privdata={msgs, 2};
        if (ioctl(fd, RT_I2C_DEV_CTRL_RW, &privdata) != 0){ //读写数据
            ret=-1;
        }

    return ret;
}

static int tca8418_write_reg(int fd, uint8_t reg_addr,
        uint8_t reg_data)
{
    int ret=0;
    uint8_t buff[2];
    buff[0]=reg_addr;
    buff[1]=reg_data;
    i2c_msg_t msgs[1]=
        {
        {
        .addr=0x34, 
        .flags=0, 
        .len=2, 
        .buf=buff
        }
        
        };
        i2c_priv_data_t privdata={msgs, 1};
        if (ioctl(fd, RT_I2C_DEV_CTRL_RW, &privdata) != 0){ //读写数据
            ret=-1;
        }
    return ret;
}

static int xl9555_write_reg(int fd, uint8_t reg_addr,
        uint8_t reg_data)
{
    int ret=0;
    uint8_t buff[2];
    buff[0]=reg_addr;
    buff[1]=reg_data;
    i2c_msg_t msgs[1]=
        {
        {
        .addr=0x20, 
        .flags=0, 
        .len=2, 
        .buf=buff
        }
        
        };
        i2c_priv_data_t privdata={msgs, 1};
        if (ioctl(fd, RT_I2C_DEV_CTRL_RW, &privdata) != 0){ //读写数据
            ret=-1;
        }
    return ret;
}
static const uint32_t tca8418_keyboard_lvgl[] =
    {
        LV_KEY_RIGHT,LV_KEY_LEFT,0x8E,0x00,' ',LV_KEY_NEXT,0x90,0x8F,0x8E,0x8B,
	  0xA2,LV_KEY_DOWN,'m',' ','v','c','x','z',0x8C,'q',
	  LV_KEY_ENTER,LV_KEY_UP,0x8D,'n','b','f','d','s','a',0x00,
	   0x00,'l','k','j','h','g','r','e','w',LV_KEY_ESC,
	   LV_KEY_BACKSPACE,'p','o','i','u','y','t','2','1',0x81,
	   '0','9','8','7','6','5','4','3',0x83,0x82,
	   0x91,0x8A,0x89,0x88,0x87,0x86,0x85,0x84
	   };
static const uint32_t tca8418_keyboard_lvgl_shift[] =
    {
        LV_KEY_RIGHT,LV_KEY_LEFT,0x8E,0x00,' ',LV_KEY_NEXT,0x90,0x8F,0x8E,0x8B,
	  0xA2,LV_KEY_DOWN,'>',' ','v','c','x','z',0x8C,'\'',
	  LV_KEY_ENTER,LV_KEY_UP,0x8D,'<','.','{',']','[','~',0x00,
	   0x00,'?','/','`','\'','}','+','-','_',LV_KEY_ESC,
	   LV_KEY_BACKSPACE,'"',':',';','|','\\','=','@','!',0x81,
	   ')','(','*','&','^','%','$','#',0x83,0x82,
	   0x91,0x8A,0x89,0x88,0x87,0x86,0x85,0x84
	   };
void tca8418_init()
{
//return;
 int gpio_fd = open("/dev/gpio", O_RDWR);
    if (gpio_fd>0)
    {
      //reset tca8418
      pin_gpio_t pin_43;
      pin_43.pin = 43;
      ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_43);  //pin43 output
      ioctl(gpio_fd, GPIO_WRITE_HIGH, &pin_43);//
      usleep(10000);
      ioctl(gpio_fd, GPIO_WRITE_LOW, &pin_43);//
      usleep(10000);
      ioctl(gpio_fd, GPIO_WRITE_HIGH, &pin_43);//
      close(gpio_fd);          
    }
 

    fd_keyboard = ensure_i2c4_open();
    if (fd_keyboard > 0)
    {
    //init_tca8418
    int ret;
    uint8_t reg_data;
    /////////////////////////////
    reg_data=0x00;
    ret=tca8418_write_reg(fd_keyboard, 0x23,reg_data);//
    printf("tca8418_write_reg 0x23,ret:%d\n",ret);
    reg_data=0x00;
    ret=tca8418_write_reg(fd_keyboard, 0x24,reg_data);//
    reg_data=0x00;
    ret=tca8418_write_reg(fd_keyboard, 0x25,reg_data);//
    
    reg_data=0xFF;
    ret=tca8418_write_reg(fd_keyboard, 0x20,reg_data);//
    reg_data=0xFF;
    ret=tca8418_write_reg(fd_keyboard, 0x21,reg_data);//
    reg_data=0xFF;
    ret=tca8418_write_reg(fd_keyboard, 0x22,reg_data);//
    
    reg_data=0x00;
    ret=tca8418_write_reg(fd_keyboard, 0x26,reg_data);//   
    reg_data=0x00;
    ret=tca8418_write_reg(fd_keyboard, 0x27,reg_data);//
    reg_data=0x00;
    ret=tca8418_write_reg(fd_keyboard, 0x28,reg_data);//
    
    reg_data=0xFF;
    ret=tca8418_write_reg(fd_keyboard, 0x1A,reg_data);
    reg_data=0xFF;
    ret=tca8418_write_reg(fd_keyboard, 0x1B,reg_data);
    reg_data=0xFF;
    ret=tca8418_write_reg(fd_keyboard, 0x1C,reg_data);    
    
    /////////////////////////////////////////////////////////////////////////
    reg_data=0x7F;
    ret=tca8418_write_reg(fd_keyboard, 0x1D,reg_data);//KP_GPIO1 row0-6
    //printf("tca8418_write_reg ret=%d\n",ret);
    reg_data=0xFF;
    tca8418_write_reg(fd_keyboard, 0x1E,reg_data);//KP_GPIO2 col0-7
    reg_data=0x03;
    tca8418_write_reg(fd_keyboard, 0x1F,reg_data);//KP_GPIO3 col8-9

    reg_data=0x00; 
    ret=tca8418_read_reg(fd_keyboard, 0x01,&reg_data,1);//CFG   read
    printf("tca8418_read_reg 0x01 ret=%d\n",ret);
    //test keyboard exist
    if(ret==-1)
    {
    s_kb_connected = false;
    fd_keyboard = -1;
    printf("keyboard is not exist\n");
    return;
    }
    s_kb_connected = true;
    printf("detect keyboard device\n");
    //printf("addr1 0x01 and data=%#X\n", reg_data);
    reg_data=(reg_data&0xF0)|0x01;//key_event
    //printf("addr2 0x01 and data=%#X\n", reg_data);
    tca8418_write_reg(fd_keyboard, 0x01,reg_data);//CFG
    printf("addr3 0x01 and data=%#X\n", reg_data);
    reg_data=0x01;
    tca8418_write_reg(fd_keyboard, 0x02,reg_data);//
	}

}
bool tca8418_is_connected()
{
    return s_kb_connected && (fd_keyboard > 0);
}

bool tca8418_poll_presence(void)
{
    uint8_t reg_data = 0;
    int fd = ensure_i2c4_open();
    if (fd <= 0) {
        return false;
    }
    if (s_kb_connected && fd_keyboard > 0) {
        if (tca8418_read_reg(fd_keyboard, 0x01, &reg_data, 1) != 0) {
            s_kb_connected = false;
            fd_keyboard = -1;
            printf("[KB] Keyboard detached from I2C4\n");
            return false;
        }
        return true;
    } else {
        int ret = tca8418_read_reg(fd, 0x01, &reg_data, 1);
        if (ret == 0) {
            tca8418_init();
            if (s_kb_connected && fd_keyboard > 0) {
                control_keyboard_backlight(20000, g_kb_backlight_duty);
                printf("[KB] Keyboard hot-plugged and initialized on I2C4\n");
                return true;
            }
        }
        return false;
    }
}

uint32_t tca8418_get_key()
{
    if(fd_keyboard<=0)
     {
        return 0;
     }
   uint32_t key_code=0x00;
    //int ret;
    uint8_t reg_data;    
    tca8418_read_reg(fd_keyboard, 0x02,&reg_data,1);//_INT_STAT
    if((reg_data&0x01)==0x01)
    { 
    tca8418_read_reg(fd_keyboard, 0x03,&reg_data,1);//KEY_LCK_EC
    int event_num=reg_data&0x0F;
    //printf("event_num:%d\n",event_num);
    for(int i=0;i<event_num;i++)
    {
     tca8418_read_reg(fd_keyboard, 0x04,&reg_data,1);//KEY_EVENT_A
     //printf("addr 0x04 and data=%#X\n", reg_data); 
     int flag_press=reg_data>>7;
     int code_index=reg_data&0x7F;
     if(code_index>96)
     {
     printf("flag_press:%d,code_index:%d,code:%s\n",flag_press,code_index,"GPIO");
     }
     else
     {
     printf("flag_press:%d,code_index:%d\n",flag_press,code_index);
     if(flag_press)
     {
     if(code_index==7)//shift
     {
     flag_press_shift=1;
     continue;
     }
     if(code_index==3||code_index==9)//fn
     {
     flag_press_fn=1;
     continue;
     }
     if(code_index==10)//CapsLock
     {
     flag_press_caps_lock=(flag_press_caps_lock==1)?0:1;
     }
     if(flag_press_shift==1)
     {
     key_code=tca8418_keyboard_lvgl_shift[code_index-1];
     }
     else
     {
       char ch=tca8418_keyboard_lvgl[code_index-1];
       if((flag_press_caps_lock==1)&&(ch <= 'z') && (ch >= 'a'))
       {
              ch = ch + 'A' - 'a';
              key_code=ch;           
       }
       else
       {
             key_code=tca8418_keyboard_lvgl[code_index-1];
       }
     }
     }
     else//flag_press==0
     {
     
      if(code_index==7)//shift
     {
     flag_press_shift=0;
     }
     if(code_index==3||code_index==9)//fn
     {
     flag_press_fn=0;
     }
     
     
     }
     }
     
     
    }
    //clear int_flag
    reg_data=0x01;
    tca8418_write_reg(fd_keyboard, 0x02,reg_data);//
    }
    
    
    return key_code;

}
 void control_keyboard_backlight(int freq,int duty)//hz  50%
 {
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;
    g_kb_backlight_duty = duty;
    fpioa_set_function(52,PWM4,-1,-1,-1,-1,-1,-1,-1);
    int fd_pwm = open("/dev/pwm", O_RDWR);
    if (fd_pwm>0)
    {
    pwm_config_t config;
    config.channel = 4;
    config.period = pow(10,9)/freq;//ns  50000
    config.pulse = config.period*duty/100;  //20khz    10%
    ioctl(fd_pwm, KD_PWM_CMD_SET, &config); 
    if (duty > 0) {
        ioctl(fd_pwm, KD_PWM_CMD_ENABLE,&config); 
    } else {
        ioctl(fd_pwm, KD_PWM_CMD_DISABLE,&config);
    }
    close(fd_pwm);    
    }
 }

void keyboard_set_backlight_level(int duty)
{
    control_keyboard_backlight(20000, duty);
}

int keyboard_get_backlight_level(void)
{
    return g_kb_backlight_duty;
}

void keyboard_set_backlight_sleep(bool sleep_off)
{
    int saved = g_kb_backlight_duty;
    if (sleep_off) {
        control_keyboard_backlight(20000, 0);
        g_kb_backlight_duty = saved;
    } else if (tca8418_is_connected()) {
        control_keyboard_backlight(20000, saved);
    }
}

static int vapcell_g68_ocv_to_soc_x10(int vocv_mv)
{
    /* 11-point piecewise-linear OCV curve for 2x Vapcell G68 21700 (6800mAh x 2 = 13600mAh 1S2P)
     * High-density INR21700-6800 cell (4.20V full, 3.60V nominal, 3.00V system cutoff) */
    static const struct {
        int mv;
        int soc_x10;
    } kCurve[] = {
        {4180, 1000},
        {4075,  900},
        {3975,  800},
        {3885,  700},
        {3795,  600},
        {3715,  500},
        {3640,  400},
        {3570,  300},
        {3490,  200},
        {3390,  100},
        {3250,   30},
        {3050,    0},
    };
    const int n = (int)(sizeof(kCurve) / sizeof(kCurve[0]));
    if (vocv_mv >= kCurve[0].mv) return 1000;
    if (vocv_mv <= kCurve[n - 1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (vocv_mv <= kCurve[i].mv && vocv_mv >= kCurve[i + 1].mv) {
            int dv = kCurve[i].mv - kCurve[i + 1].mv;
            int ds = kCurve[i].soc_x10 - kCurve[i + 1].soc_x10;
            return kCurve[i + 1].soc_x10 + ((vocv_mv - kCurve[i + 1].mv) * ds) / dv;
        }
    }
    return 500;
}

bool k230_pmic_poll_status(k230_pmic_status_t *out_status)
{
    static bool s_soc_initialized = false;
    static double s_tracked_soc = 0.0;

    if (!out_status) return false;
    memset(out_status, 0, sizeof(*out_status));
    out_status->full_cap_mah = 13600; /* 2x Vapcell G68 21700 6800mAh in 1S2P parallel */

    int fd = ensure_i2c4_open();
    if (fd <= 0) return false;

    if (!s_bq25896_adc_started) {
        uint8_t r02 = 0;
        if (i2c4_read_bytes(fd, 0x6B, 0x02, &r02, 1) == 0) {
            /* Enable CONV_START (bit 7) + CONV_RATE 1s continuous (bit 6) */
            i2c4_write_reg_byte(fd, 0x6B, 0x02, (uint8_t)(r02 | 0xC0));
            s_bq25896_adc_started = true;
        }
    }

    uint8_t r0b = 0, r0e = 0, r11 = 0, r12 = 0;
    if (i2c4_read_bytes(fd, 0x6B, 0x0B, &r0b, 1) != 0) {
        return false;
    }
    out_status->valid = true;
    out_status->charge_state = (r0b >> 3) & 0x03;
    bool pg_stat = ((r0b >> 2) & 0x01) != 0;

    if (i2c4_read_bytes(fd, 0x6B, 0x0E, &r0e, 1) == 0) {
        int raw_vbat = (int)(r0e & 0x7F);
        out_status->vbat_mv = (raw_vbat > 0) ? (2304 + raw_vbat * 20) : 0;
    }
    if (i2c4_read_bytes(fd, 0x6B, 0x11, &r11, 1) == 0) {
        bool vbus_gd = ((r11 >> 7) & 0x01) != 0;
        int raw_vbus = (int)(r11 & 0x7F);
        out_status->usb_present = vbus_gd || pg_stat || (out_status->charge_state != 0);
        out_status->vbus_mv = out_status->usb_present ? (2600 + raw_vbus * 100) : 0;
    } else {
        out_status->usb_present = pg_stat || (out_status->charge_state != 0);
    }

    int ichg_ma = 0;
    if (i2c4_read_bytes(fd, 0x6B, 0x12, &r12, 1) == 0) {
        ichg_ma = (int)(r12 & 0x7F) * 50;
    }

    /* Query BQ27220 fuel gauge (0x55) for StateOfCharge (0x2C), Voltage (0x08), and Current (0x0C) */
    uint8_t soc_buf[2] = {0, 0};
    if (i2c4_read_bytes(fd, 0x55, 0x2C, soc_buf, 2) == 0) {
        int soc = (int)((soc_buf[1] << 8) | soc_buf[0]);
        if (soc >= 0 && soc <= 100) {
            out_status->raw_gauge_soc = soc;
        }
    }
    uint8_t v_buf[2] = {0, 0};
    if (i2c4_read_bytes(fd, 0x55, 0x08, v_buf, 2) == 0) {
        int mv = (int)((v_buf[1] << 8) | v_buf[0]);
        if (mv >= 2500 && mv <= 4500) {
            out_status->vbat_mv = mv;
        }
    }
    uint8_t i_buf[2] = {0, 0};
    bool got_gauge_curr = false;
    if (i2c4_read_bytes(fd, 0x55, 0x0C, i_buf, 2) == 0) {
        int16_t cur_ma = (int16_t)((i_buf[1] << 8) | i_buf[0]);
        if (cur_ma >= -6000 && cur_ma <= 6000) {
            out_status->ibat_ma = (int)cur_ma;
            got_gauge_curr = true;
        }
    }
    if ((!got_gauge_curr || out_status->ibat_ma == 0) && ichg_ma > 0 &&
        (out_status->charge_state == 1 || out_status->charge_state == 2)) {
        out_status->ibat_ma = ichg_ma;
    }

    /* Compute IR-compensated open-circuit voltage (Vocv = Vbat - Ibat * R_path)
     * R_path ~= 65 mOhm (parallel 21700 cells + spring holder contacts + protection FETs) */
    int ir_drop_mv = (out_status->ibat_ma * 65) / 1000;
    if (ir_drop_mv > 180) ir_drop_mv = 180;
    if (ir_drop_mv < -180) ir_drop_mv = -180;
    out_status->vocv_mv = (out_status->vbat_mv > 0) ? (out_status->vbat_mv - ir_drop_mv) : 0;

    if (out_status->vocv_mv >= 2800) {
        double ocv_soc = vapcell_g68_ocv_to_soc_x10(out_status->vocv_mv) / 10.0;
        if (out_status->charge_state == 3 && out_status->vbat_mv >= 4120) {
            ocv_soc = 100.0;
        }
        if (!s_soc_initialized) {
            s_tracked_soc = ocv_soc;
            s_soc_initialized = true;
        } else {
            /* Integrate real-time coulomb delta over ~1.0s poll interval for 13,600 mAh pack:
             * dSOC(%) = (I_mA * 1.0s / 3600s) / 13600mAh * 100 = I_mA / 489600.0 */
            double dsoc_coulomb = (double)out_status->ibat_ma / 489600.0;
            s_tracked_soc += dsoc_coulomb;
            /* Gently blend (2% per poll) toward IR-compensated Vapcell G68 OCV target */
            s_tracked_soc = s_tracked_soc * 0.98 + ocv_soc * 0.02;
        }
        if (s_tracked_soc < 1.0 && out_status->vbat_mv > 3050) s_tracked_soc = 1.0;
        if (s_tracked_soc < 0.0) s_tracked_soc = 0.0;
        if (s_tracked_soc > 100.0) s_tracked_soc = 100.0;
        out_status->soc_percent = (int)(s_tracked_soc + 0.5);
    } else {
        out_status->soc_percent = out_status->raw_gauge_soc;
    }

    out_status->rem_cap_mah = (out_status->full_cap_mah * out_status->soc_percent) / 100;
    return true;
}

bool k230_pmic_power_off_ship_mode(void)
{
    int fd = ensure_i2c4_open();
    if (fd <= 0) return false;
    uint8_t r09 = 0;
    if (i2c4_read_bytes(fd, 0x6B, 0x09, &r09, 1) != 0) {
        return false;
    }
    /* Clear BATFET_DLY (bit 3) for immediate turn-off and set BATFET_DIS (bit 5) */
    r09 &= (uint8_t)~(1 << 3);
    r09 |= (uint8_t)(1 << 5);
    return (i2c4_write_reg_byte(fd, 0x6B, 0x09, r09) == 0);
}
 
void control_keyboard_indicator_led_caps_lock(int is_open)
{

    int ret;
    uint8_t reg_data;
    if(fd_keyboard>0)
    {
    reg_data=0x00;
    ret=xl9555_write_reg(fd_keyboard, 0x06,reg_data);//P00-P07   output
    printf("xl9555_write_reg ret=%d\n",ret);
    if(is_open)
    {
    reg_data=0xF7;
    }
    else
    {
    reg_data=0xFF;
    }
    xl9555_write_reg(fd_keyboard, 0x02,reg_data);//P00-P07   output high  low  led light on
    }
}














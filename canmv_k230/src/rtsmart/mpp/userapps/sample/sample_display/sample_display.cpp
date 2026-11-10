#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <stdbool.h>
#include "lvgl/lvgl.h"
#include "port/lv_port_disp.h"
#include "port/lv_port_indev.h"
#include "lvgl/src/hal/lv_hal_tick.h"
//#include "rtdevice.h"
//#include "lvgl/demos/lv_demos.h"
#include "cap_vio.h"
#include "esp_brookesia.hpp"
#include <math.h>
/* These are built-in app examples in `esp-brookesia` library */
#include "app_examples/phone/calculator/Calculator.hpp"
#include "app_examples/phone/camera/Camera.hpp"
#include "app_examples/phone/lora/Lora.hpp"
#include "app_examples/phone/setting/Setting.hpp"
#include "app_examples/phone/clock/Clock.hpp"
#include "app_examples/phone/battery/Battery.hpp"
#include "brookesia_app.hpp"
#include "lexios_apps.hpp"
#include "ui/dashboard.h"
#include "keyboard_ext.h"
#include "mpi_vo_api.h"
#include "k_vo_comm.h"
//#define USING_BLE
//#define USING_WEATHER
//#define USING_KEYBOARD
// Disable synchronous I2C4 battery polling & thermal sensor reads in UI timer to prevent UI freezes
//#define USING_BATTERY
//#define USING_FAN
// Do NOT define USING_LTE_GNSS here because it remuxes GPIO5 (LR2021 RADIO_RST_PIN) to UART2_TXD
//#define USING_LTE_GNSS
static int gpio_fd = -1;
static int fd_ts = -1;
static volatile int g_os_thermal_level = 0; // 0 = normal (<68C), 1 = throttle (68..82C), 2 = emergency cutoff (>=82C)
static volatile int g_os_cpu_dvfs_stage = -1; // 0=AUTO, 1=1188MHz, 2=800MHz, 3=594MHz, 4=400MHz, 5=200MHz
static volatile double g_last_die_temp_c = 38.0;
static volatile bool g_fan_active = false;
static volatile int g_fan_mode = LEXIOS_FAN_AUTO;
static volatile bool g_screen_asleep = false;
static volatile bool g_screen_dimmed = false;
static volatile int g_user_brightness_pct = 100; // 25, 50, 75, 100
static volatile int g_sleep_timeout_sec = 60;    // 30, 60 (1m default), 300 (5m), 0 (Never)
static volatile long g_last_activity_sec = 0;
static volatile long g_boot_time_sec = 0;
static volatile bool g_nrf9151_power_on = false;
static volatile bool g_wifi_ble_power_on = true;
static volatile bool g_manual_battery_saver = false;
static k230_pmic_status_t g_pmic_status = {false, false, 0, 85, 85, 3950, 3950, 0, 0, 13600, 11560};

static void boot_log(const char *step) {
    printf("[LexiOS-BOOT] %s\n", step);
    FILE *fp = fopen("/sdcard/display_boot.log", "a");
    if (fp) {
        fprintf(fp, "%s\n", step);
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
    }
}

extern "C" void disp_enable_update(void);
extern "C" void disp_disable_update(void);
extern k_u8 *pic_addr;
extern k_video_frame_info vf_info;
extern k_vo_osd osd_vo;

#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include "fpioa/rt_fpioa.h"
#include "mpi_pm_api.h"
/* ioctl */
#define	GPIO_DM_OUTPUT           _IOW('G', 0, int)
#define	GPIO_DM_INPUT            _IOW('G', 1, int)
#define	GPIO_DM_INPUT_PULL_UP    _IOW('G', 2, int)
#define	GPIO_DM_INPUT_PULL_DOWN  _IOW('G', 3, int)
#define	GPIO_WRITE_LOW           _IOW('G', 4, int)
#define	GPIO_WRITE_HIGH          _IOW('G', 5, int)
#define	GPIO_READ_VALUE          _IOW('G', 12, int)

static long monotonic_sec_now(void) {
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static void write_os_thermal_state(int level, double temp_c) {
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *fp = fopen("/tmp/os_thermal_state", "w");
    if (fp) {
        fprintf(fp, "%d %.1f %ld\n", level, temp_c, (long)ts.tv_sec);
        fclose(fp);
    }
}

static void apply_cpu_dvfs_stage(int stage, double temp_c) {
    if (stage == g_os_cpu_dvfs_stage) {
        return;
    }
    if (stage <= 0) {
        kd_mpi_pm_set_governor(PM_DOMAIN_CPU, PM_GOVERNOR_AUTO);
        printf("[OS-THERMAL] CPU1 DVFS -> AUTO (200 MHz idle / on-demand scaling) at %.1f C\n", temp_c);
        stage = 0;
    } else {
        // stage 1..5 maps directly to K230 PM_DOMAIN_CPU hardware profile index 1..5:
        // 1 = 1.188 GHz (-26% power), 2 = 800 MHz (-50%), 3 = 594 MHz (-63%), 4 = 400 MHz (-75%), 5 = 200 MHz (-87.5%)
        static const int kFreqMhz[6] = {1600, 1188, 800, 594, 400, 200};
        if (stage > 5) stage = 5;
        kd_mpi_pm_set_governor(PM_DOMAIN_CPU, PM_GOVERNOR_MANUAL);
        kd_mpi_pm_set_profile(PM_DOMAIN_CPU, stage);
        printf("[OS-THERMAL] CPU1 DVFS -> Profile %d (%d MHz) at %.1f C (throttles all processes)\n",
               stage, kFreqMhz[stage], temp_c);
    }
    g_os_cpu_dvfs_stage = stage;
}

static void os_init_low_power_and_dvfs(void) {
    // Power-gate unused AI/3D/Video domains on the K230 die that default to ON in pm_core_init()
    kd_mpi_pm_set_clock(PM_DOMAIN_KPU, K_FALSE);
    kd_mpi_pm_set_power(PM_DOMAIN_KPU, K_FALSE);
    kd_mpi_pm_set_clock(PM_DOMAIN_DPU, K_FALSE);
    kd_mpi_pm_set_power(PM_DOMAIN_DPU, K_FALSE);
    kd_mpi_pm_set_clock(PM_DOMAIN_VPU, K_FALSE);
    kd_mpi_pm_set_power(PM_DOMAIN_VPU, K_FALSE);

    // Enable hardware AUTO frequency governor so idle CPU runs at 200 MHz instead of locked 1.6 GHz
    apply_cpu_dvfs_stage(0, 35.0);
    printf("[OS-THERMAL] Initialized K230 PM: KPU/DPU/VPU powered off, CPU1 governor = AUTO.\n");
}

static int compute_target_dvfs_stage(int prev_stage, double temp) {
    // 6-step progressive hardware clock scaling with 3C hysteresis on step-down recovery
    int stage = 0;
    if (temp >= 74.0) stage = 5; // 200 MHz (-87.5% power)
    else if (prev_stage == 5 && temp >= 71.0) stage = 5;
    else if (temp >= 68.0) stage = 4; // 400 MHz (-75% power)
    else if (prev_stage >= 4 && temp >= 65.0) stage = 4;
    else if (temp >= 62.0) stage = 3; // 594 MHz (-63% power)
    else if (prev_stage >= 3 && temp >= 59.0) stage = 3;
    else if (temp >= 56.0) stage = 2; // 800 MHz (-50% power)
    else if (prev_stage >= 2 && temp >= 53.0) stage = 2;
    else if (temp >= 50.0) stage = 1; // 1188 MHz (-26% power)
    else if (prev_stage >= 1 && temp >= 47.0) stage = 1;
    else stage = 0;                   // AUTO (200 MHz idle <-> on-demand)

    if (g_manual_battery_saver && stage < 3) {
        stage = 3; // Cap at 594 MHz (-63% power) when Battery Saver is enabled
    }
    return stage;
}

static void apply_fan_state(double temp) {
    if (gpio_fd <= 0) return;
    pin_gpio_t pin_42;
    pin_42.pin = 42;
    bool target_on = g_fan_active;
    if (temp >= 68.0 || g_fan_mode == LEXIOS_FAN_FORCE_ON) {
        target_on = true;
    } else if (g_fan_mode == LEXIOS_FAN_FORCE_OFF) {
        target_on = false;
    } else {
        if (temp >= 45.0) target_on = true;
        else if (temp < 38.0) target_on = false;
    }
    if (target_on != g_fan_active) {
        ioctl(gpio_fd, target_on ? GPIO_WRITE_HIGH : GPIO_WRITE_LOW, &pin_42);
        g_fan_active = target_on;
    }
}

extern "C" double lexios_get_die_temp_c(void) {
    return g_last_die_temp_c;
}

extern "C" int lexios_get_cpu_dvfs_stage(void) {
    return g_os_cpu_dvfs_stage < 0 ? 0 : g_os_cpu_dvfs_stage;
}

extern "C" int lexios_get_cpu_freq_mhz(void) {
    static const int kFreqMhz[6] = {1600, 1188, 800, 594, 400, 200};
    int st = lexios_get_cpu_dvfs_stage();
    if (st < 0) st = 0;
    if (st > 5) st = 5;
    return kFreqMhz[st];
}

extern "C" int lexios_get_thermal_level(void) {
    return g_os_thermal_level;
}

extern "C" bool lexios_get_fan_active(void) {
    return g_fan_active;
}

extern "C" lexios_fan_mode_t lexios_get_fan_mode(void) {
    return (lexios_fan_mode_t)g_fan_mode;
}

extern "C" void lexios_set_fan_mode(lexios_fan_mode_t mode) {
    int m = (int)mode;
    if (m < LEXIOS_FAN_AUTO || m > LEXIOS_FAN_FORCE_OFF) {
        m = LEXIOS_FAN_AUTO;
    }
    g_fan_mode = m;
    apply_fan_state(g_last_die_temp_c);
}

extern "C" bool lexios_is_screen_asleep(void) {
    return g_screen_asleep;
}

extern "C" int lexios_get_sleep_timeout_sec(void) {
    return g_sleep_timeout_sec;
}

static void save_rotation_config(void);
static void flush_rotation_config_to_sdcard(void);
static void qs_refresh_ui(void);

extern "C" void lexios_set_sleep_timeout_sec(int sec) {
    g_sleep_timeout_sec = sec;
    lexios_notify_user_activity();
    save_rotation_config();
    flush_rotation_config_to_sdcard();
    qs_refresh_ui();
}

extern "C" uint64_t lexios_get_uptime_sec(void) {
    long now = monotonic_sec_now();
    if (g_boot_time_sec == 0) g_boot_time_sec = now;
    long diff = now - g_boot_time_sec;
    return diff > 0 ? (uint64_t)diff : 0ULL;
}

extern "C" bool lexios_get_nrf9151_power(void) {
    return g_nrf9151_power_on;
}

extern "C" void lexios_set_nrf9151_power(bool enable) {
    if (gpio_fd <= 0) {
        gpio_fd = open("/dev/gpio", O_RDWR);
    }
    if (enable) {
        fpioa_set_function(2, GPIO2, -1, -1, -1, -1, -1, -1, -1);
        if (gpio_fd > 0) {
            pin_gpio_t pin_2 = {.pin = 2, .val = 1};
            ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_2);
            ioctl(gpio_fd, GPIO_WRITE_HIGH, &pin_2);
        }
        usleep(150000);
        fpioa_set_function(5, UART2_TXD, -1, -1, -1, -1, -1, -1, -1);
        fpioa_set_function(6, UART2_RXD, -1, -1, -1, -1, -1, -1, -1);
        g_nrf9151_power_on = true;
        printf("[LexiOS-SAT] Powered ON nRF9151 (GPIO2 HIGH) & muxed GPIO5/6 -> UART2 (115200 8N1)\n");
    } else {
        fpioa_set_function(5, GPIO5, -1, -1, -1, -1, -1, -1, -1);
        if (gpio_fd > 0) {
            pin_gpio_t pin_5 = {.pin = 5, .val = 1};
            ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_5);
            ioctl(gpio_fd, GPIO_WRITE_HIGH, &pin_5);
        }
        fpioa_set_function(2, GPIO2, -1, -1, -1, -1, -1, -1, -1);
        if (gpio_fd > 0) {
            pin_gpio_t pin_2 = {.pin = 2, .val = 0};
            ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_2);
            ioctl(gpio_fd, GPIO_WRITE_LOW, &pin_2);
        }
        g_nrf9151_power_on = false;
        printf("[LexiOS-SAT] Powered OFF nRF9151 (GPIO2 LOW) & locked GPIO5 HIGH for LR2021\n");
    }
}

extern "C" bool lexios_get_wifi_ble_power(void) {
    return g_wifi_ble_power_on;
}

extern "C" void lexios_set_wifi_ble_power(bool enable) {
    /* Keep IO45_WIFI_EN pulled HIGH via U-Boot PU=1 so RTL8189 SDIO never glitches;
     * only mux UART1 for BLE NUS when requested, and notify k230_remoted Wi-Fi manager. */
    if (enable) {
        fpioa_set_function(3, UART1_TXD, -1, -1, -1, -1, -1, -1, -1);
        fpioa_set_function(4, UART1_RXD, -1, -1, -1, -1, -1, -1, -1);
        g_wifi_ble_power_on = true;
    } else {
        g_wifi_ble_power_on = false;
    }
    FILE *cf = fopen("/tmp/wifi_cmd.txt.tmp", "w");
    if (cf) {
        fprintf(cf, "ENABLE %d\n", enable ? 1 : 0);
        fclose(cf);
        unlink("/tmp/wifi_cmd.txt");
        rename("/tmp/wifi_cmd.txt.tmp", "/tmp/wifi_cmd.txt");
    }
}

extern "C" void lexios_set_screen_dim(bool dim_mode) {
    if (g_screen_asleep || dim_mode == g_screen_dimmed) {
        return;
    }
    g_screen_dimmed = dim_mode;
    if (dim_mode) {
        int dim_brt = (g_user_brightness_pct > 20) ? 20 : g_user_brightness_pct;
        vo_display_set_brightness(dim_brt);
        keyboard_set_backlight_sleep(true);
    } else {
        vo_display_set_brightness(g_user_brightness_pct);
        keyboard_set_backlight_sleep(false);
    }
    if (lv_scr_act() != NULL) {
        lv_obj_invalidate(lv_scr_act());
    }
    if (lv_layer_top() != NULL) {
        lv_obj_invalidate(lv_layer_top());
    }
}

extern "C" void lexios_notify_user_activity(void) {
    g_last_activity_sec = monotonic_sec_now();
    if (g_screen_dimmed && !g_screen_asleep) {
        lexios_set_screen_dim(false);
    }
}

extern "C" void lexios_set_screen_sleep(bool sleep_mode) {
    if (sleep_mode == g_screen_asleep) {
        return;
    }
    g_screen_asleep = sleep_mode;
    if (sleep_mode) {
        g_screen_dimmed = false;
        printf("[OS-PM] Entering Pocket Standby: Blank RM69A10 AMOLED + KB LED OFF + CPU1 -> 200 MHz\n");
        keyboard_set_backlight_sleep(true);
        vo_display_set_brightness(0);
        disp_disable_update();
        if (pic_addr != NULL) {
            /* Fill with opaque black (0x000000FF in swapped ARGB) so AMOLED pixels emit zero light
             * while keeping MIPI DSI & RM69A10 panel registers continuously initialized */
            uint32_t *px = (uint32_t *)pic_addr;
            const int total_px = 568 * 1232;
            for (int i = 0; i < total_px; i++) {
                px[i] = 0x000000FFu;
            }
            kd_mpi_vo_chn_insert_frame(osd_vo + 3, &vf_info);
        }
        /* Keep CPU1 on hardware AUTO governor (200 MHz idle, scales on SDIO/radio IRQ) so Wi-Fi OTA stays online during sleep */
        apply_cpu_dvfs_stage(0, g_last_die_temp_c);
    } else {
        g_screen_dimmed = false;
        lexios_notify_user_activity();
        int target_stage = compute_target_dvfs_stage(0, g_last_die_temp_c);
        apply_cpu_dvfs_stage(target_stage, g_last_die_temp_c);
        vo_display_set_brightness(g_user_brightness_pct);
        disp_enable_update();
        keyboard_set_backlight_sleep(false);
        if (lv_scr_act() != NULL) {
            lv_obj_invalidate(lv_scr_act());
        }
        if (lv_layer_top() != NULL) {
            lv_obj_invalidate(lv_layer_top());
        }
        printf("[OS-PM] Woke from Pocket Standby: AMOLED ON (%d%%), CPU1 stage %d\n",
               g_user_brightness_pct, target_stage);
    }
}

static void *os_thermal_guardian_thread(void *arg) {
    (void)arg;
    bool fan_on = false;
    os_init_low_power_and_dvfs();
    write_os_thermal_state(0, 35.0);
    while (1) {
        sleep(3);
        if (fd_ts <= 0) {
            fd_ts = open("/dev/ts", O_RDWR);
            if (fd_ts <= 0) {
                continue;
            }
        }
        uint32_t buffer[1] = {0};
        int ret = read(fd_ts, buffer, sizeof(buffer));
        if (ret <= 0) {
            continue;
        }
        uint32_t ts_val = buffer[0];
        double code = (double)(ts_val & 0xfff);
        if (code <= 0.0 || code >= 4095.0) {
            continue;
        }
        double temp = (1e-10 * pow(code, 4) * 1.01472 -
                       1e-6 * pow(code, 3) * 1.10063 +
                       4.36150 * 1e-3 * pow(code, 2) -
                       7.10128 * code + 3565.87);
        if (temp < -20.0 || temp > 150.0) {
            continue;
        }
        g_last_die_temp_c = temp;

        // 1. Active fan control on GPIO42 (JP1 Pin 9) with hysteresis (ON >= 45C, OFF < 38C) & user mode override
        apply_fan_state(temp);

        // 2. Progressive hardware CPU1 DVFS throttling (throttles ALL RT-Smart processes in hardware)
        int next_dvfs = compute_target_dvfs_stage(g_os_cpu_dvfs_stage, temp);
        apply_cpu_dvfs_stage(next_dvfs, temp);

        // 3. System / Radio thermal state machine (0 = normal, 1 = throttle duty/TX, 2 = emergency cutoff >=82C)
        int prev_level = g_os_thermal_level;
        int next_level = prev_level;
        if (temp >= 82.0) {
            next_level = 2; // Emergency failsafe cutoff only if >=82C despite 200 MHz CPU throttle
        } else if (prev_level == 2) {
            if (temp < 55.0) {
                next_level = 0;
            }
        } else if (temp >= 68.0) {
            next_level = 1; // Throttle radio TX power (+4 dBm) & duty cycle while keeping app running
        } else if (prev_level == 1 && temp < 60.0) {
            next_level = 0;
        }

        g_os_thermal_level = next_level;
        write_os_thermal_state(next_level, temp);

        if (next_level != prev_level) {
            if (next_level == 2) {
                printf("[OS-THERMAL] EMERGENCY CUTOFF: Die temp %.1f C >= 82 C! Powering down RF rails & pausing UI.\n", temp);
                if (gpio_fd > 0) {
                    pin_gpio_t p42 = {.pin = 42};
                    pin_gpio_t p44 = {.pin = 44};
                    pin_gpio_t p2  = {.pin = 2};
                    pin_gpio_t p45 = {.pin = 45};
                    ioctl(gpio_fd, GPIO_WRITE_HIGH, &p42); // Fan 100%
                    ioctl(gpio_fd, GPIO_WRITE_LOW, &p44);  // Cut LR2021 radio power
                    ioctl(gpio_fd, GPIO_WRITE_LOW, &p2);   // Cut nRF9151 power
                    ioctl(gpio_fd, GPIO_WRITE_LOW, &p45);  // Cut Wi-Fi power
                }
            } else if (next_level == 1) {
                printf("[OS-THERMAL] THROTTLE: Die temp %.1f C >= 68 C — stepping down CPU & radio TX power.\n", temp);
            } else {
                printf("[OS-THERMAL] RECOVERY: Die temp cooled to %.1f C — resuming normal operation.\n", temp);
                if (gpio_fd > 0 && prev_level == 2) {
                    pin_gpio_t p44 = {.pin = 44};
                    pin_gpio_t p45 = {.pin = 45};
                    ioctl(gpio_fd, GPIO_WRITE_HIGH, &p44); // Restore LR2021 radio power
                    ioctl(gpio_fd, GPIO_WRITE_HIGH, &p45); // Restore Wi-Fi power
                }
            }
        }
    }
    return NULL;
}

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


//////////////uart   to use nRF9151 LTE-m
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <poll.h>
using namespace std;
#define BUF_SIZE    (1024)
#define UART_DEVICE_NAME    "/dev/uart2"
static char buf_rx[BUF_SIZE];
static int fd_uart=0;
#define	IOC_SET_BAUDRATE            _IOW('U', 0x40, int)
struct uart_configure
{
    uint32_t baud_rate;

    uint32_t data_bits               :4;
    uint32_t stop_bits               :2;
    uint32_t parity                  :2;
    uint32_t fifo_lenth              :2;
    uint32_t auto_flow               :1;
    uint32_t reserved                :21;
};

typedef enum _uart_parity
{
    UART_PARITY_NONE,
    UART_PARITY_ODD,
    UART_PARITY_EVEN
} uart_parity_t;

typedef enum _uart_receive_trigger
{
    UART_RECEIVE_FIFO_1,
    UART_RECEIVE_FIFO_8,
    UART_RECEIVE_FIFO_16,
    UART_RECEIVE_FIFO_30,
} uart_receive_trigger_t;
static int flag_running=1;
static pthread_t pthread_handle_uart_rx = NULL;
static pthread_cond_t cond;
static pthread_mutex_t mutex_uart_rx;

//////////////////////////////
static int fd_i2c0=-1;
#if defined(USING_BATTERY)
static int bq27220_read_cmd_data(int fd,uint8_t reg_addr,uint8_t *reg_data, int length)
{
    int ret=0;
        i2c_msg_t msgs[2]=
        {
         {
        .addr=0x55, 
        .flags=0, 
        .len=1, 
        .buf=&reg_addr
        },
        {
        .addr=0x55, 
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
static int bq27220_write_cmd_params(int fd, uint8_t*cmd_params,int length)
{
    int ret=0;
    i2c_msg_t msgs[1]=
        {
        {
        .addr=0x55, 
        .flags=0, 
        .len=(uint16_t)length, 
        .buf=cmd_params
        }
        
        };
        i2c_priv_data_t privdata={msgs, 1};
        if (ioctl(fd, RT_I2C_DEV_CTRL_RW, &privdata) != 0){ //读写数据
            ret=-1;
        }
    return ret;
}
#endif
uint8_t bq27220_data[2];
//int bat_percent=0;

/* ============================================================================
 * Screen Orientation & Top Quick Settings Drawer
 *   Mode 0: AUTO (KB)  -> Landscape (1232x568) when keyboard attached, Portrait (568x1232) when detached
 *   Mode 1: PORTRAIT   -> Locked Portrait (568x1232)
 *   Mode 2: LANDSCAPE  -> Locked Landscape (1232x568)
 *   Flip 180: 0 or 1   -> Flips active Portrait (0 <-> 180) or Landscape (270 <-> 90)
 * ============================================================================ */
#define ROT_MODE_AUTO_KB    0
#define ROT_MODE_PORTRAIT   1
#define ROT_MODE_LANDSCAPE  2

static ESP_Brookesia_Phone *g_phone_instance = NULL;
static ESP_Brookesia_PhoneStylesheet_t g_sheet_portrait;
static ESP_Brookesia_PhoneStylesheet_t g_sheet_landscape;
static bool g_sheets_initialized = false;
static bool g_landscape_sheet_added = false;

static void init_orientation_stylesheets(void) {
    g_sheet_portrait = ESP_BROOKESIA_PHONE_568_1232_DARK_STYLESHEET();
    g_sheet_portrait.core.home.background.color = ESP_BROOKESIA_STYLE_COLOR(0x0b1324);
    g_sheet_portrait.core.home.background.wallpaper_image_resource.resource = NULL;
    for (int i = 0; i < 6; i++) {
        g_sheet_portrait.core.home.container.styles[i].outline_width = 0;
    }

    /* Rounded-AMOLED bezel safe-area insets (~56-64px corner arc clearance) */
    g_sheet_portrait.home.status_bar.data.main.size = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(100, 56);
    g_sheet_portrait.home.status_bar.data.main.background_color = ESP_BROOKESIA_STYLE_COLOR(0x0f172a);
    g_sheet_portrait.home.status_bar.data.area.data[0].size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(26, 100);
    g_sheet_portrait.home.status_bar.data.area.data[0].layout_column_start_offset = 58;
    g_sheet_portrait.home.status_bar.data.area.data[1].size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(30, 100);
    g_sheet_portrait.home.status_bar.data.area.data[2].size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(44, 100);
    g_sheet_portrait.home.status_bar.data.area.data[2].layout_column_start_offset = 58;
    g_sheet_portrait.home.navigation_bar.data.main.background_color = ESP_BROOKESIA_STYLE_COLOR(0x0f172a);
    g_sheet_portrait.home.app_launcher.data.table.size = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(92, 860);
    g_sheet_portrait.home.app_launcher.data.indicator.main_layout_bottom_offset = 52;

    g_sheet_landscape = g_sheet_portrait;
    g_sheet_landscape.core.name = "1232x568 Dark";
    g_sheet_landscape.core.screen_size = ESP_BROOKESIA_STYLE_SIZE_RECT(1232, 568);
    g_sheet_landscape.home.status_bar.data.area.data[0].size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(33, 100);
    g_sheet_landscape.home.status_bar.data.area.data[0].layout_column_start_offset = 66;
    g_sheet_landscape.home.status_bar.data.area.data[1].size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(34, 100);
    g_sheet_landscape.home.status_bar.data.area.data[2].size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(33, 100);
    g_sheet_landscape.home.status_bar.data.area.data[2].layout_column_start_offset = 66;
    g_sheet_landscape.home.navigation_bar.data.main.size = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(100, 64);
    g_sheet_landscape.home.app_launcher.data.table.size = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(90, 410);
    g_sheet_landscape.home.app_launcher.data.indicator.main_layout_bottom_offset = 18;
    g_sheet_landscape.home.recents_screen.data.main.layout_row_pad = 12;
    g_sheet_landscape.home.recents_screen.data.main.layout_bottom_pad = 16;
    g_sheet_landscape.home.recents_screen.data.snapshot_table.snapshot.main_size =
        ESP_BROOKESIA_STYLE_SIZE_RECT(480, 330);
    g_sheet_landscape.home.recents_screen.data.snapshot_table.snapshot.title.main_size =
        ESP_BROOKESIA_STYLE_SIZE_RECT(480, 54);
    g_sheet_landscape.home.recents_screen.data.snapshot_table.snapshot.image.main_size =
        ESP_BROOKESIA_STYLE_SIZE_RECT(480, 276);
    g_sheets_initialized = true;
}

static void style_home_wallpaper_gradient(void) {
    if (!g_phone_instance) return;
    lv_obj_t *home_obj = g_phone_instance->getHome().getMainScreenObject();
    if (home_obj) {
        lv_obj_set_style_bg_color(home_obj, lv_color_hex(0x0b1324), 0);
        lv_obj_set_style_bg_grad_color(home_obj, lv_color_hex(0x172554), 0);
        lv_obj_set_style_bg_grad_dir(home_obj, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(home_obj, LV_OPA_COVER, 0);
    }
}

static int g_rot_mode = ROT_MODE_PORTRAIT;
static int g_rot_flip_180 = 0;
static int g_applied_hw_rot = 0;

static lv_obj_t *g_qs_pill_btn = NULL;
static lv_obj_t *g_qs_pill_lbl = NULL;
static lv_obj_t *g_qs_drawer = NULL;
static lv_obj_t *g_qs_info_lbl = NULL;
static lv_obj_t *g_qs_pwr_lbl = NULL;
static lv_obj_t *g_qs_rot_btns[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *g_qs_brt_btns[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *g_qs_slp_btns[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *g_qs_kb_btns[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *g_qs_poweroff_btn = NULL;
static lv_obj_t *g_qs_poweroff_lbl = NULL;
static bool g_qs_open = false;
static bool g_qs_poweroff_confirm = false;
static bool g_rot_config_dirty = false;
static int g_last_qs_layout_w = 0;
static int g_low_bat_crit_polls = 0;
static bool g_low_bat_throttled = false;

static void flush_rotation_config_to_sdcard(void) {
    if (!g_rot_config_dirty) return;
    g_rot_config_dirty = false;
    FILE *fp = fopen("/sdcard/display_rot.conf", "w");
    if (fp) {
        fprintf(fp, "MODE=%d\nFLIP=%d\nKB_LIGHT=%d\nBRIGHTNESS=%d\nTIMEOUT=%d\nBAT_SAVER=%d\n",
                g_rot_mode, g_rot_flip_180, keyboard_get_backlight_level(),
                g_user_brightness_pct, g_sleep_timeout_sec, g_manual_battery_saver ? 1 : 0);
        fclose(fp);
    }
}

static void save_rotation_config(void) {
    /* Write immediately to RAM tmpfs (/tmp) to avoid synchronous SD card contention during touch events */
    FILE *fp = fopen("/tmp/display_rot.conf", "w");
    if (fp) {
        fprintf(fp, "MODE=%d\nFLIP=%d\nKB_LIGHT=%d\nBRIGHTNESS=%d\nTIMEOUT=%d\nBAT_SAVER=%d\n",
                g_rot_mode, g_rot_flip_180, keyboard_get_backlight_level(),
                g_user_brightness_pct, g_sleep_timeout_sec, g_manual_battery_saver ? 1 : 0);
        fclose(fp);
    }
    g_rot_config_dirty = true;
}

static void load_rotation_config(void) {
    FILE *fp = fopen("/tmp/display_rot.conf", "r");
    if (!fp) {
        fp = fopen("/sdcard/display_rot.conf", "r");
    }
    if (!fp) return;
    char line[64];
    int kb_light = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MODE=", 5) == 0) {
            int m = atoi(line + 5);
            if (m >= 0 && m <= 2) g_rot_mode = m;
        } else if (strncmp(line, "FLIP=", 5) == 0) {
            g_rot_flip_180 = atoi(line + 5) ? 1 : 0;
        } else if (strncmp(line, "KB_LIGHT=", 9) == 0) {
            kb_light = atoi(line + 9);
        } else if (strncmp(line, "BRIGHTNESS=", 11) == 0) {
            int b = atoi(line + 11);
            if (b >= 20 && b <= 100) g_user_brightness_pct = b;
        } else if (strncmp(line, "TIMEOUT=", 8) == 0) {
            int t = atoi(line + 8);
            if (t == 0 || (t >= 15 && t <= 3600)) g_sleep_timeout_sec = t;
        } else if (strncmp(line, "BAT_SAVER=", 10) == 0) {
            g_manual_battery_saver = (atoi(line + 10) != 0);
        }
    }
    fclose(fp);
    if (g_manual_battery_saver) {
        apply_cpu_dvfs_stage(3, g_last_die_temp_c);
        vo_display_set_brightness(g_user_brightness_pct > 40 ? 40 : g_user_brightness_pct);
    } else {
        vo_display_set_brightness(g_user_brightness_pct);
    }
    if (kb_light >= 0 && tca8418_is_connected()) {
        keyboard_set_backlight_level(kb_light);
        if (g_manual_battery_saver) {
            keyboard_set_backlight_sleep(true);
        }
    }
}

static int compute_target_hw_rotation(void) {
    bool want_landscape = false;
    if (g_rot_mode == ROT_MODE_AUTO_KB) {
        want_landscape = tca8418_is_connected();
    } else if (g_rot_mode == ROT_MODE_LANDSCAPE) {
        want_landscape = true;
    } else {
        want_landscape = false;
    }
    if (want_landscape) {
        /* Default landscape = 3 (270 deg CCW), flipped = 1 (90 deg CW) */
        return g_rot_flip_180 ? 1 : 3;
    } else {
        /* Default portrait = 0 (0 deg), flipped = 2 (180 deg) */
        return g_rot_flip_180 ? 2 : 0;
    }
}

static void qs_refresh_ui(void) {
    if (g_qs_pill_lbl) {
        const char *mstr = (g_rot_mode == ROT_MODE_AUTO_KB)
                               ? (tca8418_is_connected() ? "AUTO:LAND" : "AUTO:PORT")
                               : (g_rot_mode == ROT_MODE_LANDSCAPE ? "LANDSCAPE" : "PORTRAIT");
        char pbuf[32];
        snprintf(pbuf, sizeof(pbuf), "%s %s", mstr, g_qs_open ? "^" : "v");
        if (strcmp(lv_label_get_text(g_qs_pill_lbl), pbuf) != 0) {
            lv_label_set_text(g_qs_pill_lbl, pbuf);
        }
    }
    if (!g_qs_drawer) return;

    int cur_w = (int)lv_disp_get_hor_res(NULL);
    if (cur_w != g_last_qs_layout_w) {
        g_last_qs_layout_w = cur_w;
        if (g_qs_pill_btn) {
            lv_obj_set_pos(g_qs_pill_btn, (cur_w > 700) ? 62 : 52, 10);
        }
        int drawer_w = (cur_w > 700) ? 760 : 512;
        lv_obj_set_width(g_qs_drawer, drawer_w);
        lv_obj_align(g_qs_drawer, LV_ALIGN_TOP_MID, 0, 62);
    }

    if (!g_qs_open) return;

    if (g_qs_info_lbl) {
        char wifi_summary[64] = "Disconnected";
        FILE *wf = fopen("/tmp/wifi_status.txt", "r");
        if (wf) {
            char wline[96] = {0};
            if (fgets(wline, sizeof(wline), wf)) {
                if (strncmp(wline, "CONNECTED ", 10) == 0) {
                    char wip[24] = {0}, wssid[32] = {0};
                    if (sscanf(wline + 10, "%23s %31s", wip, wssid) >= 2 && wssid[0] != '\0') {
                        snprintf(wifi_summary, sizeof(wifi_summary), "%s (%s)", wssid, wip);
                    } else if (wip[0] != '\0') {
                        snprintf(wifi_summary, sizeof(wifi_summary), "%s", wip);
                    }
                } else if (strncmp(wline, "DISABLED", 8) == 0) {
                    snprintf(wifi_summary, sizeof(wifi_summary), "Off");
                }
            }
            fclose(wf);
        }
        char ibuf[128];
        snprintf(ibuf, sizeof(ibuf), "Wi-Fi: %s  |  KB: %s  |  Die: %.1f C",
                 wifi_summary,
                 tca8418_is_connected() ? "ATTACHED" : "DETACHED",
                 g_last_die_temp_c);
        lv_label_set_text(g_qs_info_lbl, ibuf);
    }

    if (g_qs_pwr_lbl) {
        char pbuf[160];
        if (g_pmic_status.valid) {
            const char *chg_str = "ON BATTERY";
            if (g_pmic_status.charge_state == 1) chg_str = "PRE-CHARGING";
            else if (g_pmic_status.charge_state == 2) chg_str = "USB CHARGING";
            else if (g_pmic_status.charge_state == 3) chg_str = "CHARGE FULL";
            else if (g_pmic_status.usb_present) chg_str = "USB POWERED";
            snprintf(pbuf, sizeof(pbuf),
                     "Bat: %d%% (%d/%dmAh)  |  %.2fV (%+dmA)  |  %s",
                     g_pmic_status.soc_percent,
                     g_pmic_status.rem_cap_mah,
                     g_pmic_status.full_cap_mah,
                     g_pmic_status.vbat_mv / 1000.0,
                     g_pmic_status.ibat_ma,
                     chg_str);
        } else {
            snprintf(pbuf, sizeof(pbuf), "Power: Active  |  Brightness: %d%%", g_user_brightness_pct);
        }
        lv_label_set_text(g_qs_pwr_lbl, pbuf);
    }

    for (int i = 0; i < 3; i++) {
        if (!g_qs_rot_btns[i]) continue;
        bool active = (g_rot_mode == i);
        lv_obj_set_style_bg_color(g_qs_rot_btns[i],
                                  lv_color_hex(active ? 0x38bdf8 : 0x1e293b), 0);
        lv_obj_t *l = lv_obj_get_child(g_qs_rot_btns[i], 0);
        if (l) {
            lv_obj_set_style_text_color(l, lv_color_hex(active ? 0x070d19 : 0xe2e8f0), 0);
        }
    }
    if (g_qs_rot_btns[3]) {
        bool flipped = (g_rot_flip_180 != 0);
        lv_obj_set_style_bg_color(g_qs_rot_btns[3],
                                  lv_color_hex(flipped ? 0xf59e0b : 0x1e293b), 0);
        lv_obj_t *l = lv_obj_get_child(g_qs_rot_btns[3], 0);
        if (l) {
            lv_obj_set_style_text_color(l, lv_color_hex(flipped ? 0x070d19 : 0xe2e8f0), 0);
        }
    }

    static const int kBrtLevels[4] = {25, 50, 75, 100};
    for (int i = 0; i < 4; i++) {
        if (!g_qs_brt_btns[i]) continue;
        bool active = (g_user_brightness_pct == kBrtLevels[i]);
        lv_obj_set_style_bg_color(g_qs_brt_btns[i],
                                  lv_color_hex(active ? 0xf59e0b : 0x1e293b), 0);
        lv_obj_t *l = lv_obj_get_child(g_qs_brt_btns[i], 0);
        if (l) {
            lv_obj_set_style_text_color(l, lv_color_hex(active ? 0x070d19 : 0xe2e8f0), 0);
        }
    }

    static const int kSlpTimeouts[4] = {30, 60, 300, 0};
    for (int i = 0; i < 4; i++) {
        if (!g_qs_slp_btns[i]) continue;
        bool active = (g_sleep_timeout_sec == kSlpTimeouts[i]);
        lv_obj_set_style_bg_color(g_qs_slp_btns[i],
                                  lv_color_hex(active ? 0xa855f7 : 0x1e293b), 0);
        lv_obj_t *l = lv_obj_get_child(g_qs_slp_btns[i], 0);
        if (l) {
            lv_obj_set_style_text_color(l, lv_color_hex(active ? 0x070d19 : 0xe2e8f0), 0);
        }
    }

    static const int kKbDuties[4] = {0, 5, 25, 60};
    int cur_duty = keyboard_get_backlight_level();
    for (int i = 0; i < 4; i++) {
        if (!g_qs_kb_btns[i]) continue;
        bool active = (cur_duty == kKbDuties[i]);
        lv_obj_set_style_bg_color(g_qs_kb_btns[i],
                                  lv_color_hex(active ? 0x10b981 : 0x1e293b), 0);
        lv_obj_t *l = lv_obj_get_child(g_qs_kb_btns[i], 0);
        if (l) {
            lv_obj_set_style_text_color(l, lv_color_hex(active ? 0x070d19 : 0xe2e8f0), 0);
        }
    }

    if (g_qs_poweroff_btn && g_qs_poweroff_lbl) {
        lv_obj_set_style_bg_color(g_qs_poweroff_btn,
                                  lv_color_hex(g_qs_poweroff_confirm ? 0xdc2626 : 0x450a0a), 0);
        lv_label_set_text(g_qs_poweroff_lbl,
                          g_qs_poweroff_confirm ? "CONFIRM POWER OFF" : "POWER OFF (SHIP)");
    }
}

static void apply_effective_rotation(void) {
    int target_rot = compute_target_hw_rotation();
    if (target_rot == g_applied_hw_rot) {
        qs_refresh_ui();
        return;
    }
    bool is_landscape = (target_rot == 1 || target_rot == 3);
    g_applied_hw_rot = target_rot;

    /* 1. First update LVGL display driver hor_res/ver_res so ESP_Brookesia screen-size calibration matches */
    lexios_set_display_rotation(target_rot);

    int new_w = is_landscape ? 1232 : 568;
    int new_h = is_landscape ? 568 : 1232;

    /* 2. Switch ESP_Brookesia Phone Home, StatusBar, AppLauncher, NavBar & RecentsScreen stylesheet */
    if (g_phone_instance && g_sheets_initialized) {
        if (is_landscape) {
            if (!g_landscape_sheet_added) {
                if (g_phone_instance->addStylesheet(&g_sheet_landscape)) {
                    g_landscape_sheet_added = true;
                }
            }
            if (g_landscape_sheet_added) {
                g_phone_instance->activateStylesheet(&g_sheet_landscape);
            }
        } else {
            g_phone_instance->activateStylesheet(&g_sheet_portrait);
        }
        g_phone_instance->getHome().hideContainerBorder();
        style_home_wallpaper_gradient();
    }

    /* 3. If an app screen is currently active, resize both the app screen and its root container */
    if (lv_scr_act() && g_phone_instance &&
        lv_scr_act() != g_phone_instance->getHome().getMainScreen()) {
        lv_obj_set_pos(lv_scr_act(), 0, 56);
        lv_obj_set_size(lv_scr_act(), new_w, new_h - 56);
        lv_obj_t *app_root = lv_obj_get_child(lv_scr_act(), 0);
        if (app_root) {
            lv_obj_set_size(app_root, new_w, new_h - 56);
        }
    }
    dashboard_on_orientation_changed(new_w, new_h);
    qs_refresh_ui();
    if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
    if (lv_layer_top()) lv_obj_invalidate(lv_layer_top());
}

extern "C" void lexios_toggle_quick_settings(void) {
    if (!g_qs_drawer) return;
    g_qs_open = !g_qs_open;
    g_qs_poweroff_confirm = false;
    if (g_qs_open) {
        lv_obj_clear_flag(g_qs_drawer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_qs_drawer, LV_OBJ_FLAG_HIDDEN);
        flush_rotation_config_to_sdcard();
    }
    qs_refresh_ui();
}

extern "C" void lexios_cycle_rotation_mode(void) {
    g_rot_mode = (g_rot_mode + 1) % 3;
    save_rotation_config();
    apply_effective_rotation();
}

static void on_qs_pill_clicked(lv_event_t *e) {
    (void)e;
    lexios_toggle_quick_settings();
}

static void on_qs_rot_btn_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx <= 2) {
        g_rot_mode = idx;
    } else if (idx == 3) {
        g_rot_flip_180 = !g_rot_flip_180;
    }
    save_rotation_config();
    apply_effective_rotation();
}

static void on_qs_brt_btn_clicked(lv_event_t *e) {
    int brt = (int)(intptr_t)lv_event_get_user_data(e);
    g_user_brightness_pct = brt;
    g_screen_dimmed = false;
    vo_display_set_brightness(brt);
    save_rotation_config();
    qs_refresh_ui();
    if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
    if (lv_layer_top()) lv_obj_invalidate(lv_layer_top());
}

static void on_qs_slp_btn_clicked(lv_event_t *e) {
    int timeout_sec = (int)(intptr_t)lv_event_get_user_data(e);
    g_sleep_timeout_sec = timeout_sec;
    lexios_notify_user_activity();
    save_rotation_config();
    qs_refresh_ui();
}

static void on_qs_kb_btn_clicked(lv_event_t *e) {
    int duty = (int)(intptr_t)lv_event_get_user_data(e);
    keyboard_set_backlight_level(duty);
    save_rotation_config();
    qs_refresh_ui();
}

static void on_qs_sleep_now_clicked(lv_event_t *e) {
    (void)e;
    if (g_qs_open) {
        lexios_toggle_quick_settings();
    }
    lexios_set_screen_sleep(true);
}

static void lexios_full_power_off(const char *reason) {
    if (g_qs_open) {
        lexios_toggle_quick_settings();
    }
    flush_rotation_config_to_sdcard();
    printf("[OS-PM] Full Power Off triggered (%s): syncing FS, blanking AMOLED, cutting RF & VDD_5V + BQ25896 Ship Mode...\n",
           reason ? reason : "user");
    sync();
    if (gpio_fd > 0) {
        pin_gpio_t p42 = {.pin = 42, .val = 0};
        pin_gpio_t p44 = {.pin = 44, .val = 0};
        pin_gpio_t p2  = {.pin = 2,  .val = 0};
        pin_gpio_t p45 = {.pin = 45, .val = 0};
        ioctl(gpio_fd, GPIO_WRITE_LOW, &p42);
        ioctl(gpio_fd, GPIO_WRITE_LOW, &p44);
        ioctl(gpio_fd, GPIO_WRITE_LOW, &p2);
        ioctl(gpio_fd, GPIO_WRITE_LOW, &p45);
    }
    lexios_set_screen_sleep(true);
    usleep(50000);
    /* 1. Disconnect battery FET via BQ25896 Ship Mode (BATFET_DIS=1, BATFET_DLY=0) */
    k230_pmic_power_off_ship_mode();
    /* 2. Deassert K230 PMU OUT0 (GPIO70 / IO70) so U10 (MT9700) cuts main VDD_5V rail */
    if (gpio_fd > 0) {
        pin_gpio_t p70 = {.pin = 70, .val = 0};
        ioctl(gpio_fd, GPIO_DM_OUTPUT, &p70);
        ioctl(gpio_fd, GPIO_WRITE_LOW, &p70);
    }
}

extern "C" int lexios_get_user_brightness_pct(void) {
    return g_user_brightness_pct;
}

extern "C" void lexios_set_user_brightness_pct(int pct) {
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    g_user_brightness_pct = pct;
    g_screen_dimmed = false;
    if (!g_screen_asleep) {
        vo_display_set_brightness(pct);
    }
    save_rotation_config();
    flush_rotation_config_to_sdcard();
    qs_refresh_ui();
    if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
    if (lv_layer_top()) lv_obj_invalidate(lv_layer_top());
}

extern "C" int lexios_get_rot_mode(void) {
    return g_rot_mode;
}

extern "C" void lexios_set_rot_mode(int mode) {
    if (mode < 0 || mode > 2) return;
    g_rot_mode = mode;
    save_rotation_config();
    flush_rotation_config_to_sdcard();
    apply_effective_rotation();
}

extern "C" int lexios_get_rot_flip(void) {
    return g_rot_flip_180;
}

extern "C" void lexios_set_rot_flip(int flip) {
    g_rot_flip_180 = flip ? 1 : 0;
    save_rotation_config();
    flush_rotation_config_to_sdcard();
    apply_effective_rotation();
}

extern "C" int lexios_get_kb_backlight(void) {
    return keyboard_get_backlight_level();
}

extern "C" void lexios_set_kb_backlight(int duty) {
    keyboard_set_backlight_level(duty);
    save_rotation_config();
    flush_rotation_config_to_sdcard();
    qs_refresh_ui();
}

extern "C" bool lexios_get_battery_saver(void) {
    return g_manual_battery_saver;
}

extern "C" void lexios_set_battery_saver(bool enable) {
    g_manual_battery_saver = enable;
    if (enable) {
        if (g_os_cpu_dvfs_stage < 3) {
            apply_cpu_dvfs_stage(3, g_last_die_temp_c);
        }
        if (!g_screen_asleep && g_user_brightness_pct > 40) {
            vo_display_set_brightness(40);
        }
        keyboard_set_backlight_sleep(true);
    } else {
        int next_stage = compute_target_dvfs_stage(0, g_last_die_temp_c);
        apply_cpu_dvfs_stage(next_stage, g_last_die_temp_c);
        if (!g_screen_asleep && !g_screen_dimmed && !g_low_bat_throttled) {
            vo_display_set_brightness(g_user_brightness_pct);
            keyboard_set_backlight_sleep(false);
        }
    }
    save_rotation_config();
    flush_rotation_config_to_sdcard();
    qs_refresh_ui();
}

extern "C" void lexios_get_battery_telemetry(int *soc, int *vbat_mv, int *ibat_ma, int *rem_mah,
                                             int *full_mah, int *chg_state, bool *usb_present,
                                             bool *low_bat_throttled) {
    if (soc) *soc = g_pmic_status.soc_percent;
    if (vbat_mv) *vbat_mv = g_pmic_status.vbat_mv;
    if (ibat_ma) *ibat_ma = g_pmic_status.ibat_ma;
    if (rem_mah) *rem_mah = g_pmic_status.rem_cap_mah;
    if (full_mah) *full_mah = g_pmic_status.full_cap_mah;
    if (chg_state) *chg_state = g_pmic_status.charge_state;
    if (usb_present) *usb_present = g_pmic_status.usb_present;
    if (low_bat_throttled) *low_bat_throttled = g_low_bat_throttled;
}

extern "C" void lexios_request_power_off(void) {
    lexios_full_power_off("Settings App Power Off");
}

#define CTRL_WDT_SET_TIMEOUT_OS _IOW('W', 2, int)
#define CTRL_WDT_START_OS       _IOW('W', 5, int)

extern "C" void lexios_request_reboot(void) {
    flush_rotation_config_to_sdcard();
    printf("[OS-PM] Hardware Reboot requested from Settings App...\n");
    sync();
    if (gpio_fd > 0) {
        pin_gpio_t p45 = {.pin = 45, .val = 0};
        ioctl(gpio_fd, GPIO_DM_OUTPUT, &p45);
        ioctl(gpio_fd, GPIO_WRITE_LOW, &p45);
        usleep(150000);
        p45.val = 1;
        ioctl(gpio_fd, GPIO_WRITE_HIGH, &p45);
    }
    const char *wdt_devs[2] = {"/dev/watchdog1", "/dev/watchdog0"};
    for (int i = 0; i < 2; i++) {
        int wfd = open(wdt_devs[i], O_RDWR);
        if (wfd >= 0) {
            unsigned int timeout = 1;
            ioctl(wfd, CTRL_WDT_SET_TIMEOUT_OS, &timeout);
            ioctl(wfd, CTRL_WDT_START_OS, NULL);
            sleep(3);
            close(wfd);
        }
    }
}

static void init_physical_buttons(void) {
    /* Configure SW3 ('0' button on GPIO0 / BOOT0, ball C8, active-LOW with external 10k pull-up R8 to 1.8V) */
    fpioa_set_function(0, GPIO0, 1, 0, 1, 0, -1, 1, -1);
    if (gpio_fd > 0) {
        pin_gpio_t pin_0 = {.pin = 0, .val = 1};
        ioctl(gpio_fd, GPIO_DM_INPUT_PULL_UP, &pin_0);
    }
    printf("[OS-BTN] Physical buttons initialized: SW1=RST (HW Reset), SW3='0' (GPIO0: Tap=Quick Settings/Wake, 1.5s=Rotate, 4s=Power Off)\n");
}

static void poll_physical_buttons(const struct timespec *now_ts) {
    static long last_poll_ms = 0;
    long now_ms = now_ts->tv_sec * 1000L + now_ts->tv_nsec / 1000000L;
    if (last_poll_ms != 0 && (now_ms - last_poll_ms) < 25) {
        return;
    }
    long dt_ms = (last_poll_ms == 0) ? 25 : (now_ms - last_poll_ms);
    if (dt_ms > 200) dt_ms = 200;
    last_poll_ms = now_ms;

    /* Poll SW3 ('0' button on GPIO0 / BOOT0, active-LOW) */
    static long sw3_idle_ms = 0;
    static bool sw3_armed = false;
    static long sw3_press_ms = 0;
    static bool sw3_long_handled = false;

    bool sw3_raw_pressed = false;
    if (gpio_fd > 0) {
        pin_gpio_t pin_0 = {.pin = 0, .val = 1};
        if (ioctl(gpio_fd, GPIO_READ_VALUE, &pin_0) == 0) {
            sw3_raw_pressed = (pin_0.val == 0);
        }
    }
    if (!sw3_armed) {
        if (!sw3_raw_pressed) {
            sw3_idle_ms += dt_ms;
            if (sw3_idle_ms >= 1000) {
                sw3_armed = true;
            }
        } else {
            sw3_idle_ms = 0;
        }
    } else {
        if (sw3_raw_pressed) {
            sw3_press_ms += dt_ms;
            if (sw3_press_ms >= 4000 && !sw3_long_handled) {
                sw3_long_handled = true;
                lexios_full_power_off("SW3 '0' button 4s hold");
            }
        } else {
            if (sw3_press_ms >= 60 && !sw3_long_handled) {
                if (g_screen_asleep) {
                    /* When asleep, pressing '0' wakes the screen immediately */
                    printf("[OS-BTN] SW3 ('0') press (%ld ms) while asleep -> Wake Screen\n", sw3_press_ms);
                    lexios_set_screen_sleep(false);
                } else if (sw3_press_ms >= 1500) {
                    /* Medium hold (1.5s .. 3.9s) on '0' button: cycle Portrait/Landscape rotation */
                    printf("[OS-BTN] SW3 ('0') medium hold (%ld ms) -> Cycle Screen Rotation\n", sw3_press_ms);
                    lexios_notify_user_activity();
                    lexios_cycle_rotation_mode();
                } else {
                    /* Quick press (< 1.5s) on '0' button: toggle Quick Settings drawer */
                    printf("[OS-BTN] SW3 ('0') quick press (%ld ms) -> Toggle Quick Settings\n", sw3_press_ms);
                    lexios_notify_user_activity();
                    lexios_toggle_quick_settings();
                }
            }
            sw3_press_ms = 0;
            sw3_long_handled = false;
        }
    }
}

static void on_qs_poweroff_clicked(lv_event_t *e) {
    (void)e;
    if (!g_qs_poweroff_confirm) {
        g_qs_poweroff_confirm = true;
        qs_refresh_ui();
        return;
    }
    g_qs_poweroff_confirm = false;
    lexios_full_power_off("Quick Settings Power Off");
}

static void detach_from_default_group(lv_obj_t *obj) {
    if (obj && lv_group_get_default()) {
        lv_group_remove_obj(obj);
    }
}

static void init_quick_settings_drawer(void) {
    lv_obj_t *top = lv_layer_top();
    if (!top) return;

    /* Top-left status bar pill button inset inside rounded AMOLED corner safe zone */
    g_qs_pill_btn = lv_btn_create(top);
    detach_from_default_group(g_qs_pill_btn);
    lv_obj_set_size(g_qs_pill_btn, 140, 36);
    lv_obj_set_pos(g_qs_pill_btn, 52, 10);
    lv_obj_set_style_bg_color(g_qs_pill_btn, lv_color_hex(0x0b1324), 0);
    lv_obj_set_style_bg_opa(g_qs_pill_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_qs_pill_btn, 1, 0);
    lv_obj_set_style_border_color(g_qs_pill_btn, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_radius(g_qs_pill_btn, 18, 0);
    lv_obj_set_style_pad_all(g_qs_pill_btn, 4, 0);
    lv_obj_add_event_cb(g_qs_pill_btn, on_qs_pill_clicked, LV_EVENT_CLICKED, NULL);

    g_qs_pill_lbl = lv_label_create(g_qs_pill_btn);
    lv_label_set_text(g_qs_pill_lbl, "AUTO:PORT v");
    lv_obj_set_style_text_color(g_qs_pill_lbl, lv_color_hex(0x38bdf8), 0);
    lv_obj_center(g_qs_pill_lbl);

    /* Pull-down Quick Settings drawer on lv_layer_top() */
    g_qs_drawer = lv_obj_create(top);
    detach_from_default_group(g_qs_drawer);
    lv_obj_set_size(g_qs_drawer, 512, 392);
    lv_obj_align(g_qs_drawer, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(g_qs_drawer, lv_color_hex(0x0b1324), 0);
    lv_obj_set_style_bg_opa(g_qs_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_qs_drawer, 2, 0);
    lv_obj_set_style_border_color(g_qs_drawer, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_radius(g_qs_drawer, 18, 0);
    lv_obj_set_style_pad_all(g_qs_drawer, 12, 0);
    lv_obj_clear_flag(g_qs_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_qs_drawer, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(g_qs_drawer);
    lv_label_set_text(title, "QUICK SETTINGS  (Fn+R: Rotate | Fn+Q: Drawer | Fn+S: Sleep)");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38bdf8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_btn_create(g_qs_drawer);
    detach_from_default_group(close_btn);
    lv_obj_set_size(close_btn, 84, 28);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(close_btn, 6, 0);
    lv_obj_add_event_cb(close_btn, on_qs_pill_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "CLOSE ^");
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xe2e8f0), 0);
    lv_obj_center(close_lbl);

    g_qs_info_lbl = lv_label_create(g_qs_drawer);
    lv_label_set_text(g_qs_info_lbl, "Wi-Fi: --  |  KB: --  |  Die: -- C");
    lv_obj_set_style_text_color(g_qs_info_lbl, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_pos(g_qs_info_lbl, 0, 26);

    g_qs_pwr_lbl = lv_label_create(g_qs_drawer);
    lv_label_set_text(g_qs_pwr_lbl, "Power: --  |  Bat: --%");
    lv_obj_set_style_text_color(g_qs_pwr_lbl, lv_color_hex(0x34d399), 0);
    lv_obj_set_pos(g_qs_pwr_lbl, 0, 48);

    /* Row 1: Orientation Buttons */
    lv_obj_t *rot_row = lv_obj_create(g_qs_drawer);
    lv_obj_remove_style_all(rot_row);
    lv_obj_set_size(rot_row, lv_pct(100), 48);
    lv_obj_set_pos(rot_row, 0, 76);
    lv_obj_set_flex_flow(rot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rot_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rot_row, 8, 0);

    static const char *kRotNames[4] = {"AUTO (KB)", "PORTRAIT", "LANDSCAPE", "FLIP 180"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(rot_row);
        detach_from_default_group(b);
        lv_obj_set_height(b, 42);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x334155), 0);
        lv_obj_add_event_cb(b, on_qs_rot_btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kRotNames[i]);
        lv_obj_center(l);
        g_qs_rot_btns[i] = b;
    }

    /* Row 2: Screen Brightness */
    lv_obj_t *brt_row = lv_obj_create(g_qs_drawer);
    lv_obj_remove_style_all(brt_row);
    lv_obj_set_size(brt_row, lv_pct(100), 48);
    lv_obj_set_pos(brt_row, 0, 134);
    lv_obj_set_flex_flow(brt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brt_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brt_row, 8, 0);

    static const char *kBrtNames[4] = {"BRT 25%", "BRT 50%", "BRT 75%", "BRT 100%"};
    static const int kBrtLevels[4] = {25, 50, 75, 100};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(brt_row);
        detach_from_default_group(b);
        lv_obj_set_height(b, 42);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x334155), 0);
        lv_obj_add_event_cb(b, on_qs_brt_btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)kBrtLevels[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kBrtNames[i]);
        lv_obj_center(l);
        g_qs_brt_btns[i] = b;
    }

    /* Row 3: Auto-Dim & Screen Sleep Timeout */
    lv_obj_t *slp_row = lv_obj_create(g_qs_drawer);
    lv_obj_remove_style_all(slp_row);
    lv_obj_set_size(slp_row, lv_pct(100), 48);
    lv_obj_set_pos(slp_row, 0, 192);
    lv_obj_set_flex_flow(slp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slp_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slp_row, 8, 0);

    static const char *kSlpNames[4] = {"SLEEP 30s", "SLEEP 1m", "SLEEP 5m", "NEVER"};
    static const int kSlpTimeouts[4] = {30, 60, 300, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(slp_row);
        detach_from_default_group(b);
        lv_obj_set_height(b, 42);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x334155), 0);
        lv_obj_add_event_cb(b, on_qs_slp_btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)kSlpTimeouts[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kSlpNames[i]);
        lv_obj_center(l);
        g_qs_slp_btns[i] = b;
    }

    /* Row 4: Keyboard Backlight */
    lv_obj_t *kb_row = lv_obj_create(g_qs_drawer);
    lv_obj_remove_style_all(kb_row);
    lv_obj_set_size(kb_row, lv_pct(100), 48);
    lv_obj_set_pos(kb_row, 0, 250);
    lv_obj_set_flex_flow(kb_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kb_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(kb_row, 8, 0);

    static const char *kKbNames[4] = {"KB OFF", "KB LOW", "KB MED", "KB HIGH"};
    static const int kKbDuties[4] = {0, 5, 25, 60};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(kb_row);
        detach_from_default_group(b);
        lv_obj_set_height(b, 42);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x334155), 0);
        lv_obj_add_event_cb(b, on_qs_kb_btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)kKbDuties[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kKbNames[i]);
        lv_obj_center(l);
        g_qs_kb_btns[i] = b;
    }

    /* Row 5: Sleep Screen Now & Power Off (Ship Mode) */
    lv_obj_t *act_row = lv_obj_create(g_qs_drawer);
    lv_obj_remove_style_all(act_row);
    lv_obj_set_size(act_row, lv_pct(100), 50);
    lv_obj_set_pos(act_row, 0, 310);
    lv_obj_set_flex_flow(act_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act_row, 10, 0);

    lv_obj_t *slp_now_btn = lv_btn_create(act_row);
    detach_from_default_group(slp_now_btn);
    lv_obj_set_height(slp_now_btn, 44);
    lv_obj_set_flex_grow(slp_now_btn, 1);
    lv_obj_set_style_radius(slp_now_btn, 8, 0);
    lv_obj_set_style_bg_color(slp_now_btn, lv_color_hex(0x1e3a8a), 0);
    lv_obj_set_style_border_width(slp_now_btn, 1, 0);
    lv_obj_set_style_border_color(slp_now_btn, lv_color_hex(0x38bdf8), 0);
    lv_obj_add_event_cb(slp_now_btn, on_qs_sleep_now_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *slp_now_lbl = lv_label_create(slp_now_btn);
    lv_label_set_text(slp_now_lbl, "SLEEP SCREEN (Fn+S)");
    lv_obj_set_style_text_color(slp_now_lbl, lv_color_hex(0xe2e8f0), 0);
    lv_obj_center(slp_now_lbl);

    g_qs_poweroff_btn = lv_btn_create(act_row);
    detach_from_default_group(g_qs_poweroff_btn);
    lv_obj_set_height(g_qs_poweroff_btn, 44);
    lv_obj_set_flex_grow(g_qs_poweroff_btn, 1);
    lv_obj_set_style_radius(g_qs_poweroff_btn, 8, 0);
    lv_obj_set_style_bg_color(g_qs_poweroff_btn, lv_color_hex(0x450a0a), 0);
    lv_obj_set_style_border_width(g_qs_poweroff_btn, 1, 0);
    lv_obj_set_style_border_color(g_qs_poweroff_btn, lv_color_hex(0xf43f5e), 0);
    lv_obj_add_event_cb(g_qs_poweroff_btn, on_qs_poweroff_clicked, LV_EVENT_CLICKED, NULL);
    g_qs_poweroff_lbl = lv_label_create(g_qs_poweroff_btn);
    lv_label_set_text(g_qs_poweroff_lbl, "POWER OFF (SHIP)");
    lv_obj_set_style_text_color(g_qs_poweroff_lbl, lv_color_hex(0xfecdd3), 0);
    lv_obj_center(g_qs_poweroff_lbl);
}

#if 1
static void on_clock_update_timer_cb(struct _lv_timer_t *t)
{
    time_t now;
    struct tm timeinfo;
    bool is_time_pm = false;
    ESP_Brookesia_Phone *phone = (ESP_Brookesia_Phone *)t->user_data;

    time(&now);
    //printf("当前系统时间戳：%lld（应与at_ts一致)\n", (long long)now);
    localtime_r(&now, &timeinfo);
    is_time_pm = (timeinfo.tm_hour >= 12);

    /* Since this callback is called from LVGL task, it is safe to operate LVGL */
    // Update clock on "Status Bar"
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        phone->getHome().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min, is_time_pm),
        "Refresh status bar failed"
    );
    {
        int wifi_state = 0;
        FILE *wf = fopen("/tmp/wifi_status.txt", "r");
        if (wf) {
            char wbuf[96] = {0};
            bool connected = false;
            int rssi = -55;
            if (fgets(wbuf, sizeof(wbuf), wf) && strncmp(wbuf, "CONNECTED", 9) == 0) {
                connected = true;
                while (fgets(wbuf, sizeof(wbuf), wf)) {
                    if (strncmp(wbuf, "RSSI=", 5) == 0) {
                        rssi = atoi(wbuf + 5);
                        break;
                    }
                }
            }
            fclose(wf);
            if (connected) {
                if (rssi > -60) wifi_state = 3;
                else if (rssi > -75) wifi_state = 2;
                else wifi_state = 1;
            }
        }
        phone->getHome().getStatusBar()->setWifiIconState(wifi_state);
    }
    /* Poll BQ25896 charger + BQ27220 fuel gauge every 2 ticks & enforce Two-Stage Low-Battery UVLO Guardian */
    {
        static int pmic_div = 0;
        pmic_div = (pmic_div + 1) & 1;
        if (pmic_div == 1) {
            bool prev_usb = g_pmic_status.usb_present;
            bool had_valid = g_pmic_status.valid;
            if (k230_pmic_poll_status(&g_pmic_status)) {
                bool is_chg = (g_pmic_status.charge_state == 1 || g_pmic_status.charge_state == 2);
                phone->getHome().getStatusBar()->setBatteryPercent(is_chg, g_pmic_status.soc_percent);
                if (had_valid && prev_usb != g_pmic_status.usb_present) {
                    /* Briefly wake screen when USB charging cable is plugged or unplugged */
                    if (lexios_is_screen_asleep()) {
                        lexios_set_screen_sleep(false);
                    } else {
                        lexios_notify_user_activity();
                    }
                }

                /* Two-Stage Low-Battery Brownout Prevention & Automatic Graceful Shutdown (UVLO Guardian) */
                bool on_battery_discharging = !g_pmic_status.usb_present && !is_chg &&
                                              (g_pmic_status.vbat_mv >= 2500);
                if (on_battery_discharging) {
                    /* Stage 1 (<= 10% SOC or VBAT <= 3420 mV): Pre-emptive brownout prevention
                     * Cap AMOLED brightness to <= 30%, turn off keyboard LED, and step CPU1 down to <= 594 MHz
                     * so transient current spikes cannot pull VSYS below the regulator dropout voltage. */
                    if (g_pmic_status.soc_percent <= 10 || g_pmic_status.vbat_mv <= 3420) {
                        if (!g_low_bat_throttled) {
                            g_low_bat_throttled = true;
                            printf("[OS-PM] LOW BATTERY WARNING: VBAT=%dmV (Vocv=%dmV) SOC=%d%% -> Capping AMOLED <=30%% & CPU1 <=594MHz to prevent brownout\n",
                                   g_pmic_status.vbat_mv, g_pmic_status.vocv_mv, g_pmic_status.soc_percent);
                            if (!g_screen_asleep && g_user_brightness_pct > 30) {
                                vo_display_set_brightness(30);
                            }
                            keyboard_set_backlight_sleep(true);
                            if (g_os_cpu_dvfs_stage < 3) {
                                apply_cpu_dvfs_stage(3, g_last_die_temp_c);
                            }
                        }
                    } else if (g_low_bat_throttled && g_pmic_status.soc_percent >= 14 && g_pmic_status.vbat_mv >= 3520) {
                        g_low_bat_throttled = false;
                        if (!g_screen_asleep && !g_screen_dimmed) {
                            vo_display_set_brightness(g_user_brightness_pct);
                            keyboard_set_backlight_sleep(false);
                        }
                    }

                    /* Stage 2 (<= 4% SOC or VBAT <= 3320 mV sustained for 3 polls (~6s), or immediate <= 3220 mV):
                     * Graceful OS shutdown into BQ25896 Ship Mode + VDD_5V cut BEFORE voltage sags into brownout! */
                    if (g_pmic_status.vbat_mv <= 3220) {
                        g_low_bat_crit_polls = 3;
                    } else if (g_pmic_status.soc_percent <= 4 || g_pmic_status.vbat_mv <= 3320) {
                        g_low_bat_crit_polls++;
                        printf("[OS-PM] CRITICAL BATTERY (%d/3): VBAT=%dmV (Vocv=%dmV) SOC=%d%%\n",
                               g_low_bat_crit_polls, g_pmic_status.vbat_mv, g_pmic_status.vocv_mv, g_pmic_status.soc_percent);
                    } else {
                        g_low_bat_crit_polls = 0;
                    }

                    if (g_low_bat_crit_polls >= 3) {
                        lexios_full_power_off("Critical Low Battery UVLO (<3.32V / <=4%)");
                        return;
                    }
                } else {
                    g_low_bat_crit_polls = 0;
                    if (g_low_bat_throttled && (g_pmic_status.usb_present || is_chg)) {
                        g_low_bat_throttled = false;
                        if (!g_screen_asleep && !g_screen_dimmed) {
                            vo_display_set_brightness(g_user_brightness_pct);
                            keyboard_set_backlight_sleep(false);
                        }
                    }
                }
            }
        }
    }
    /* Hot-plug poll for physical keyboard module on I2C4 (0x34) & auto-rotate */
    {
        static int kb_poll_div = 0;
        bool prev_kb = tca8418_is_connected();
        bool now_kb = prev_kb;
        kb_poll_div = (kb_poll_div + 1) & 3;
        if (prev_kb || kb_poll_div == 0 || g_applied_hw_rot < 0) {
            now_kb = tca8418_poll_presence();
        }
        if (prev_kb != now_kb || g_applied_hw_rot < 0) {
            apply_effective_rotation();
        } else if (g_qs_open) {
            qs_refresh_ui();
        }
    }
    //printf("driver->screen_transp:%d\n", lv_disp_get_default()->driver->screen_transp);
    //printf("disp_bg_opa:%d\n", lv_disp_get_default()->bg_opa);
    //printf("+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-\n");
    #if defined(USING_BATTERY)    
    memset(bq27220_data,0,2);
    bq27220_read_cmd_data(fd_i2c0,0x2C,bq27220_data,2);
    int bq27220_soc=((bq27220_data[1]<<8)|bq27220_data[0]);
    //printf("bq27220 get_soc():%02x %02x,value:%d%%\n",bq27220_data[0],bq27220_data[1],bq27220_soc);
     ESP_BROOKESIA_CHECK_FALSE_EXIT(
        phone->getHome().getStatusBar()->setBatteryPercent(false,bq27220_soc),
        "Refresh status bar failed"
    ); //true
    #endif
    //temperature control fan
    #if defined(USING_FAN)
    //read temperature if temperature>60 open fan else close fan
    uint32_t buffer[1] = {0};
    uint32_t ts_val = 0;
    double code = 0, temp = 0;
    int ret=read(fd_ts,buffer,0);
    if(ret>0)
    {
    ts_val = *(uint32_t *)buffer;
    code = (double)(ts_val & 0xfff);
    temp = (1e-10 * pow(code, 4) * 1.01472 - 1e-6 * pow(code, 3) * 1.10063 + 4.36150 * 1e-3 * pow(code, 2) - 7.10128 * code + 3565.87);
    //printf("ts_val: 0x%x, TS = %lf C\n", ts_val, temp); 
    }
    if (gpio_fd>0)
    {
      pin_gpio_t pin_42;
      pin_42.pin = 42;
      //ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_42);  //pin42 output
      if(temp>50)
      {
      ioctl(gpio_fd, GPIO_WRITE_HIGH, &pin_42);
      }
      else if(temp<40)
      {
      ioctl(gpio_fd, GPIO_WRITE_LOW, &pin_42);
      }
      //close(gpio_fd);
        
    }
    #endif 
}
#endif


#if defined(USING_LTE_GNSS)
static void on_clock_ltem_time_cb(struct _lv_timer_t *t)
{
printf("on_clock_ltem_time_cb start\n");
fd_uart = open(UART_DEVICE_NAME, O_RDWR);
    if (fd_uart>0)
    {
    struct uart_configure config = {
        .baud_rate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = UART_PARITY_NONE,
        .fifo_lenth = UART_RECEIVE_FIFO_16,
        .auto_flow = 0,
    };


    if (ioctl(fd_uart, IOC_SET_BAUDRATE, &config))
    {
        printf("uart2 ioctl failed!\n");
    }
    
   
    char msg_send[25]={0};
    sprintf(msg_send, "AT+CCLK?\r\n");
    write(fd_uart, msg_send, strlen(msg_send));
    int try_cnt=0;
    int ret = 0, cnt = 0; 
    string cclk_arr[3]={""};
    while(try_cnt<10)
    {  usleep(100000);
     ret = read(fd_uart, &buf_rx[ret], BUF_SIZE-cnt);
            if (ret > 0 && ret <= BUF_SIZE)
            {

                cnt += ret;
            }
            printf("package size %d\n", ret);
            if(ret > 0)
            { 
                 int  is_end=0;
                 printf("[app] read %d byte\n", cnt);
                printf("uart recv msg:%s\n",buf_rx);           
                 if(buf_rx[0]==0x00)
                {
                buf_rx[0]='.';
                }
                printf("++++++++++++++++++++++\n");
                char *saveptr;
                char*token=strtok_r(buf_rx,"\r\n",&saveptr);
                 while (token != NULL) {
                  printf("%s\n",token);
                  if(strstr(token,"+CCLK:")&&strlen(token)>12)//12
                  { 
                    char *saveptr1;
                    char*token1=strtok_r(token,"\"",&saveptr1);
                    
                    int i=0;
                    printf("************\n");
                     while (token1 != NULL) {
                        printf("%s\n", token1);
                        cclk_arr[i]=token1;
                        token1 = strtok_r(NULL, "\"", &saveptr1);
                     i++;
                     }
                    printf("************\n");
                    is_end=1;
                  break;
                  }
                   token = strtok_r(NULL, "\r\n", &saveptr);
                  }
                printf("---------------------\n");
                memset(buf_rx,0,BUF_SIZE);
                ret = 0;
                cnt = 0;
                if(is_end==1)
                {
                printf("try_cnt:%d\n",try_cnt);
                break;
                }
            } 
      try_cnt++;
      
    }
//////////////////////////////////////////
 //fuzhi
                if(cclk_arr[0]!="") 
                {           
    int year_2d, mon, day, hour, min, sec;
    char tz_sign; // 时区符号（+/-）
    int tz_val;   // 时区数值（示例中是32）
    int parsed = sscanf(cclk_arr[1].c_str(), "%d/%d/%d,%d:%d:%d%c%d",
                        &year_2d, &mon, &day, &hour, &min, &sec, &tz_sign, &tz_val);
    struct tm at_time={0};
    at_time.tm_year = year_2d + 100;;
    at_time.tm_mon = mon-1;      
    at_time.tm_mday = day;
    at_time.tm_hour = hour;
    at_time.tm_min = min;
    at_time.tm_sec = sec;
    at_time.tm_isdst = -1; 
   time_t at_ts = mktime(&at_time); // 转为time_t             
  struct timespec ts;
  ts.tv_sec = at_ts;    // 秒级时间戳
  ts.tv_nsec = 0;       // 纳秒（设为0即可）
  // 设置CLOCK_REALTIME（实时时钟）
  clock_settime(CLOCK_REALTIME, &ts);  
  }


}

// 执行完后删除定时器，避免重复触发
    lv_timer_del(t);
printf("on_clock_ltem_time_cb end\n");

}





#endif

#if 0
#include <signal.h>
typedef enum {
    HWTIMER_CTRL_FREQ_SET = 19 * 0x100 + 0x01,           /* set the count frequency */
    HWTIMER_CTRL_STOP = 19 * 0x100 + 0x02,               /* stop timer */
    HWTIMER_CTRL_INFO_GET = 19 * 0x100 + 0x03,           /* get a timer feature information */
    HWTIMER_CTRL_MODE_SET = 19 * 0x100 + 0x04,           /* Setting the timing mode(oneshot/period) */
    HWTIMER_CTRL_IRQ_SET = 19 * 0x100 + 0x10,
} hwtimer_ctrl_t;

typedef struct {
    int32_t sec;      /* second */
    int32_t usec;     /* microsecond */
} hwtimerval_t;

typedef struct {
    uint8_t enable;
    uint8_t signo;
    void *sigval;
} hwtimer_irqcfg_t;

typedef enum {
    HWTIMER_MODE_ONESHOT = 0x01,
    HWTIMER_MODE_PERIOD
} hwtimer_mode_t;

//#define TIMER1_SIG SIGUSR1
//#define TIMER2_SIG SIGUSR2
#define TIMER1_SIG SIGUSR2
#define errExit(msg)        \
    do {                    \
        perror(msg);        \
        exit(EXIT_FAILURE); \
    } while (0)






void hwtimer_handler_timeout(int sig)
{
    /* Note: calling printf() from a signal handler is not safe
       (and should not be done in production programs), since
       printf() is not async-signal-safe; see signal-safety(7).
       Nevertheless, we use printf() here as a simple way of
       showing that the handler was called. */

    printf("Caught2 signal %d\n", sig);
    //hwtimerval_t val;
    //read(fd_hwtimer1, &val, sizeof(val));
}

#endif



/*
static void btn_clicked_event(lv_event_t * e)
{
    //lv_event_code_t code = lv_event_get_code(e);
    //lv_obj_t * label = lv_obj_get_child(btn, 0);
    char *btn_name=lv_event_get_user_data(e);
    printf("btn_name:%s\n",btn_name);
    
}*/
void user_gui_init()
{
    boot_log("user_gui_init: enter");
#if 1
    lv_disp_t*disp=lv_disp_get_default();
 
 /* Create a phone object */
    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone(disp);
    ESP_BROOKESIA_CHECK_NULL_EXIT(phone, "Create phone failed");
    g_phone_instance = phone;

    /* Initialize Portrait (568x1232) & Landscape (1232x568) stylesheets */
    init_orientation_stylesheets();
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->addStylesheet(&g_sheet_portrait), "Add portrait stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->activateStylesheet(&g_sheet_portrait), "Activate stylesheet failed");
    boot_log("user_gui_init: stylesheet activated");

    lv_indev_t *indev_touch = NULL;
    for (lv_indev_t *cur = lv_indev_get_next(NULL); cur != NULL; cur = lv_indev_get_next(cur)) {
        if (lv_indev_get_type(cur) == LV_INDEV_TYPE_POINTER) {
            indev_touch = cur;
            break;
        }
    }
    /* Configure and begin the phone */
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->setTouchDevice(indev_touch), "Set touch device failed");
    boot_log("user_gui_init: setTouchDevice ok");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");
    style_home_wallpaper_gradient();
    boot_log("user_gui_init: phone->begin() ok");

    /* Install LexiOS First-Party & Curated Mobile Apps */
    SuperLexiconApp *app_superlexicon = new SuperLexiconApp();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_superlexicon, "Create app superlexicon failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_superlexicon) >= 0), "Install app superlexicon failed");

    LexiSystemApp *app_lexisystem = new LexiSystemApp();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_lexisystem, "Create app lexisystem failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_lexisystem) >= 0), "Install app lexisystem failed");

    LexiSatApp *app_lexisat = new LexiSatApp();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_lexisat, "Create app lexisat failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_lexisat) >= 0), "Install app lexisat failed");

    LexiRFApp *app_lexirf = new LexiRFApp();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_lexirf, "Create app lexirf failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_lexirf) >= 0), "Install app lexirf failed");

    LexiPTTApp *app_lexiptt = new LexiPTTApp();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_lexiptt, "Create app lexiptt failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_lexiptt) >= 0), "Install app lexiptt failed");

    Lora *app_lora = new Lora();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_lora, "Create app lora failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_lora) >= 0), "Install app lora failed");

    AppSettings *app_setting = new AppSettings();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_setting, "Create app setting failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_setting) >= 0), "Install app setting failed");

    Camera *app_camera = new Camera();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_camera, "Create app camera failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_camera) >= 0), "Install app camera failed");

    Clock *app_clock = new Clock();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_clock, "Create app clock failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_clock) >= 0), "Install app clock failed");

    Calculator *app_calculator = new Calculator();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_calculator, "Create app calculator failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_calculator) >= 0), "Install app calculator failed");

    boot_log("user_gui_init: apps installed");
    init_quick_settings_drawer();
    load_rotation_config();
    apply_effective_rotation();
    boot_log("user_gui_init: quick settings & rotation applied");

    /* Immediately render and flush the first LVGL frame to RM69A10 so the user sees the home screen right away */
    lv_tick_inc(20);
    lv_task_handler();
    boot_log("user_gui_init: first LVGL frame flushed");

    g_boot_time_sec = monotonic_sec_now();
    g_last_activity_sec = g_boot_time_sec;

    gpio_fd = open("/dev/gpio", O_RDWR);
    if (gpio_fd > 0)
    {
       pin_gpio_t pin_42;
       pin_42.pin = 42;
       ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_42);  //pin42 fan output
       ioctl(gpio_fd, GPIO_WRITE_LOW, &pin_42);
    }
    init_physical_buttons();
    fd_ts = open("/dev/ts", O_RDWR);
    if (fd_ts <= 0)
    {
       printf("open /dev/ts fail\n");
    }
    pthread_t thermal_tid;
    if (pthread_create(&thermal_tid, NULL, os_thermal_guardian_thread, NULL) == 0) {
       pthread_detach(thermal_tid);
    }

    // nRF9151 3.3v: keep OFF unless USING_LTE_GNSS is explicitly enabled to avoid idle modem heat
    fpioa_set_function(2,GPIO2,-1,-1,-1,-1,-1,-1,-1); 
    pin_gpio_t pin_2;
    pin_2.pin = 2;
    ioctl(gpio_fd, GPIO_DM_OUTPUT, &pin_2);  //pin2 output
    ioctl(gpio_fd, GPIO_WRITE_LOW, &pin_2);

    /* IMPORTANT: Do NOT touch GPIO45 (IO45_WIFI_EN) here!
     * U-Boot configures IO45 with internal pull-up HIGH (PU=1) so U24 (WIFI_3V3) continuously
     * powers the Realtek RTL8189 SDIO chip from kernel boot (realtek_init). Calling GPIO_DM_OUTPUT
     * on GPIO45 glitches DATA_OUTPUT LOW (0) and resets the RTL8189 behind the SDIO driver's back! */

    /* Create a timer to update the clock */
    ESP_BROOKESIA_CHECK_NULL_EXIT(lv_timer_create(on_clock_update_timer_cb, 1000, phone), "Create clock update timer failed");
#endif
    boot_log("user_gui_init: complete");
}

int main(int argc, char **argv)
{
    unlink("/tmp/load_temp");
    FILE *fp = fopen("/sdcard/display_boot.log", "w");
    if (fp) {
        fprintf(fp, "=== LexiOS Display Boot ===\n");
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
    }
    boot_log("main: lv_init");
    lv_init();
    boot_log("main: lv_port_disp_init");
    lv_port_disp_init();
    if (lv_scr_act()) {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x070d19), 0);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
        lv_tick_inc(20);
        lv_task_handler();
    }
    boot_log("main: lv_port_indev_init");
    lv_port_indev_init();
    boot_log("main: user_gui_init");
    user_gui_init();






#if 0
static int fd_hwtimer1=0;
 struct sigaction sa;   
    sa.sa_flags = SA_SIGINFO;
    sa.sa_handler=hwtimer_handler_timeout;
    sigemptyset(&sa.sa_mask);
    if (sigaction(TIMER1_SIG, &sa, NULL) == -1)
        errExit("sigaction");
    if(fd_hwtimer1==0)
    {
        fd_hwtimer1 = open("/dev/hwtimer1", O_RDWR);
	if (fd_hwtimer1 < 0) {
        perror("open /dev/hwtimer1");
        return false;
         }
    }
    hwtimer_irqcfg_t irqcfg;
    irqcfg.enable = 1;
    irqcfg.signo = TIMER1_SIG;
    irqcfg.sigval = (void *)(uint64_t)1;
    ioctl(fd_hwtimer1, HWTIMER_CTRL_IRQ_SET, &irqcfg);
    hwtimer_mode_t mode = HWTIMER_MODE_PERIOD;
    ioctl(fd_hwtimer1, HWTIMER_CTRL_MODE_SET, &mode);
     hwtimerval_t val;
    	val.sec = 0;
        val.usec = 1000000;
        write(fd_hwtimer1, &val, sizeof(val));
#endif




    
    #if 0
     /*Change the active screen's background color*/
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x003a57), LV_PART_MAIN);

    /*Create a white label, set its text and align it to the center*/
    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello world");
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    #endif
    #if 0
    
    lv_obj_t * label;

    lv_obj_t * btn1 = lv_button_create(lv_screen_active());
    lv_obj_add_event_cb(btn1, event_handler, LV_EVENT_ALL, NULL);
    lv_obj_align(btn1, LV_ALIGN_CENTER, 0, -40);
    lv_obj_remove_flag(btn1, LV_OBJ_FLAG_PRESS_LOCK);

    label = lv_label_create(btn1);
    lv_label_set_text(label, "Button");
    lv_obj_center(label);

    lv_obj_t * btn2 = lv_button_create(lv_screen_active());
    lv_obj_add_event_cb(btn2, event_handler, LV_EVENT_ALL, NULL);
    lv_obj_align(btn2, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(btn2, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_height(btn2, LV_SIZE_CONTENT);

    label = lv_label_create(btn2);
    lv_label_set_text(label, "Toggle");
    lv_obj_center(label);   
    #endif

uint8_t reg_data;
struct timespec last_ts;
clock_gettime(CLOCK_MONOTONIC, &last_ts);

while(1)
    {
    if (g_os_thermal_level >= 2) {
        usleep(250000);
        clock_gettime(CLOCK_MONOTONIC, &last_ts);
        continue;
    }
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);

    // Poll physical buttons: SW2 (BOOT/PWR on IO64/INT0) & SW3 ('0' on GPIO0)
    poll_physical_buttons(&now_ts);

    // Check touch/keyboard inactivity timeout for two-stage Auto-Dim -> Pocket Standby
    if (!g_screen_asleep && g_sleep_timeout_sec > 0) {
        long idle_sec = now_ts.tv_sec - g_last_activity_sec;
        long dim_threshold_sec = (g_sleep_timeout_sec > 20) ? (g_sleep_timeout_sec - 15) : (g_sleep_timeout_sec / 2);
        if (idle_sec >= g_sleep_timeout_sec) {
            if (g_qs_open) {
                lexios_toggle_quick_settings();
            }
            lexios_set_screen_sleep(true);
        } else if (idle_sec >= dim_threshold_sec && !g_screen_dimmed) {
            lexios_set_screen_dim(true);
        }
    }

    long elapsed_ms = (now_ts.tv_sec - last_ts.tv_sec) * 1000L +
                      (now_ts.tv_nsec - last_ts.tv_nsec) / 1000000L;
    if (elapsed_ms > 0) {
        lv_tick_inc((uint32_t)elapsed_ms);
        last_ts = now_ts;
    }
    lv_task_handler();
    if (g_screen_asleep) {
        usleep(80000); // 80ms low-power poll interval during Pocket Standby
    } else {
        usleep(g_os_thermal_level == 1 ? 50000 : 10000);
    }
    }
	
    return 0;
}




















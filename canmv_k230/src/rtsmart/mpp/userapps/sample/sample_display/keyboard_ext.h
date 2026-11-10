#ifndef KEYBOARD_EXT_H
#define KEYBOARD_EXT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool usb_present;
    int charge_state; /* 0=Not charging, 1=Pre-charge, 2=Fast charging, 3=Charge done */
    int soc_percent;  /* 0..100 (IR-compensated Vapcell G68 13,600mAh 1S2P) */
    int raw_gauge_soc;/* 0..100 raw BQ27220 0x2C */
    int vbat_mv;      /* e.g. 3980 */
    int vocv_mv;      /* IR-compensated open-circuit voltage mV */
    int ibat_ma;      /* Signed current mA (+charging, -discharging) */
    int vbus_mv;      /* e.g. 5100 */
    int full_cap_mah; /* Configured pack capacity (13600 mAh = 2x 6800 mAh) */
    int rem_cap_mah;  /* Estimated remaining mAh (0..13600) */
} k230_pmic_status_t;

void tca8418_init();
bool tca8418_is_connected();
bool tca8418_poll_presence(void);
uint32_t tca8418_get_key();
void control_keyboard_backlight(int freq,int duty);
void keyboard_set_backlight_level(int duty);
int keyboard_get_backlight_level(void);
void keyboard_set_backlight_sleep(bool sleep_off);
void control_keyboard_indicator_led_caps_lock(int is_open);

bool k230_pmic_poll_status(k230_pmic_status_t *out_status);
bool k230_pmic_power_off_ship_mode(void);

#ifdef __cplusplus
}
#endif

#endif

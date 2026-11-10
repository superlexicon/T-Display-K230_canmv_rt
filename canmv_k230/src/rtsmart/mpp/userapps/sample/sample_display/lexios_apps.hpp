#pragma once

#include "esp_brookesia.hpp"
#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LexiOS System & Thermal state hooks shared with sample_display.cpp & lv_port_indev.c */
typedef enum {
    LEXIOS_FAN_AUTO = 0,
    LEXIOS_FAN_FORCE_ON = 1,
    LEXIOS_FAN_FORCE_OFF = 2
} lexios_fan_mode_t;

double lexios_get_die_temp_c(void);
int lexios_get_cpu_dvfs_stage(void);
int lexios_get_cpu_freq_mhz(void);
int lexios_get_thermal_level(void);
bool lexios_get_fan_active(void);
lexios_fan_mode_t lexios_get_fan_mode(void);
void lexios_set_fan_mode(lexios_fan_mode_t mode);

/* Screen-Off Pocket Standby ("Pocket Mode") API */
bool lexios_is_screen_asleep(void);
void lexios_set_screen_sleep(bool sleep);
int lexios_get_sleep_timeout_sec(void);
void lexios_set_sleep_timeout_sec(int sec);
void lexios_notify_user_activity(void);
uint64_t lexios_get_uptime_sec(void);

/* nRF9151 Satellite NTN / GNSS / LTE-M Power & Pin-Mux Arbitration API */
bool lexios_get_nrf9151_power(void);
void lexios_set_nrf9151_power(bool enable);

/* Wi-Fi (802.11b/g/n) & Bluetooth 5.0 LE (GPIO45 + UART1) Power API */
bool lexios_get_wifi_ble_power(void);
void lexios_set_wifi_ble_power(bool enable);

#ifdef __cplusplus
}

/* 1. LexiSystem: Thermal, 6-Stage CPU1 DVFS, Fan, RF Rails & Pocket Standby Control Center */
class LexiSystemApp : public ESP_Brookesia_PhoneApp {
public:
    LexiSystemApp(bool use_status_bar = true, bool use_navigation_bar = true);
    ~LexiSystemApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_timer_t *_refresh_timer = nullptr;
};

/* 2. LexiChat: Sovereign On-Chain FLRC Mesh Messenger (JSON-RPC 127.0.0.1:8545) */
class LexiChatApp : public ESP_Brookesia_PhoneApp {
public:
    LexiChatApp(bool use_status_bar = true, bool use_navigation_bar = true);
    ~LexiChatApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_timer_t *_poll_timer = nullptr;
};

/* 3. LexiSat: nRF9151 Satellite NTN (3GPP Rel-17), GNSS/GPS & Physical QWERTY Keyboard Hub */
class LexiSatApp : public ESP_Brookesia_PhoneApp {
public:
    LexiSatApp(bool use_status_bar = true, bool use_navigation_bar = true);
    ~LexiSatApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_timer_t *_sat_timer = nullptr;
};

/* 4. LexiRF: Multi-Band Sub-GHz/2.4GHz Spectrum Analyzer & Wi-Fi/BLE/Cell Security Auditor */
class LexiRFApp : public ESP_Brookesia_PhoneApp {
public:
    LexiRFApp(bool use_status_bar = true, bool use_navigation_bar = true);
    ~LexiRFApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_timer_t *_rf_timer = nullptr;
};

/* 5. LexiPTT: Push-To-Talk Walkie-Talkie (G.711A Voice Codec + LR2021 FLRC & UDP Multicast) */
class LexiPTTApp : public ESP_Brookesia_PhoneApp {
public:
    LexiPTTApp(bool use_status_bar = true, bool use_navigation_bar = true);
    ~LexiPTTApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_timer_t *_ptt_timer = nullptr;
};

/* 6. LexiApps: MMU Process Supervisor & .wasm / .elf App Sandbox Runner */
class LexiAppsApp : public ESP_Brookesia_PhoneApp {
public:
    LexiAppsApp(bool use_status_bar = true, bool use_navigation_bar = true);
    ~LexiAppsApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

private:
    lv_timer_t *_proc_timer = nullptr;
};
#endif

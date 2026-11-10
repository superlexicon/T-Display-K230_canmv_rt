/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include "ui/ui.h"
#include "Setting.hpp"
#include "../../../../../network/network_rt_opt.h"
#include "../../../../../lexios_apps.hpp"
#include "esp_brookesia_versions.h"

#define ENABLE_DEBUG_LOG                (0)

#define SCREEN_BRIGHTNESS_MIN           (20)
#define SCREEN_BRIGHTNESS_MAX           (100)
#define SPEAKER_VOLUME_MIN              (0)
#define SPEAKER_VOLUME_MAX              (100)

#define NVS_STORAGE_NAMESPACE           "storage"
#define NVS_KEY_WIFI_ENABLE             "wifi_en"
#define NVS_KEY_BLE_ENABLE              "ble_en"
#define NVS_KEY_AUDIO_VOLUME            "volume"
#define NVS_KEY_DISPLAY_BRIGHTNESS      "brightness"

#define UI_MAIN_ITEM_LEFT_OFFSET        (20)
#define UI_WIFI_LIST_UP_OFFSET          (12)
#define UI_WIFI_LIST_UP_PAD             (8)
#define UI_WIFI_LIST_DOWN_PAD           (12)
#define UI_WIFI_LIST_H_PERCENT          (75)
#define UI_WIFI_LIST_ITEM_H             (68)
#define UI_WIFI_LIST_ITEM_FONT          (&lv_font_montserrat_24)
#define UI_WIFI_ICON_LOCK_RIGHT_OFFSET       (-10)
#define UI_WIFI_ICON_SIGNAL_RIGHT_OFFSET     (-48)
#define UI_WIFI_ICON_CONNECT_RIGHT_OFFSET    (-88)

using namespace std;
#define SCAN_LIST_SIZE      20

typedef struct {
    char ssid[64];
    int rssi;
    int channel;
    bool secured;
    bool saved;
    bool connected;
} ui_wifi_ap_t;

static char st_wifi_ssid[64] = "";
static char st_wifi_password[64] = "";

static char mac_str[24] = "00:00:00:00:00:00";

static lv_obj_t *panel_wifi_btn[SCAN_LIST_SIZE];
static lv_obj_t *label_wifi_ssid[SCAN_LIST_SIZE];
static lv_obj_t *label_wifi_sub[SCAN_LIST_SIZE];
static lv_obj_t *img_img_wifi_lock[SCAN_LIST_SIZE];
static lv_obj_t *wifi_image[SCAN_LIST_SIZE];
static lv_obj_t *wifi_connect[SCAN_LIST_SIZE];

static ui_wifi_ap_t s_scan_aps[SCAN_LIST_SIZE];
static int s_scan_ap_count = 0;
static int s_selected_ap_idx = -1;

static lv_obj_t *s_label_wifi_header_sub = nullptr;
static lv_obj_t *s_btn_wifi_rescan = nullptr;
static lv_obj_t *s_label_wifi_rescan = nullptr;

static lv_obj_t *s_btn_verify_back = nullptr;
static lv_obj_t *s_btn_verify_connect = nullptr;
static lv_obj_t *s_btn_verify_eye = nullptr;
static lv_obj_t *s_label_verify_eye = nullptr;
static lv_obj_t *s_btn_verify_forget = nullptr;
static lv_obj_t *s_label_verify_meta = nullptr;
static lv_obj_t *s_label_connect_overlay = nullptr;

/* Main screen extra item & status badges */
static lv_obj_t *s_item_main_power = nullptr;
static lv_obj_t *s_label_main_wifi_sub = nullptr;
static lv_obj_t *s_label_main_light_sub = nullptr;
static lv_obj_t *s_label_main_power_sub = nullptr;

/* Display & Brightness screen widgets */
static lv_obj_t *s_label_light_pct = nullptr;
static lv_obj_t *s_btn_light_rot[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *s_btn_light_slp[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *s_btn_light_kb[4]  = {nullptr, nullptr, nullptr, nullptr};

/* Battery & Power screen widgets */
static lv_obj_t *s_screen_power = nullptr;
static lv_obj_t *s_panel_power_scroll = nullptr;
static lv_obj_t *s_label_pwr_bat_headline = nullptr;
static lv_obj_t *s_label_pwr_bat_detail = nullptr;
static lv_obj_t *s_label_pwr_uvlo_status = nullptr;
static lv_obj_t *s_switch_pwr_saver = nullptr;
static lv_obj_t *s_btn_pwr_slp[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *s_label_pwr_thermal_info = nullptr;
static lv_obj_t *s_btn_pwr_fan[3] = {nullptr, nullptr, nullptr};
static lv_obj_t *s_switch_pwr_wifi_rail = nullptr;
static lv_obj_t *s_switch_pwr_sat_rail = nullptr;
static lv_obj_t *s_btn_pwr_reboot = nullptr;
static lv_obj_t *s_label_pwr_reboot = nullptr;
static lv_obj_t *s_btn_pwr_poweroff = nullptr;
static lv_obj_t *s_label_pwr_poweroff = nullptr;
static bool s_confirm_reboot = false;
static bool s_confirm_poweroff = false;
static lv_coord_t s_last_known_scr_w = 0;

static lv_timer_t *s_wifi_poll_timer = nullptr;
static bool s_suppress_switch_cb = false;
static unsigned int s_last_rendered_scan_seq = 0;
static bool s_force_list_refresh = true;
static uint32_t s_last_auto_scan_tick = 0;

static bool s_connect_in_progress = false;
static char s_connect_target_ssid[64] = "";
static uint32_t s_connect_start_tick = 0;
static uint32_t s_connect_result_tick = 0;
static int s_connect_result_state = 0; // 0: running, 1: success, -1: fail

static bool s_live_wifi_enabled = true;
static bool s_live_wifi_connected = false;
static char s_live_wifi_state[24] = "DISCONNECTED";
static char s_live_wifi_ssid[64] = "";
static char s_live_wifi_ip[24] = "0.0.0.0";
static int s_live_wifi_rssi = -99;
static unsigned int s_live_scan_seq = 0;

static int brightness = 100;

LV_IMG_DECLARE(img_wifisignal_absent);
LV_IMG_DECLARE(img_wifisignal_wake);
LV_IMG_DECLARE(img_wifisignal_moderate);
LV_IMG_DECLARE(img_wifisignal_good);
LV_IMG_DECLARE(img_wifi_lock);
LV_IMG_DECLARE(img_wifi_connect_success);
LV_IMG_DECLARE(img_wifi_connect_fail);
LV_IMG_DECLARE(img_app_setting);

static void detach_obj_from_group(lv_obj_t *obj)
{
    if (obj && lv_group_get_default()) {
        lv_group_remove_obj(obj);
    }
}

static void send_wifi_daemon_cmd(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') return;
    FILE *fp = fopen("/tmp/wifi_cmd.txt.tmp", "w");
    if (fp) {
        fprintf(fp, "%s\n", cmd);
        fflush(fp);
        fclose(fp);
        unlink("/tmp/wifi_cmd.txt");
        rename("/tmp/wifi_cmd.txt.tmp", "/tmp/wifi_cmd.txt");
    }
}

static bool lookup_saved_wifi_pass(const char *ssid, char *out_pass, size_t pass_sz)
{
    if (out_pass && pass_sz > 0) out_pass[0] = '\0';
    if (!ssid || ssid[0] == '\0') return false;
    FILE *fp = fopen("/sdcard/wifi.conf", "r");
    if (!fp) return false;
    char line[160];
    char cur_ssid[64] = "";
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "SSID=", 5) == 0) {
            snprintf(cur_ssid, sizeof(cur_ssid), "%s", line + 5);
        } else if (strncmp(line, "PASS=", 5) == 0) {
            if (strcmp(cur_ssid, ssid) == 0) {
                if (out_pass && pass_sz > 0) {
                    snprintf(out_pass, pass_sz, "%s", line + 5);
                }
                found = true;
            }
            cur_ssid[0] = '\0';
        }
    }
    fclose(fp);
    return found;
}

static void read_live_wifi_status(void)
{
    FILE *fp = fopen("/tmp/wifi_status.txt", "r");
    if (!fp) {
        FILE *cf = fopen("/sdcard/wifi.conf", "r");
        if (cf) {
            char line[128];
            while (fgets(line, sizeof(line), cf)) {
                if (strncmp(line, "ENABLED=", 8) == 0) {
                    s_live_wifi_enabled = (atoi(line + 8) != 0);
                }
            }
            fclose(cf);
        }
        return;
    }

    char line[160];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        line_no++;
        if (line_no == 1) {
            if (strncmp(line, "CONNECTED ", 10) == 0) {
                s_live_wifi_connected = true;
                char ip[24] = {0}, ssid[64] = {0}, mac[24] = {0};
                if (sscanf(line + 10, "%23s %63s %23s", ip, ssid, mac) >= 1) {
                    snprintf(s_live_wifi_ip, sizeof(s_live_wifi_ip), "%s", ip);
                    if (ssid[0] != '\0') snprintf(s_live_wifi_ssid, sizeof(s_live_wifi_ssid), "%s", ssid);
                    if (mac[0] != '\0') snprintf(mac_str, sizeof(mac_str), "%s", mac);
                }
            } else if (strncmp(line, "DISABLED", 8) == 0) {
                s_live_wifi_connected = false;
                s_live_wifi_enabled = false;
            } else {
                s_live_wifi_connected = false;
            }
            continue;
        }
        if (strncmp(line, "ENABLED=", 8) == 0) {
            s_live_wifi_enabled = (atoi(line + 8) != 0);
        } else if (strncmp(line, "STATE=", 6) == 0) {
            snprintf(s_live_wifi_state, sizeof(s_live_wifi_state), "%s", line + 6);
        } else if (strncmp(line, "RSSI=", 5) == 0) {
            s_live_wifi_rssi = atoi(line + 5);
        } else if (strncmp(line, "IP=", 3) == 0) {
            snprintf(s_live_wifi_ip, sizeof(s_live_wifi_ip), "%s", line + 3);
        } else if (strncmp(line, "MAC=", 4) == 0 && line[4] != '\0') {
            snprintf(mac_str, sizeof(mac_str), "%s", line + 4);
        } else if (strncmp(line, "SSID=", 5) == 0) {
            snprintf(s_live_wifi_ssid, sizeof(s_live_wifi_ssid), "%s", line + 5);
        } else if (strncmp(line, "SCAN_SEQ=", 9) == 0) {
            s_live_scan_seq = (unsigned int)strtoul(line + 9, nullptr, 10);
        }
    }
    fclose(fp);

    if (s_live_wifi_connected && s_live_wifi_ssid[0] != '\0') {
        snprintf(st_wifi_ssid, sizeof(st_wifi_ssid), "%s", s_live_wifi_ssid);
    } else if (!s_live_wifi_connected) {
        st_wifi_ssid[0] = '\0';
    }
}

static void load_scan_results_from_file(void)
{
    s_scan_ap_count = 0;
    FILE *fp = fopen("/tmp/wifi_scan.txt", "r");
    if (!fp) return;

    char line[192];
    while (fgets(line, sizeof(line), fp) && s_scan_ap_count < SCAN_LIST_SIZE) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        int rssi = -80, secured = 1, channel = 1, saved = 0, connected = 0;
        char *p = line;
        char *tok_rssi = strsep(&p, "\t");
        char *tok_sec  = strsep(&p, "\t");
        char *tok_chan = strsep(&p, "\t");
        char *tok_sav  = strsep(&p, "\t");
        char *tok_con  = strsep(&p, "\t");
        char *tok_ssid = p;

        if (!tok_rssi || !tok_sec || !tok_chan || !tok_sav || !tok_con || !tok_ssid || tok_ssid[0] == '\0') {
            continue;
        }
        rssi = atoi(tok_rssi);
        secured = atoi(tok_sec);
        channel = atoi(tok_chan);
        saved = atoi(tok_sav);
        connected = atoi(tok_con);

        ui_wifi_ap_t *ap = &s_scan_aps[s_scan_ap_count++];
        memset(ap, 0, sizeof(*ap));
        snprintf(ap->ssid, sizeof(ap->ssid), "%s", tok_ssid);
        ap->rssi = rssi;
        ap->channel = channel;
        ap->secured = (secured != 0);
        ap->saved = (saved != 0);
        ap->connected = (connected != 0) || (s_live_wifi_connected && strcmp(tok_ssid, s_live_wifi_ssid) == 0);
    }
    fclose(fp);
}

static void adapt_main_screen_geometry(void)
{
    if (!ui_PanelSettingMainContainer) return;
    lv_coord_t scr_w = lv_disp_get_hor_res(nullptr);
    lv_coord_t scr_h = lv_disp_get_ver_res(nullptr);
    bool is_landscape = (scr_w > scr_h);

    lv_coord_t top_y = is_landscape ? 64 : 76;
    lv_coord_t cont_h = scr_h - top_y - 68;
    if (cont_h < 220) cont_h = 220;

    lv_obj_set_width(ui_PanelSettingMainContainer, lv_pct(94));
    lv_obj_set_height(ui_PanelSettingMainContainer, cont_h);
    lv_obj_align(ui_PanelSettingMainContainer, LV_ALIGN_TOP_MID, 0, top_y);
}

static void adapt_wifi_screen_geometry(void)
{
    lv_coord_t scr_w = lv_disp_get_hor_res(nullptr);
    lv_coord_t scr_h = lv_disp_get_ver_res(nullptr);
    bool is_landscape = (scr_w > scr_h);

    if (ui_PanelScreenSettingWiFiSwitch) {
        lv_obj_set_width(ui_PanelScreenSettingWiFiSwitch, lv_pct(94));
        lv_obj_set_height(ui_PanelScreenSettingWiFiSwitch, is_landscape ? 74 : 86);
        lv_obj_align(ui_PanelScreenSettingWiFiSwitch, LV_ALIGN_TOP_MID, 0, 56);
    }
    if (ui_ImagePanelScreenSettingWiFiSwitch) {
        lv_obj_align(ui_ImagePanelScreenSettingWiFiSwitch, LV_ALIGN_LEFT_MID, 6, 0);
    }
    if (ui_LabelPanelScreenSettingWiFiSwitch) {
        lv_obj_set_style_text_font(ui_LabelPanelScreenSettingWiFiSwitch, &lv_font_montserrat_26, 0);
        lv_obj_align(ui_LabelPanelScreenSettingWiFiSwitch, LV_ALIGN_LEFT_MID, 58, -12);
    }
    if (s_label_wifi_header_sub) {
        lv_obj_align(s_label_wifi_header_sub, LV_ALIGN_LEFT_MID, 58, 16);
    }
    if (ui_SwitchPanelScreenSettingWiFiSwitch) {
        lv_obj_set_size(ui_SwitchPanelScreenSettingWiFiSwitch, 76, 44);
        lv_obj_align(ui_SwitchPanelScreenSettingWiFiSwitch, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    if (s_btn_wifi_rescan) {
        lv_obj_set_size(s_btn_wifi_rescan, 86, 42);
        lv_obj_align(s_btn_wifi_rescan, LV_ALIGN_RIGHT_MID, -96, 0);
    }
    if (ui_PanelScreenSettingWiFiList) {
        lv_coord_t top_y = is_landscape ? 138 : 152;
        lv_coord_t list_h = scr_h - top_y - 56;
        if (list_h < 180) list_h = 180;
        lv_obj_set_width(ui_PanelScreenSettingWiFiList, lv_pct(94));
        lv_obj_set_height(ui_PanelScreenSettingWiFiList, list_h);
        lv_obj_align(ui_PanelScreenSettingWiFiList, LV_ALIGN_TOP_MID, 0, top_y);
    }
}

static void adapt_verification_screen_geometry(void)
{
    if (!ui_ScreenSettingVerification) return;
    lv_coord_t scr_w = lv_disp_get_hor_res(nullptr);
    lv_coord_t scr_h = lv_disp_get_ver_res(nullptr);
    bool is_landscape = (scr_w > scr_h);

    if (s_btn_verify_back) {
        lv_obj_set_size(s_btn_verify_back, 100, 42);
        lv_obj_align(s_btn_verify_back, LV_ALIGN_TOP_LEFT, 16, 56);
    }
    if (ui_LabelScreenSettingVerification) {
        lv_obj_add_flag(ui_LabelScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_LabelScreenSettingVerificationSSID) {
        lv_obj_set_style_text_font(ui_LabelScreenSettingVerificationSSID, &lv_font_montserrat_24, 0);
        lv_obj_align(ui_LabelScreenSettingVerificationSSID, LV_ALIGN_TOP_MID, 0, is_landscape ? 58 : 112);
    }
    if (s_label_verify_meta) {
        lv_obj_align(s_label_verify_meta, LV_ALIGN_TOP_MID, 0, is_landscape ? 90 : 146);
    }
    if (ui_TextAreaScreenSettingVerificationPassword) {
        lv_coord_t ta_w = is_landscape ? 460 : (scr_w - 130);
        lv_obj_set_size(ui_TextAreaScreenSettingVerificationPassword, ta_w, 56);
        lv_obj_align(ui_TextAreaScreenSettingVerificationPassword, LV_ALIGN_TOP_MID, -44, is_landscape ? 124 : 186);
    }
    if (s_btn_verify_eye) {
        lv_obj_set_size(s_btn_verify_eye, 80, 54);
        lv_obj_align_to(s_btn_verify_eye, ui_TextAreaScreenSettingVerificationPassword, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }
    if (s_btn_verify_connect) {
        lv_obj_set_size(s_btn_verify_connect, 170, 48);
        lv_obj_align(s_btn_verify_connect, LV_ALIGN_TOP_MID,
                     (s_btn_verify_forget && !lv_obj_has_flag(s_btn_verify_forget, LV_OBJ_FLAG_HIDDEN)) ? -95 : 0,
                     is_landscape ? 190 : 258);
    }
    if (s_btn_verify_forget) {
        lv_obj_set_size(s_btn_verify_forget, 170, 48);
        lv_obj_align(s_btn_verify_forget, LV_ALIGN_TOP_MID, 95, is_landscape ? 190 : 258);
    }
    if (ui_KeyboardScreenSettingVerification) {
        lv_obj_set_width(ui_KeyboardScreenSettingVerification, lv_pct(98));
        lv_obj_set_height(ui_KeyboardScreenSettingVerification, is_landscape ? 235 : 360);
        lv_obj_align(ui_KeyboardScreenSettingVerification, LV_ALIGN_BOTTOM_MID, 0, -48);
    }
}

static void adapt_light_screen_geometry(void)
{
    if (!ui_ScreenSettingLight) return;
    lv_coord_t scr_w = lv_disp_get_hor_res(nullptr);
    lv_coord_t scr_h = lv_disp_get_ver_res(nullptr);
    bool is_landscape = (scr_w > scr_h);

    if (ui_PanelScreenSettingLightSwitch) {
        lv_obj_set_width(ui_PanelScreenSettingLightSwitch, lv_pct(94));
        lv_obj_set_height(ui_PanelScreenSettingLightSwitch, is_landscape ? 76 : 86);
        lv_obj_align(ui_PanelScreenSettingLightSwitch, LV_ALIGN_TOP_MID, 0, 58);
    }
    if (ui_ImagePanelScreenSettingLightSwitch) {
        lv_obj_align(ui_ImagePanelScreenSettingLightSwitch, LV_ALIGN_LEFT_MID, 6, 0);
    }
    if (ui_LabelPanelScreenSettingLightSwitch) {
        lv_obj_set_style_text_font(ui_LabelPanelScreenSettingLightSwitch, &lv_font_montserrat_24, 0);
        lv_obj_align(ui_LabelPanelScreenSettingLightSwitch, LV_ALIGN_LEFT_MID, 54, -12);
    }
    if (s_label_light_pct) {
        lv_obj_align(s_label_light_pct, LV_ALIGN_LEFT_MID, 54, 16);
    }
    if (ui_SliderPanelScreenSettingLightSwitch1) {
        lv_coord_t slider_w = is_landscape ? 580 : 260;
        lv_obj_set_width(ui_SliderPanelScreenSettingLightSwitch1, slider_w);
        lv_obj_set_height(ui_SliderPanelScreenSettingLightSwitch1, 14);
        lv_obj_align(ui_SliderPanelScreenSettingLightSwitch1, LV_ALIGN_RIGHT_MID, -18, 0);
    }
    if (ui_PanelScreenSettingLightList) {
        lv_coord_t top_y = is_landscape ? 142 : 154;
        lv_coord_t list_h = scr_h - top_y - 64;
        if (list_h < 180) list_h = 180;
        lv_obj_set_width(ui_PanelScreenSettingLightList, lv_pct(94));
        lv_obj_set_height(ui_PanelScreenSettingLightList, list_h);
        lv_obj_align(ui_PanelScreenSettingLightList, LV_ALIGN_TOP_MID, 0, top_y);
    }
}

static void adapt_power_screen_geometry(void)
{
    if (!s_screen_power || !s_panel_power_scroll) return;
    lv_coord_t scr_w = lv_disp_get_hor_res(nullptr);
    lv_coord_t scr_h = lv_disp_get_ver_res(nullptr);
    bool is_landscape = (scr_w > scr_h);

    lv_coord_t top_y = is_landscape ? 58 : 62;
    lv_coord_t list_h = scr_h - top_y - 64;
    if (list_h < 200) list_h = 200;
    lv_obj_set_width(s_panel_power_scroll, lv_pct(94));
    lv_obj_set_height(s_panel_power_scroll, list_h);
    lv_obj_align(s_panel_power_scroll, LV_ALIGN_TOP_MID, 0, top_y);
}

static void refresh_display_and_power_ui(void)
{
    int cur_brt = lexios_get_user_brightness_pct();
    int rot_mode = lexios_get_rot_mode();
    int rot_flip = lexios_get_rot_flip();
    int slp_sec  = lexios_get_sleep_timeout_sec();
    int kb_duty  = lexios_get_kb_backlight();

    int soc = 85, vbat_mv = 3950, ibat_ma = 0, rem_mah = 11560, full_mah = 13600, chg_state = 0;
    bool usb_present = false, low_bat_throttled = false;
    lexios_get_battery_telemetry(&soc, &vbat_mv, &ibat_ma, &rem_mah, &full_mah,
                                 &chg_state, &usb_present, &low_bat_throttled);

    const char *chg_str = "On Battery";
    if (chg_state == 1) chg_str = "Pre-Charging";
    else if (chg_state == 2) chg_str = "USB Charging";
    else if (chg_state == 3) chg_str = "Charge Full";
    else if (usb_present) chg_str = "USB Powered";

    const char *rot_str = (rot_mode == 0) ? "Auto (KB)" : (rot_mode == 2 ? "Landscape" : "Portrait");

    /* Update Main Screen subtitles */
    if (s_label_main_wifi_sub) {
        if (!s_live_wifi_enabled) {
            lv_label_set_text(s_label_main_wifi_sub, "Off");
        } else if (s_live_wifi_connected && s_live_wifi_ssid[0] != '\0') {
            lv_label_set_text_fmt(s_label_main_wifi_sub, "%s (%s)", s_live_wifi_ssid, s_live_wifi_ip);
        } else {
            lv_label_set_text(s_label_main_wifi_sub, "Disconnected");
        }
    }
    if (s_label_main_light_sub) {
        lv_label_set_text_fmt(s_label_main_light_sub, "%d%% • %s", cur_brt, rot_str);
    }
    if (s_label_main_power_sub) {
        lv_label_set_text_fmt(s_label_main_power_sub, "%d%% • %s • %.1f°C",
                              soc, chg_str, lexios_get_die_temp_c());
    }

    /* Update Display & Brightness Screen */
    if (s_label_light_pct) {
        lv_label_set_text_fmt(s_label_light_pct, "%d%%", cur_brt);
    }
    if (ui_SliderPanelScreenSettingLightSwitch1 &&
        lv_slider_get_value(ui_SliderPanelScreenSettingLightSwitch1) != cur_brt) {
        lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, cur_brt, LV_ANIM_OFF);
    }

    for (int i = 0; i < 3; i++) {
        if (!s_btn_light_rot[i]) continue;
        bool active = (rot_mode == i);
        lv_obj_set_style_bg_color(s_btn_light_rot[i], lv_color_hex(active ? 0x2563EB : 0xE2E8F0), 0);
        lv_obj_t *l = lv_obj_get_child(s_btn_light_rot[i], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : 0x1E293B), 0);
    }
    if (s_btn_light_rot[3]) {
        bool active = (rot_flip != 0);
        lv_obj_set_style_bg_color(s_btn_light_rot[3], lv_color_hex(active ? 0xD97706 : 0xE2E8F0), 0);
        lv_obj_t *l = lv_obj_get_child(s_btn_light_rot[3], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : 0x1E293B), 0);
    }

    static const int kSlpSecs[4] = {30, 60, 300, 0};
    for (int i = 0; i < 4; i++) {
        bool active = (slp_sec == kSlpSecs[i]);
        if (s_btn_light_slp[i]) {
            lv_obj_set_style_bg_color(s_btn_light_slp[i], lv_color_hex(active ? 0x7C3AED : 0xE2E8F0), 0);
            lv_obj_t *l = lv_obj_get_child(s_btn_light_slp[i], 0);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : 0x1E293B), 0);
        }
        if (s_btn_pwr_slp[i]) {
            lv_obj_set_style_bg_color(s_btn_pwr_slp[i], lv_color_hex(active ? 0x7C3AED : 0xE2E8F0), 0);
            lv_obj_t *l = lv_obj_get_child(s_btn_pwr_slp[i], 0);
            if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : 0x1E293B), 0);
        }
    }

    static const int kKbDuties[4] = {0, 5, 25, 60};
    for (int i = 0; i < 4; i++) {
        if (!s_btn_light_kb[i]) continue;
        bool active = (kb_duty == kKbDuties[i]);
        lv_obj_set_style_bg_color(s_btn_light_kb[i], lv_color_hex(active ? 0x059669 : 0xE2E8F0), 0);
        lv_obj_t *l = lv_obj_get_child(s_btn_light_kb[i], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : 0x1E293B), 0);
    }

    /* Update Battery & Power Screen */
    if (s_label_pwr_bat_headline) {
        lv_label_set_text_fmt(s_label_pwr_bat_headline, "Battery: %d%% (%s)", soc, chg_str);
    }
    if (s_label_pwr_bat_detail) {
        char dbuf[128];
        snprintf(dbuf, sizeof(dbuf), "Capacity: %d / %d mAh  |  %.2f V (%+d mA)",
                 rem_mah, full_mah, vbat_mv / 1000.0, ibat_ma);
        lv_label_set_text(s_label_pwr_bat_detail, dbuf);
    }
    if (s_label_pwr_uvlo_status) {
        if (low_bat_throttled) {
            lv_obj_set_style_text_color(s_label_pwr_uvlo_status, lv_color_hex(0xDC2626), 0);
            lv_label_set_text(s_label_pwr_uvlo_status,
                              "LOW BATTERY SAVER ACTIVE (<=10% / <=3.42V • Auto-Off at <=4%)");
        } else {
            lv_obj_set_style_text_color(s_label_pwr_uvlo_status, lv_color_hex(0x059669), 0);
            lv_label_set_text(s_label_pwr_uvlo_status,
                              "UVLO Protection Active: <=10% Auto-Saver • <=4% Graceful Shutdown");
        }
    }

    s_suppress_switch_cb = true;
    if (s_switch_pwr_saver) {
        if (lexios_get_battery_saver()) lv_obj_add_state(s_switch_pwr_saver, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_switch_pwr_saver, LV_STATE_CHECKED);
    }
    if (s_switch_pwr_wifi_rail) {
        if (lexios_get_wifi_ble_power()) lv_obj_add_state(s_switch_pwr_wifi_rail, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_switch_pwr_wifi_rail, LV_STATE_CHECKED);
    }
    if (s_switch_pwr_sat_rail) {
        if (lexios_get_nrf9151_power()) lv_obj_add_state(s_switch_pwr_sat_rail, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_switch_pwr_sat_rail, LV_STATE_CHECKED);
    }
    s_suppress_switch_cb = false;

    if (s_label_pwr_thermal_info) {
        int dvfs_st = lexios_get_cpu_dvfs_stage();
        int freq_mhz = lexios_get_cpu_freq_mhz();
        char tbuf[128];
        snprintf(tbuf, sizeof(tbuf), "Die: %.1f °C  |  CPU1: %d MHz (%s)  |  Fan: %s",
                 lexios_get_die_temp_c(),
                 freq_mhz,
                 (dvfs_st == 0) ? "AUTO" : (lexios_get_battery_saver() ? "SAVER" : "THROTTLED"),
                 lexios_get_fan_active() ? "RUNNING" : "IDLE");
        lv_label_set_text(s_label_pwr_thermal_info, tbuf);
    }

    int fan_mode = (int)lexios_get_fan_mode();
    for (int i = 0; i < 3; i++) {
        if (!s_btn_pwr_fan[i]) continue;
        bool active = (fan_mode == i);
        lv_obj_set_style_bg_color(s_btn_pwr_fan[i], lv_color_hex(active ? 0x0284C7 : 0xE2E8F0), 0);
        lv_obj_t *l = lv_obj_get_child(s_btn_pwr_fan[i], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(active ? 0xFFFFFF : 0x1E293B), 0);
    }

    if (s_btn_pwr_reboot && s_label_pwr_reboot) {
        lv_obj_set_style_bg_color(s_btn_pwr_reboot, lv_color_hex(s_confirm_reboot ? 0xD97706 : 0x2563EB), 0);
        lv_label_set_text(s_label_pwr_reboot, s_confirm_reboot ? "Confirm Reboot" : "Reboot Device");
    }
    if (s_btn_pwr_poweroff && s_label_pwr_poweroff) {
        lv_obj_set_style_bg_color(s_btn_pwr_poweroff, lv_color_hex(s_confirm_poweroff ? 0x991B1B : 0xDC2626), 0);
        lv_label_set_text(s_label_pwr_poweroff, s_confirm_poweroff ? "Confirm Power Off" : "Power Off (Ship Mode)");
    }
}

AppSettings::AppSettings(bool use_status_bar, bool use_navigation_bar):
    ESP_Brookesia_PhoneApp(
        ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("Settings", &img_app_setting, true),
        ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_setting, use_status_bar, use_navigation_bar)
    ),
    _is_ui_resumed(false),
    _is_ui_del(true),
    _screen_index(UI_MAIN_SETTING_INDEX),
    _screen_list({nullptr})
{
}

AppSettings::~AppSettings()
{
}

static void on_rescan_btn_clicked_cb(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    if (s_label_wifi_header_sub) {
        lv_label_set_text(s_label_wifi_header_sub, "Scanning nearby networks...");
    }
    send_wifi_daemon_cmd("SCAN");
    s_last_auto_scan_tick = lv_tick_get();
}

static void on_verify_back_clicked_cb(lv_event_t *e)
{
    (void)e;
    s_connect_in_progress = false;
    lv_scr_load(ui_ScreenSettingWiFi);
}

static void on_verify_eye_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!ui_TextAreaScreenSettingVerificationPassword) return;
    bool cur = lv_textarea_get_password_mode(ui_TextAreaScreenSettingVerificationPassword);
    lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, !cur);
    if (s_label_verify_eye) {
        lv_label_set_text(s_label_verify_eye, cur ? "Hide" : "Show");
    }
}

static void on_verify_forget_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_selected_ap_idx >= 0 && s_selected_ap_idx < s_scan_ap_count) {
        char cmd[128];
        if (s_scan_aps[s_selected_ap_idx].saved) {
            snprintf(cmd, sizeof(cmd), "FORGET %s", s_scan_aps[s_selected_ap_idx].ssid);
        } else {
            snprintf(cmd, sizeof(cmd), "DISCONNECT");
        }
        send_wifi_daemon_cmd(cmd);
        s_force_list_refresh = true;
    }
    s_connect_in_progress = false;
    lv_scr_load(ui_ScreenSettingWiFi);
}

static void on_rot_btn_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx <= 2) {
        lexios_set_rot_mode(idx);
    } else if (idx == 3) {
        lexios_set_rot_flip(!lexios_get_rot_flip());
    }
    adapt_main_screen_geometry();
    adapt_light_screen_geometry();
    adapt_power_screen_geometry();
    refresh_display_and_power_ui();
}

static void on_slp_btn_clicked_cb(lv_event_t *e)
{
    int sec = (int)(intptr_t)lv_event_get_user_data(e);
    lexios_set_sleep_timeout_sec(sec);
    refresh_display_and_power_ui();
}

static void on_kb_btn_clicked_cb(lv_event_t *e)
{
    int duty = (int)(intptr_t)lv_event_get_user_data(e);
    lexios_set_kb_backlight(duty);
    refresh_display_and_power_ui();
}

static void on_sleep_now_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    lexios_set_screen_sleep(true);
}

static void on_fan_btn_clicked_cb(lv_event_t *e)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    lexios_set_fan_mode((lexios_fan_mode_t)mode);
    refresh_display_and_power_ui();
}

static void on_bat_saver_switch_cb(lv_event_t *e)
{
    if (s_suppress_switch_cb) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool en = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
    lexios_set_battery_saver(en);
    refresh_display_and_power_ui();
}

static void on_wifi_rail_switch_cb(lv_event_t *e)
{
    if (s_suppress_switch_cb) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool en = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
    lexios_set_wifi_ble_power(en);
    s_live_wifi_enabled = en;
    if (!en) s_live_wifi_connected = false;
    refresh_display_and_power_ui();
}

static void on_sat_rail_switch_cb(lv_event_t *e)
{
    if (s_suppress_switch_cb) return;
    lv_obj_t *sw = lv_event_get_target(e);
    bool en = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
    lexios_set_nrf9151_power(en);
    refresh_display_and_power_ui();
}

static void on_reboot_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    s_confirm_poweroff = false;
    if (!s_confirm_reboot) {
        s_confirm_reboot = true;
        refresh_display_and_power_ui();
        return;
    }
    s_confirm_reboot = false;
    lexios_request_reboot();
}

static void on_poweroff_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    s_confirm_reboot = false;
    if (!s_confirm_poweroff) {
        s_confirm_poweroff = true;
        refresh_display_and_power_ui();
        return;
    }
    s_confirm_poweroff = false;
    lexios_request_power_off();
}

static void on_main_power_item_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_screen_power) {
        s_confirm_reboot = false;
        s_confirm_poweroff = false;
        adapt_power_screen_geometry();
        refresh_display_and_power_ui();
        lv_scr_load(s_screen_power);
    }
}

static lv_obj_t *create_setting_card(lv_obj_t *parent, const char *title_text, lv_coord_t min_h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, min_h);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (title_text && title_text[0] != '\0') {
        lv_obj_t *title = lv_label_create(card);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x0F172A), 0);
        lv_label_set_text(title, title_text);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return card;
}

bool AppSettings::run(void)
{
    _is_ui_del = false;

    FILE *id_fp = fopen("/sdcard/k230-device.id", "r");
    if (id_fp) {
        char line[128];
        while (fgets(line, sizeof(line), id_fp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (strncmp(line, "MAC=", 4) == 0 && line[4] != '\0') {
                snprintf(mac_str, sizeof(mac_str), "%s", line + 4);
            }
        }
        fclose(id_fp);
    }

    read_live_wifi_status();
    brightness = lexios_get_user_brightness_pct();
    _nvs_param_map[NVS_KEY_WIFI_ENABLE] = s_live_wifi_enabled ? 1 : 0;
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;

    ui_setting_init();
    extraUiInit();
    updateUiByNvsParam();
    refresh_display_and_power_ui();

    return true;
}

bool AppSettings::back(void)
{
    _is_ui_resumed = false;
    s_confirm_reboot = false;
    s_confirm_poweroff = false;

    if (_screen_index == UI_WIFI_CONNECT_INDEX) {
        s_connect_in_progress = false;
        processWifiConnect(WIFI_CONNECT_HIDE);
        lv_scr_load(ui_ScreenSettingWiFi);
    } else if (_screen_index != UI_MAIN_SETTING_INDEX) {
        lv_scr_load(ui_ScreenSettingMain);
    } else {
        notifyCoreClosed();
    }

    return true;
}

bool AppSettings::close(void)
{
    if (s_wifi_poll_timer) {
        lv_timer_del(s_wifi_poll_timer);
        s_wifi_poll_timer = nullptr;
    }
    if (s_screen_power) {
        lv_obj_del(s_screen_power);
        s_screen_power = nullptr;
    }
    s_connect_in_progress = false;
    _is_ui_del = true;
    return true;
}

bool AppSettings::init(void)
{
    ESP_Brookesia_Phone *phone = getPhone();
    ESP_Brookesia_PhoneHome& home = phone->getHome();
    status_bar = home.getStatusBar();
    backstage = home.getRecentsScreen();

    read_live_wifi_status();
    brightness = lexios_get_user_brightness_pct();
    _nvs_param_map[NVS_KEY_WIFI_ENABLE] = s_live_wifi_enabled ? 1 : 0;
    _nvs_param_map[NVS_KEY_BLE_ENABLE] = lexios_get_wifi_ble_power() ? 1 : 0;
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
    loadNvsParam();

    return true;
}

bool AppSettings::pause(void)
{
    _is_ui_resumed = true;
    return true;
}

bool AppSettings::resume(void)
{
    _is_ui_resumed = false;
    read_live_wifi_status();
    brightness = lexios_get_user_brightness_pct();
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
    updateUiByNvsParam();
    adapt_main_screen_geometry();
    adapt_wifi_screen_geometry();
    adapt_verification_screen_geometry();
    adapt_light_screen_geometry();
    adapt_power_screen_geometry();
    refresh_display_and_power_ui();
    if (_screen_index == UI_WIFI_SCAN_INDEX) {
        s_force_list_refresh = true;
    }
    return true;
}

void AppSettings::extraUiInit(void)
{
    /* 1. Main Settings Screen */
    lv_label_set_text(ui_LabelPanelSettingMainContainer3Volume, "Audio");
    lv_label_set_text(ui_LabelPanelSettingMainContainer4Light, "Display & Brightness");
    lv_obj_align_to(ui_LabelPanelSettingMainContainer1WiFi, ui_ImagePanelSettingMainContainer1WiFi, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, -10);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer2Blue, ui_ImagePanelSettingMainContainer2Blue, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer3Volume, ui_ImagePanelSettingMainContainer3Volume, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer4Light, ui_ImagePanelSettingMainContainer4Light, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, -10);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer5About, ui_ImagePanelSettingMainContainer5About, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);

    lv_obj_add_flag(ui_PanelSettingMainContainer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_PanelSettingMainContainer, LV_DIR_VER);
    lv_obj_set_style_pad_row(ui_PanelSettingMainContainer, 8, 0);
    lv_obj_set_style_pad_all(ui_PanelSettingMainContainer, 8, 0);

    s_label_main_wifi_sub = lv_label_create(ui_PanelSettingMainContainerItem1);
    lv_obj_set_style_text_font(s_label_main_wifi_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_main_wifi_sub, lv_color_hex(0x64748B), 0);
    lv_obj_align_to(s_label_main_wifi_sub, ui_ImagePanelSettingMainContainer1WiFi, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 16);
    lv_label_set_text(s_label_main_wifi_sub, "");

    s_label_main_light_sub = lv_label_create(ui_PanelSettingMainContainerItem4);
    lv_obj_set_style_text_font(s_label_main_light_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_main_light_sub, lv_color_hex(0x64748B), 0);
    lv_obj_align_to(s_label_main_light_sub, ui_ImagePanelSettingMainContainer4Light, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 16);
    lv_label_set_text(s_label_main_light_sub, "");

    /* Add "Battery & Power" item to Main Settings menu */
    s_item_main_power = lv_obj_create(ui_PanelSettingMainContainer);
    lv_obj_set_width(s_item_main_power, lv_pct(100));
    lv_obj_set_height(s_item_main_power, 72);
    lv_obj_clear_flag(s_item_main_power, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_item_main_power, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_item_main_power, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(s_item_main_power, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_item_main_power, lv_color_hex(0xCBCBCB), LV_STATE_PRESSED);

    lv_obj_t *pwr_icon = lv_label_create(s_item_main_power);
    lv_obj_set_style_text_font(pwr_icon, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(pwr_icon, lv_color_hex(0x059669), 0);
    lv_label_set_text(pwr_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_align(pwr_icon, LV_ALIGN_CENTER);
    lv_obj_set_x(pwr_icon, lv_pct(-47));

    lv_obj_t *pwr_arrow = lv_img_create(s_item_main_power);
    lv_img_set_src(pwr_arrow, &ui_img_arrow_png);
    lv_obj_set_align(pwr_arrow, LV_ALIGN_CENTER);
    lv_obj_set_x(pwr_arrow, lv_pct(49));

    lv_obj_t *pwr_title = lv_label_create(s_item_main_power);
    lv_obj_set_style_text_font(pwr_title, &lv_font_montserrat_30, 0);
    lv_label_set_text(pwr_title, "Battery & Power");
    lv_obj_align_to(pwr_title, pwr_icon, LV_ALIGN_OUT_RIGHT_MID, UI_MAIN_ITEM_LEFT_OFFSET + 4, -10);

    s_label_main_power_sub = lv_label_create(s_item_main_power);
    lv_obj_set_style_text_font(s_label_main_power_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_main_power_sub, lv_color_hex(0x059669), 0);
    lv_obj_align_to(s_label_main_power_sub, pwr_icon, LV_ALIGN_OUT_RIGHT_MID, UI_MAIN_ITEM_LEFT_OFFSET + 4, 16);
    lv_label_set_text(s_label_main_power_sub, "");

    /* Place "Battery & Power" right above "About Device" */
    lv_obj_move_to_index(s_item_main_power, 4);
    lv_obj_add_event_cb(s_item_main_power, on_main_power_item_clicked_cb, LV_EVENT_CLICKED, this);

    _screen_list[UI_MAIN_SETTING_INDEX] = ui_ScreenSettingMain;
    lv_obj_add_event_cb(ui_ScreenSettingMain, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* 2. WiFi Header & Network List */
    s_label_wifi_header_sub = lv_label_create(ui_PanelScreenSettingWiFiSwitch);
    lv_obj_set_style_text_font(s_label_wifi_header_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_wifi_header_sub, lv_color_hex(0x475569), 0);
    lv_label_set_text(s_label_wifi_header_sub, s_live_wifi_enabled ? "Ready" : "Off");

    s_btn_wifi_rescan = lv_btn_create(ui_PanelScreenSettingWiFiSwitch);
    lv_obj_set_style_radius(s_btn_wifi_rescan, 10, 0);
    lv_obj_set_style_bg_color(s_btn_wifi_rescan, lv_color_hex(0x2563EB), 0);
    s_label_wifi_rescan = lv_label_create(s_btn_wifi_rescan);
    lv_obj_set_style_text_font(s_label_wifi_rescan, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_wifi_rescan, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_label_wifi_rescan, "Scan");
    lv_obj_center(s_label_wifi_rescan);
    lv_obj_add_event_cb(s_btn_wifi_rescan, on_rescan_btn_clicked_cb, LV_EVENT_CLICKED, this);
    detach_obj_from_group(s_btn_wifi_rescan);
    detach_obj_from_group(ui_SwitchPanelScreenSettingWiFiSwitch);

    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingWiFiSwitch, onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);

    lv_obj_set_scroll_dir(ui_PanelScreenSettingWiFiList, LV_DIR_VER);
    lv_obj_set_style_pad_all(ui_PanelScreenSettingWiFiList, 6, 0);
    lv_obj_set_style_pad_row(ui_PanelScreenSettingWiFiList, 6, 0);
    lv_obj_set_style_bg_color(ui_PanelScreenSettingWiFiList, lv_color_hex(0xF8FAFC), 0);

    for (int i = 0; i < SCAN_LIST_SIZE; i++) {
        panel_wifi_btn[i] = lv_obj_create(ui_PanelScreenSettingWiFiList);
        lv_obj_set_size(panel_wifi_btn[i], lv_pct(100), UI_WIFI_LIST_ITEM_H);
        lv_obj_set_style_radius(panel_wifi_btn[i], 10, 0);
        lv_obj_set_style_border_width(panel_wifi_btn[i], 1, 0);
        lv_obj_set_style_border_color(panel_wifi_btn[i], lv_color_hex(0xCBD5E1), 0);
        lv_obj_set_style_bg_color(panel_wifi_btn[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(panel_wifi_btn[i], 255, LV_PART_MAIN);
        lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(panel_wifi_btn[i], LV_OBJ_FLAG_SCROLLABLE);

        label_wifi_ssid[i] = lv_label_create(panel_wifi_btn[i]);
        lv_obj_set_style_text_font(label_wifi_ssid[i], UI_WIFI_LIST_ITEM_FONT, 0);
        lv_obj_set_style_text_color(label_wifi_ssid[i], lv_color_hex(0x0F172A), 0);
        lv_obj_align(label_wifi_ssid[i], LV_ALIGN_TOP_LEFT, 10, 4);

        label_wifi_sub[i] = lv_label_create(panel_wifi_btn[i]);
        lv_obj_set_style_text_font(label_wifi_sub[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label_wifi_sub[i], lv_color_hex(0x64748B), 0);
        lv_obj_align(label_wifi_sub[i], LV_ALIGN_BOTTOM_LEFT, 10, -4);
        lv_label_set_text(label_wifi_sub[i], "");

        img_img_wifi_lock[i] = lv_img_create(panel_wifi_btn[i]);
        lv_obj_align(img_img_wifi_lock[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_LOCK_RIGHT_OFFSET, 0);
        lv_obj_add_flag(img_img_wifi_lock[i], LV_OBJ_FLAG_HIDDEN);

        wifi_image[i] = lv_img_create(panel_wifi_btn[i]);
        lv_obj_align(wifi_image[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_SIGNAL_RIGHT_OFFSET, 0);

        wifi_connect[i] = lv_label_create(panel_wifi_btn[i]);
        lv_obj_set_style_text_font(wifi_connect[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(wifi_connect[i], lv_color_hex(0x059669), 0);
        lv_label_set_text(wifi_connect[i], LV_SYMBOL_OK);
        lv_obj_align(wifi_connect[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_CONNECT_RIGHT_OFFSET, 0);
        lv_obj_add_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(panel_wifi_btn[i], onButtonWifiListClickedEventCallback, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(ui_ButtonScreenSettingWiFiReturn, LV_OBJ_FLAG_HIDDEN);

    /* 3. Wi-Fi Verification / Password Screen */
    lv_obj_add_flag(ui_SpinnerScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);

    s_btn_verify_back = lv_btn_create(ui_ScreenSettingVerification);
    lv_obj_set_style_radius(s_btn_verify_back, 10, 0);
    lv_obj_set_style_bg_color(s_btn_verify_back, lv_color_hex(0x475569), 0);
    lv_obj_t *lbl_back = lv_label_create(s_btn_verify_back);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(s_btn_verify_back, on_verify_back_clicked_cb, LV_EVENT_CLICKED, this);

    s_label_verify_meta = lv_label_create(ui_ScreenSettingVerification);
    lv_obj_set_style_text_font(s_label_verify_meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_verify_meta, lv_color_hex(0x475569), 0);
    lv_label_set_text(s_label_verify_meta, "");

    s_btn_verify_eye = lv_btn_create(ui_ScreenSettingVerification);
    lv_obj_set_style_radius(s_btn_verify_eye, 10, 0);
    lv_obj_set_style_bg_color(s_btn_verify_eye, lv_color_hex(0x64748B), 0);
    s_label_verify_eye = lv_label_create(s_btn_verify_eye);
    lv_obj_set_style_text_font(s_label_verify_eye, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_verify_eye, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_label_verify_eye, "Show");
    lv_obj_center(s_label_verify_eye);
    lv_obj_add_event_cb(s_btn_verify_eye, on_verify_eye_clicked_cb, LV_EVENT_CLICKED, this);

    s_btn_verify_connect = lv_btn_create(ui_ScreenSettingVerification);
    lv_obj_set_style_radius(s_btn_verify_connect, 10, 0);
    lv_obj_set_style_bg_color(s_btn_verify_connect, lv_color_hex(0x059669), 0);
    lv_obj_t *lbl_conn = lv_label_create(s_btn_verify_connect);
    lv_obj_set_style_text_font(lbl_conn, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_conn, LV_SYMBOL_WIFI " Connect");
    lv_obj_center(lbl_conn);
    lv_obj_add_event_cb(s_btn_verify_connect, onKeyboardScreenSettingVerificationReadyCallback, LV_EVENT_CLICKED, this);

    s_btn_verify_forget = lv_btn_create(ui_ScreenSettingVerification);
    lv_obj_set_style_radius(s_btn_verify_forget, 10, 0);
    lv_obj_set_style_bg_color(s_btn_verify_forget, lv_color_hex(0xDC2626), 0);
    lv_obj_t *lbl_forget = lv_label_create(s_btn_verify_forget);
    lv_obj_set_style_text_font(lbl_forget, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_forget, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_forget, LV_SYMBOL_TRASH " Forget");
    lv_obj_center(lbl_forget);
    lv_obj_add_event_cb(s_btn_verify_forget, on_verify_forget_clicked_cb, LV_EVENT_CLICKED, this);

    detach_obj_from_group(s_btn_verify_back);
    detach_obj_from_group(s_btn_verify_eye);
    detach_obj_from_group(s_btn_verify_connect);
    detach_obj_from_group(s_btn_verify_forget);

    _panel_wifi_connect = lv_obj_create(ui_ScreenSettingVerification);
    lv_obj_set_size(_panel_wifi_connect, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(_panel_wifi_connect, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(_panel_wifi_connect, LV_OPA_80, 0);
    lv_obj_center(_panel_wifi_connect);

    _img_wifi_connect = lv_img_create(_panel_wifi_connect);
    lv_obj_align(_img_wifi_connect, LV_ALIGN_CENTER, 0, -20);

    _spinner_wifi_connect = lv_spinner_create(_panel_wifi_connect, 1000, 600);
    lv_obj_set_size(_spinner_wifi_connect, 110, 110);
    lv_obj_align(_spinner_wifi_connect, LV_ALIGN_CENTER, 0, -25);

    s_label_connect_overlay = lv_label_create(_panel_wifi_connect);
    lv_obj_set_style_text_font(s_label_connect_overlay, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_label_connect_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_label_connect_overlay, LV_ALIGN_CENTER, 0, 65);
    lv_label_set_text(s_label_connect_overlay, "Connecting...");

    processWifiConnect(WIFI_CONNECT_HIDE);

    lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
    lv_keyboard_set_textarea(ui_KeyboardScreenSettingVerification, ui_TextAreaScreenSettingVerificationPassword);
    lv_obj_add_event_cb(ui_KeyboardScreenSettingVerification, onKeyboardScreenSettingVerificationReadyCallback, LV_EVENT_READY, this);
    lv_obj_add_event_cb(ui_TextAreaScreenSettingVerificationPassword, onKeyboardScreenSettingVerificationReadyCallback, LV_EVENT_READY, this);

    lv_obj_add_flag(ui_ButtonScreenSettingBLEReturn, LV_OBJ_FLAG_HIDDEN);
    _screen_list[UI_WIFI_SCAN_INDEX] = ui_ScreenSettingWiFi;
    lv_obj_add_event_cb(ui_ScreenSettingWiFi, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);
    _screen_list[UI_WIFI_CONNECT_INDEX] = ui_ScreenSettingVerification;
    lv_obj_add_event_cb(ui_ScreenSettingVerification, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* 4. Bluetooth */
    lv_obj_clear_flag(ui_PanelSettingMainContainerItem2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingBLESwitch, onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    _screen_list[UI_BLUETOOTH_SETTING_INDEX] = ui_ScreenSettingBLE;
    lv_obj_add_event_cb(ui_ScreenSettingBLE, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* 5. Display & Brightness Screen (ui_ScreenSettingLight) */
    lv_obj_add_flag(ui_ButtonScreenSettingLightReturn, LV_OBJ_FLAG_HIDDEN);
    lv_slider_set_range(ui_SliderPanelScreenSettingLightSwitch1, SCREEN_BRIGHTNESS_MIN, SCREEN_BRIGHTNESS_MAX);
    lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, lexios_get_user_brightness_pct(), LV_ANIM_OFF);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingLightSwitch1, onSliderPanelLightSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    detach_obj_from_group(ui_SliderPanelScreenSettingLightSwitch1);

    s_label_light_pct = lv_label_create(ui_PanelScreenSettingLightSwitch);
    lv_obj_set_style_text_font(s_label_light_pct, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_light_pct, lv_color_hex(0x2563EB), 0);
    lv_label_set_text_fmt(s_label_light_pct, "%d%%", lexios_get_user_brightness_pct());

    lv_obj_clear_flag(ui_PanelScreenSettingLightList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_PanelScreenSettingLightList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_PanelScreenSettingLightList, LV_DIR_VER);
    lv_obj_set_style_bg_opa(ui_PanelScreenSettingLightList, 0, 0);
    lv_obj_set_style_border_width(ui_PanelScreenSettingLightList, 0, 0);
    lv_obj_set_style_pad_all(ui_PanelScreenSettingLightList, 4, 0);
    lv_obj_set_style_pad_row(ui_PanelScreenSettingLightList, 10, 0);

    /* Display Card 1: Screen Orientation */
    lv_obj_t *card_rot = create_setting_card(ui_PanelScreenSettingLightList, "Screen Orientation", 112);
    lv_obj_t *row_rot = lv_obj_create(card_rot);
    lv_obj_remove_style_all(row_rot);
    lv_obj_set_size(row_rot, lv_pct(100), 48);
    lv_obj_align(row_rot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row_rot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_rot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_rot, 8, 0);

    static const char *kRotLabels[4] = {"Auto (KB)", "Portrait", "Landscape", "Flip 180"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(row_rot);
        detach_obj_from_group(b);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_rot_btn_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, kRotLabels[i]);
        lv_obj_center(l);
        s_btn_light_rot[i] = b;
    }

    /* Display Card 2: Screen Auto-Sleep Timeout */
    lv_obj_t *card_slp = create_setting_card(ui_PanelScreenSettingLightList, "Screen Timeout (Auto-Sleep)", 112);
    lv_obj_t *row_slp = lv_obj_create(card_slp);
    lv_obj_remove_style_all(row_slp);
    lv_obj_set_size(row_slp, lv_pct(100), 48);
    lv_obj_align(row_slp, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row_slp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_slp, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_slp, 8, 0);

    static const char *kSlpLabels[4] = {"30 sec", "1 min", "5 min", "Never"};
    static const int kSlpVals[4] = {30, 60, 300, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(row_slp);
        detach_obj_from_group(b);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_slp_btn_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kSlpVals[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, kSlpLabels[i]);
        lv_obj_center(l);
        s_btn_light_slp[i] = b;
    }

    /* Display Card 3: Physical Keyboard Backlight */
    lv_obj_t *card_kb = create_setting_card(ui_PanelScreenSettingLightList, "Physical Keyboard Backlight", 112);
    lv_obj_t *row_kb = lv_obj_create(card_kb);
    lv_obj_remove_style_all(row_kb);
    lv_obj_set_size(row_kb, lv_pct(100), 48);
    lv_obj_align(row_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row_kb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_kb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_kb, 8, 0);

    static const char *kKbLabels[4] = {"Off", "Low", "Medium", "High"};
    static const int kKbVals[4] = {0, 5, 25, 60};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(row_kb);
        detach_obj_from_group(b);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_kb_btn_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kKbVals[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, kKbLabels[i]);
        lv_obj_center(l);
        s_btn_light_kb[i] = b;
    }

    _screen_list[UI_BRIGHTNESS_SETTING_INDEX] = ui_ScreenSettingLight;
    lv_obj_add_event_cb(ui_ScreenSettingLight, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* 6. Battery & Power Management Screen (s_screen_power) */
    s_screen_power = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen_power, lv_color_hex(0xE5F3FF), 0);
    lv_obj_set_style_bg_opa(s_screen_power, 255, 0);

    s_panel_power_scroll = lv_obj_create(s_screen_power);
    lv_obj_set_flex_flow(s_panel_power_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel_power_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_panel_power_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_panel_power_scroll, LV_DIR_VER);
    lv_obj_set_style_bg_opa(s_panel_power_scroll, 0, 0);
    lv_obj_set_style_border_width(s_panel_power_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_panel_power_scroll, 4, 0);
    lv_obj_set_style_pad_row(s_panel_power_scroll, 10, 0);

    /* Power Card 1: Live Battery & Charging Telemetry */
    lv_obj_t *card_bat = create_setting_card(s_panel_power_scroll, "", 108);
    s_label_pwr_bat_headline = lv_label_create(card_bat);
    lv_obj_set_style_text_font(s_label_pwr_bat_headline, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_label_pwr_bat_headline, lv_color_hex(0x0F172A), 0);
    lv_obj_align(s_label_pwr_bat_headline, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_label_pwr_bat_headline, "Battery: --%");

    s_label_pwr_bat_detail = lv_label_create(card_bat);
    lv_obj_set_style_text_font(s_label_pwr_bat_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_pwr_bat_detail, lv_color_hex(0x334155), 0);
    lv_obj_align(s_label_pwr_bat_detail, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_label_set_text(s_label_pwr_bat_detail, "Capacity: -- / -- mAh  |  -- V");

    s_label_pwr_uvlo_status = lv_label_create(card_bat);
    lv_obj_set_style_text_font(s_label_pwr_uvlo_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_pwr_uvlo_status, lv_color_hex(0x059669), 0);
    lv_obj_align(s_label_pwr_uvlo_status, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_label_set_text(s_label_pwr_uvlo_status, "UVLO Protection Active");

    /* Power Card 2: Battery Saver Mode */
    lv_obj_t *card_saver = create_setting_card(s_panel_power_scroll, "Battery Saver Mode", 88);
    lv_obj_t *lbl_saver_sub = lv_label_create(card_saver);
    lv_obj_set_style_text_font(lbl_saver_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_saver_sub, lv_color_hex(0x64748B), 0);
    lv_obj_align(lbl_saver_sub, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_label_set_text(lbl_saver_sub, "Caps CPU1 to 594 MHz (-63% power), dims AMOLED & KB LED");

    s_switch_pwr_saver = lv_switch_create(card_saver);
    detach_obj_from_group(s_switch_pwr_saver);
    lv_obj_set_size(s_switch_pwr_saver, 72, 40);
    lv_obj_align(s_switch_pwr_saver, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_switch_pwr_saver, on_bat_saver_switch_cb, LV_EVENT_VALUE_CHANGED, this);

    /* Power Card 3: Auto-Sleep & Pocket Standby */
    lv_obj_t *card_pslp = create_setting_card(s_panel_power_scroll, "Pocket Standby & Screen Timeout", 118);
    lv_obj_t *btn_slp_now = lv_btn_create(card_pslp);
    detach_obj_from_group(btn_slp_now);
    lv_obj_set_size(btn_slp_now, 146, 36);
    lv_obj_align(btn_slp_now, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_radius(btn_slp_now, 8, 0);
    lv_obj_set_style_bg_color(btn_slp_now, lv_color_hex(0x1E3A8A), 0);
    lv_obj_add_event_cb(btn_slp_now, on_sleep_now_btn_clicked_cb, LV_EVENT_CLICKED, this);
    lv_obj_t *lbl_slp_now = lv_label_create(btn_slp_now);
    lv_obj_set_style_text_font(lbl_slp_now, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_slp_now, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_slp_now, "Sleep Screen Now");
    lv_obj_center(lbl_slp_now);

    lv_obj_t *row_pslp = lv_obj_create(card_pslp);
    lv_obj_remove_style_all(row_pslp);
    lv_obj_set_size(row_pslp, lv_pct(100), 48);
    lv_obj_align(row_pslp, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row_pslp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_pslp, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_pslp, 8, 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(row_pslp);
        detach_obj_from_group(b);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_slp_btn_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)kSlpVals[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, kSlpLabels[i]);
        lv_obj_center(l);
        s_btn_pwr_slp[i] = b;
    }

    /* Power Card 4: SoC Thermal & Cooling Fan Control */
    lv_obj_t *card_therm = create_setting_card(s_panel_power_scroll, "Thermal & Cooling Fan", 136);
    s_label_pwr_thermal_info = lv_label_create(card_therm);
    lv_obj_set_style_text_font(s_label_pwr_thermal_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_pwr_thermal_info, lv_color_hex(0x475569), 0);
    lv_obj_align(s_label_pwr_thermal_info, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_label_set_text(s_label_pwr_thermal_info, "Die: -- °C  |  CPU1: -- MHz  |  Fan: --");

    lv_obj_t *row_fan = lv_obj_create(card_therm);
    lv_obj_remove_style_all(row_fan);
    lv_obj_set_size(row_fan, lv_pct(100), 48);
    lv_obj_align(row_fan, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row_fan, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_fan, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_fan, 8, 0);

    static const char *kFanLabels[3] = {"Auto (>=45°C)", "Force ON", "Force OFF"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(row_fan);
        detach_obj_from_group(b);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_fan_btn_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, kFanLabels[i]);
        lv_obj_center(l);
        s_btn_pwr_fan[i] = b;
    }

    /* Power Card 5: Radio Power Rails */
    lv_obj_t *card_rf = create_setting_card(s_panel_power_scroll, "Radio Power Rails", 136);
    lv_obj_t *lbl_rf_wifi = lv_label_create(card_rf);
    lv_obj_set_style_text_font(lbl_rf_wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_rf_wifi, lv_color_hex(0x1E293B), 0);
    lv_obj_align(lbl_rf_wifi, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_label_set_text(lbl_rf_wifi, "2.4 GHz Wi-Fi & BLE Radio");

    s_switch_pwr_wifi_rail = lv_switch_create(card_rf);
    detach_obj_from_group(s_switch_pwr_wifi_rail);
    lv_obj_set_size(s_switch_pwr_wifi_rail, 68, 36);
    lv_obj_align(s_switch_pwr_wifi_rail, LV_ALIGN_TOP_RIGHT, 0, 34);
    lv_obj_add_event_cb(s_switch_pwr_wifi_rail, on_wifi_rail_switch_cb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *lbl_rf_sat = lv_label_create(card_rf);
    lv_obj_set_style_text_font(lbl_rf_sat, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_rf_sat, lv_color_hex(0x1E293B), 0);
    lv_obj_align(lbl_rf_sat, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_label_set_text(lbl_rf_sat, "nRF9151 Satellite / GNSS / LTE-M (3.3V)");

    s_switch_pwr_sat_rail = lv_switch_create(card_rf);
    detach_obj_from_group(s_switch_pwr_sat_rail);
    lv_obj_set_size(s_switch_pwr_sat_rail, 68, 36);
    lv_obj_align(s_switch_pwr_sat_rail, LV_ALIGN_TOP_RIGHT, 0, 80);
    lv_obj_add_event_cb(s_switch_pwr_sat_rail, on_sat_rail_switch_cb, LV_EVENT_VALUE_CHANGED, this);

    /* Power Card 6: System Power Actions (Reboot & Ship Mode Power Off) */
    lv_obj_t *card_act = create_setting_card(s_panel_power_scroll, "System Power", 112);
    lv_obj_t *row_act = lv_obj_create(card_act);
    lv_obj_remove_style_all(row_act);
    lv_obj_set_size(row_act, lv_pct(100), 50);
    lv_obj_align(row_act, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row_act, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_act, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_act, 12, 0);

    s_btn_pwr_reboot = lv_btn_create(row_act);
    detach_obj_from_group(s_btn_pwr_reboot);
    lv_obj_set_height(s_btn_pwr_reboot, 46);
    lv_obj_set_flex_grow(s_btn_pwr_reboot, 1);
    lv_obj_set_style_radius(s_btn_pwr_reboot, 10, 0);
    lv_obj_set_style_bg_color(s_btn_pwr_reboot, lv_color_hex(0x2563EB), 0);
    lv_obj_add_event_cb(s_btn_pwr_reboot, on_reboot_btn_clicked_cb, LV_EVENT_CLICKED, this);
    s_label_pwr_reboot = lv_label_create(s_btn_pwr_reboot);
    lv_obj_set_style_text_font(s_label_pwr_reboot, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_pwr_reboot, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_label_pwr_reboot, "Reboot Device");
    lv_obj_center(s_label_pwr_reboot);

    s_btn_pwr_poweroff = lv_btn_create(row_act);
    detach_obj_from_group(s_btn_pwr_poweroff);
    lv_obj_set_height(s_btn_pwr_poweroff, 46);
    lv_obj_set_flex_grow(s_btn_pwr_poweroff, 1);
    lv_obj_set_style_radius(s_btn_pwr_poweroff, 10, 0);
    lv_obj_set_style_bg_color(s_btn_pwr_poweroff, lv_color_hex(0xDC2626), 0);
    lv_obj_add_event_cb(s_btn_pwr_poweroff, on_poweroff_btn_clicked_cb, LV_EVENT_CLICKED, this);
    s_label_pwr_poweroff = lv_label_create(s_btn_pwr_poweroff);
    lv_obj_set_style_text_font(s_label_pwr_poweroff, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_label_pwr_poweroff, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_label_pwr_poweroff, "Power Off (Ship Mode)");
    lv_obj_center(s_label_pwr_poweroff);

    _screen_list[UI_POWER_SETTING_INDEX] = s_screen_power;
    lv_obj_add_event_cb(s_screen_power, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* 7. Audio */
    lv_slider_set_range(ui_SliderPanelScreenSettingVolumeSwitch, SPEAKER_VOLUME_MIN, SPEAKER_VOLUME_MAX);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingVolumeSwitch, onSliderPanelVolumeSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_flag(ui_ButtonScreenSettingVolumeReturn, LV_OBJ_FLAG_HIDDEN);
    _screen_list[UI_VOLUME_SETTING_INDEX] = ui_ScreenSettingVolume;
    lv_obj_add_event_cb(ui_ScreenSettingVolume, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* 8. About */
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout4, "LexiOS v1.0 (RT-Smart)");
    lv_obj_add_flag(ui_ButtonScreenSettingAboutReturn, LV_OBJ_FLAG_HIDDEN);
    _screen_list[UI_ABOUT_SETTING_INDEX] = ui_ScreenSettingAbout;
    lv_obj_add_event_cb(ui_ScreenSettingAbout, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout3, mac_str);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout5, "v0.2.0");
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout2, "T-Display-K230 Sovereign");

    char char_ui_version[20];
    snprintf(char_ui_version, sizeof(char_ui_version), "v%d.%d.%d", ESP_BROOKESIA_CONF_VER_MAJOR, ESP_BROOKESIA_CONF_VER_MINOR, ESP_BROOKESIA_CONF_VER_PATCH);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout6, char_ui_version);

    s_last_known_scr_w = lv_disp_get_hor_res(nullptr);
    adapt_main_screen_geometry();
    adapt_wifi_screen_geometry();
    adapt_verification_screen_geometry();
    adapt_light_screen_geometry();
    adapt_power_screen_geometry();

    if (s_wifi_poll_timer) {
        lv_timer_del(s_wifi_poll_timer);
    }
    s_wifi_poll_timer = lv_timer_create([](lv_timer_t *t) {
        AppSettings *app = (AppSettings *)t->user_data;
        if (app) app->scanWifiAndUpdateUi();
    }, 350, this);
}

void AppSettings::processWifiConnect(WifiConnectState_t state)
{
    switch (state) {
    case WIFI_CONNECT_HIDE:
        lv_obj_add_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        if (s_label_connect_overlay) lv_obj_add_flag(s_label_connect_overlay, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_RUNNING:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        if (s_label_connect_overlay) {
            lv_obj_clear_flag(s_label_connect_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_label_connect_overlay, "Connecting to '%s'...", s_connect_target_ssid);
        }
        break;
    case WIFI_CONNECT_SUCCESS:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(_img_wifi_connect, &img_wifi_connect_success);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        if (s_label_connect_overlay) {
            lv_obj_clear_flag(s_label_connect_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_label_connect_overlay, "Connected (%s)", s_live_wifi_ip);
        }
        break;
    case WIFI_CONNECT_FAIL:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(_img_wifi_connect, &img_wifi_connect_fail);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        if (s_label_connect_overlay) {
            lv_obj_clear_flag(s_label_connect_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_label_connect_overlay, "Connection Failed — Check Password");
        }
        break;
    default:
        break;
    }
}

bool AppSettings::loadNvsParam(void)
{
    return true;
}

bool AppSettings::setNvsParam(std::string key, int value)
{
    (void)key;
    (void)value;
    return true;
}

void AppSettings::updateUiByNvsParam(void)
{
    s_suppress_switch_cb = true;
    if (_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        lv_obj_add_state(ui_SwitchPanelScreenSettingWiFiSwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingWiFiSwitch, LV_STATE_CHECKED);
    }

    if (_nvs_param_map[NVS_KEY_BLE_ENABLE]) {
        lv_obj_add_state(ui_SwitchPanelScreenSettingBLESwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingBLESwitch, LV_STATE_CHECKED);
    }
    s_suppress_switch_cb = false;

    lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderPanelScreenSettingVolumeSwitch, _nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
}

int AppSettings::initWifi()
{
    return 0;
}

void AppSettings::startWifiScan(void)
{
    adapt_wifi_screen_geometry();
    read_live_wifi_status();

    if (!_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        stopWifiScan();
        return;
    }

    lv_obj_clear_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_wifi_rescan) {
        lv_obj_clear_flag(s_btn_wifi_rescan, LV_OBJ_FLAG_HIDDEN);
    }

    send_wifi_daemon_cmd("SCAN");
    s_last_auto_scan_tick = lv_tick_get();
    s_force_list_refresh = true;
    scanWifiAndUpdateUi();
}

void AppSettings::stopWifiScan(void)
{
    lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_wifi_rescan && !_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        lv_obj_add_flag(s_btn_wifi_rescan, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_label_wifi_header_sub && !_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        lv_label_set_text(s_label_wifi_header_sub, "Off");
    }
    deinitWifiListButton();
}

void AppSettings::scanWifiAndUpdateUi(void)
{
    if (_is_ui_del || _is_ui_resumed) return;

    lv_coord_t cur_w = lv_disp_get_hor_res(nullptr);
    if (cur_w != s_last_known_scr_w) {
        s_last_known_scr_w = cur_w;
        adapt_main_screen_geometry();
        adapt_wifi_screen_geometry();
        adapt_verification_screen_geometry();
        adapt_light_screen_geometry();
        adapt_power_screen_geometry();
    }

    read_live_wifi_status();

    if (_screen_index == UI_MAIN_SETTING_INDEX ||
        _screen_index == UI_BRIGHTNESS_SETTING_INDEX ||
        _screen_index == UI_POWER_SETTING_INDEX) {
        refresh_display_and_power_ui();
    }

    /* Sync status bar Wi-Fi icon with live signal strength */
    if (!s_live_wifi_enabled || !s_live_wifi_connected) {
        this->status_bar->setWifiIconState(0);
    } else if (s_live_wifi_rssi > -60) {
        this->status_bar->setWifiIconState(3);
    } else if (s_live_wifi_rssi > -75) {
        this->status_bar->setWifiIconState(2);
    } else {
        this->status_bar->setWifiIconState(1);
    }

    /* Handle asynchronous connection verification state machine on UI_WIFI_CONNECT_INDEX */
    if (_screen_index == UI_WIFI_CONNECT_INDEX && s_connect_in_progress) {
        uint32_t now_tick = lv_tick_get();
        if (s_connect_result_state == 0) {
            uint32_t elapsed = now_tick - s_connect_start_tick;
            if (elapsed >= 900 && s_live_wifi_connected &&
                strcmp(s_live_wifi_ssid, s_connect_target_ssid) == 0 &&
                strcmp(s_live_wifi_ip, "0.0.0.0") != 0) {
                s_connect_result_state = 1;
                s_connect_result_tick = now_tick;
                processWifiConnect(WIFI_CONNECT_SUCCESS);
            } else if (elapsed >= 900 && strcmp(s_live_wifi_state, "AUTH_FAILED") == 0) {
                s_connect_result_state = -1;
                s_connect_result_tick = now_tick;
                processWifiConnect(WIFI_CONNECT_FAIL);
            } else if (elapsed >= 15000) {
                s_connect_result_state = -1;
                s_connect_result_tick = now_tick;
                processWifiConnect(WIFI_CONNECT_FAIL);
            }
        } else if (s_connect_result_state == 1) {
            if (now_tick - s_connect_result_tick >= 900) {
                s_connect_in_progress = false;
                processWifiConnect(WIFI_CONNECT_HIDE);
                s_force_list_refresh = true;
                lv_scr_load(ui_ScreenSettingWiFi);
            }
        } else if (s_connect_result_state == -1) {
            if (now_tick - s_connect_result_tick >= 1400) {
                s_connect_in_progress = false;
                processWifiConnect(WIFI_CONNECT_HIDE);
            }
        }
    }

    if (_screen_index != UI_WIFI_SCAN_INDEX) return;

    /* Update header subtitle */
    if (s_label_wifi_header_sub) {
        if (!_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
            lv_label_set_text(s_label_wifi_header_sub, "Off");
        } else if (strcmp(s_live_wifi_state, "CONNECTING") == 0) {
            lv_label_set_text_fmt(s_label_wifi_header_sub, "Connecting...");
        } else if (s_live_wifi_connected) {
            lv_label_set_text_fmt(s_label_wifi_header_sub, "%s (%s • %ddBm)",
                                  s_live_wifi_ssid, s_live_wifi_ip, s_live_wifi_rssi);
        } else if (strcmp(s_live_wifi_state, "SCANNING") == 0) {
            lv_label_set_text(s_label_wifi_header_sub, "Scanning networks...");
        } else {
            lv_label_set_text(s_label_wifi_header_sub, "Disconnected • Auto-Reconnect Active");
        }
    }

    if (!_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        return;
    }

    /* Trigger periodic background scan every 15s while Wi-Fi list screen is open */
    uint32_t now_tick = lv_tick_get();
    if (now_tick - s_last_auto_scan_tick >= 15000 && strcmp(s_live_wifi_state, "CONNECTING") != 0) {
        send_wifi_daemon_cmd("SCAN");
        s_last_auto_scan_tick = now_tick;
    }

    if (!s_force_list_refresh && s_live_scan_seq == s_last_rendered_scan_seq && s_scan_ap_count > 0) {
        return;
    }

    load_scan_results_from_file();
    s_last_rendered_scan_seq = s_live_scan_seq;
    s_force_list_refresh = false;

    if (s_scan_ap_count == 0) {
        lv_obj_clear_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < SCAN_LIST_SIZE; i++) {
        if (i >= s_scan_ap_count) {
            lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        ui_wifi_ap_t &ap = s_scan_aps[i];
        lv_obj_clear_flag(panel_wifi_btn[i], LV_OBJ_FLAG_HIDDEN);

        if (ap.rssi > -60) {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_GOOD;
        } else if (ap.rssi > -75) {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_MODERATE;
        } else if (ap.rssi > -100) {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_WEAK;
        } else {
            _wifi_signal_strength_level = WIFI_SIGNAL_STRENGTH_NONE;
        }

        initWifiListButton(label_wifi_ssid[i], img_img_wifi_lock[i], wifi_image[i], wifi_connect[i],
                           (uint8_t *)ap.ssid, ap.secured, _wifi_signal_strength_level);

        if (ap.connected) {
            lv_obj_clear_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(panel_wifi_btn[i], lv_color_hex(0xECFDF5), LV_PART_MAIN);
            lv_obj_set_style_border_color(panel_wifi_btn[i], lv_color_hex(0x10B981), LV_PART_MAIN);
            lv_obj_set_style_text_color(label_wifi_sub[i], lv_color_hex(0x059669), 0);
            lv_label_set_text_fmt(label_wifi_sub[i], "Connected • %s (%d dBm)", s_live_wifi_ip, ap.rssi);
        } else if (ap.saved) {
            lv_obj_add_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(panel_wifi_btn[i], lv_color_hex(0xEFF6FF), LV_PART_MAIN);
            lv_obj_set_style_border_color(panel_wifi_btn[i], lv_color_hex(0x93C5FD), LV_PART_MAIN);
            lv_obj_set_style_text_color(label_wifi_sub[i], lv_color_hex(0x2563EB), 0);
            lv_label_set_text_fmt(label_wifi_sub[i], "Saved • %s (%d dBm)", ap.secured ? "WPA2" : "Open", ap.rssi);
        } else {
            lv_obj_add_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(panel_wifi_btn[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_border_color(panel_wifi_btn[i], lv_color_hex(0xCBD5E1), LV_PART_MAIN);
            lv_obj_set_style_text_color(label_wifi_sub[i], lv_color_hex(0x64748B), 0);
            lv_label_set_text_fmt(label_wifi_sub[i], "%s (%d dBm • Ch %d)",
                                  ap.secured ? "Secured" : "Open", ap.rssi, ap.channel);
        }
    }
}

void AppSettings::initWifiListButton(lv_obj_t* lv_label_ssid, lv_obj_t* lv_img_wifi_lock, lv_obj_t* lv_wifi_img,
                                     lv_obj_t *lv_wifi_connect, uint8_t* ssid, bool psk, WifiSignalStrengthLevel_t signal_strength)
{
    lv_label_set_text_fmt(lv_label_ssid, "%s", (const char*)ssid);

    if (strcmp((const char*)ssid, (const char*)st_wifi_ssid) == 0 && st_wifi_ssid[0] != '\0') {
        lv_obj_clear_flag(lv_wifi_connect, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lv_wifi_connect, LV_OBJ_FLAG_HIDDEN);
    }

    if (psk) {
        lv_img_set_src(lv_img_wifi_lock, &img_wifi_lock);
        lv_obj_clear_flag(lv_img_wifi_lock, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lv_img_wifi_lock, LV_OBJ_FLAG_HIDDEN);
    }

    if (signal_strength == WIFI_SIGNAL_STRENGTH_GOOD) {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_good);
    } else if (signal_strength == WIFI_SIGNAL_STRENGTH_MODERATE) {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_moderate);
    } else if (signal_strength == WIFI_SIGNAL_STRENGTH_WEAK) {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_wake);
    } else {
        lv_img_set_src(lv_wifi_img, &img_wifisignal_absent);
    }
}

void AppSettings::deinitWifiListButton(void)
{
    for (int i = 0; i < SCAN_LIST_SIZE; i++) {
        lv_obj_add_flag(img_img_wifi_lock[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_connect[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_wifi_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::wifiConnectTask(void *arg)
{
    (void)arg;
}

void AppSettings::onKeyboardScreenSettingVerificationReadyCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    {
        const char *ssid = (s_selected_ap_idx >= 0 && s_selected_ap_idx < s_scan_ap_count)
                           ? s_scan_aps[s_selected_ap_idx].ssid
                           : lv_label_get_text(ui_LabelScreenSettingVerificationSSID);
        const char *key = lv_textarea_get_text(ui_TextAreaScreenSettingVerificationPassword);
        bool secured = (s_selected_ap_idx >= 0 && s_selected_ap_idx < s_scan_ap_count)
                       ? s_scan_aps[s_selected_ap_idx].secured
                       : true;

        if (!ssid || ssid[0] == '\0') goto end;

        if (secured && (!key || strlen(key) < 8)) {
            if (s_label_verify_meta) {
                lv_obj_set_style_text_color(s_label_verify_meta, lv_color_hex(0xDC2626), 0);
                lv_label_set_text(s_label_verify_meta, "Password must be at least 8 characters");
            }
            goto end;
        }

        snprintf(s_connect_target_ssid, sizeof(s_connect_target_ssid), "%s", ssid);
        char cmd[192];
        snprintf(cmd, sizeof(cmd), "CONNECT %s\t%s", ssid, secured ? (key ? key : "") : "");
        send_wifi_daemon_cmd(cmd);

        s_connect_in_progress = true;
        s_connect_result_state = 0;
        s_connect_start_tick = lv_tick_get();
        app->processWifiConnect(WIFI_CONNECT_RUNNING);
    }

end:
    return;
}

void AppSettings::onScreenLoadEventCallback( lv_event_t * e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    SettingScreenIndex_t last_scr_index = app->_screen_index;

    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    for (int i = 0; i < UI_MAX_INDEX; i++) {
        if (app->_screen_list[i] == lv_event_get_target(e)) {
            app->_screen_index = (SettingScreenIndex_t)i;
            break;
        }
    }

    if (last_scr_index == UI_WIFI_CONNECT_INDEX && ui_TextAreaScreenSettingVerificationPassword) {
        if (lv_group_get_default()) {
            lv_group_remove_obj(ui_TextAreaScreenSettingVerificationPassword);
        }
    }

    if (app->_screen_index == UI_MAIN_SETTING_INDEX) {
        adapt_main_screen_geometry();
        refresh_display_and_power_ui();
    } else if (app->_screen_index == UI_WIFI_SCAN_INDEX) {
        read_live_wifi_status();
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = s_live_wifi_enabled ? 1 : 0;
        app->updateUiByNvsParam();
        if (app->_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
            app->startWifiScan();
        } else {
            app->stopWifiScan();
        }
    } else if (app->_screen_index == UI_WIFI_CONNECT_INDEX) {
        adapt_verification_screen_geometry();
        if (lv_group_get_default() && ui_TextAreaScreenSettingVerificationPassword) {
            lv_group_add_obj(lv_group_get_default(), ui_TextAreaScreenSettingVerificationPassword);
            lv_group_focus_obj(ui_TextAreaScreenSettingVerificationPassword);
        }
    } else if (app->_screen_index == UI_BRIGHTNESS_SETTING_INDEX) {
        adapt_light_screen_geometry();
        refresh_display_and_power_ui();
    } else if (app->_screen_index == UI_POWER_SETTING_INDEX) {
        s_confirm_reboot = false;
        s_confirm_poweroff = false;
        adapt_power_screen_geometry();
        refresh_display_and_power_ui();
    } else if (app->_screen_index == UI_ABOUT_SETTING_INDEX) {
        read_live_wifi_status();
        lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout3, mac_str);
    }

end:
    return;
}

void AppSettings::onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback( lv_event_t * e) {
    if (s_suppress_switch_cb) return;

    lv_state_t state = lv_obj_get_state(ui_SwitchPanelScreenSettingWiFiSwitch);
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (state & LV_STATE_CHECKED) {
        lexios_set_wifi_ble_power(true);
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = true;
        app->setNvsParam(NVS_KEY_WIFI_ENABLE, 1);
        s_live_wifi_enabled = true;
        send_wifi_daemon_cmd("ENABLE 1");
        if (app->_screen_index == UI_WIFI_SCAN_INDEX) {
            app->startWifiScan();
        }
    } else {
        lexios_set_wifi_ble_power(false);
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
        app->setNvsParam(NVS_KEY_WIFI_ENABLE, 0);
        s_live_wifi_enabled = false;
        s_live_wifi_connected = false;
        send_wifi_daemon_cmd("ENABLE 0");
        app->status_bar->setWifiIconState(0);
        if (app->_screen_index == UI_WIFI_SCAN_INDEX) {
            app->stopWifiScan();
        }
    }

end:
    return;
}

void AppSettings::onButtonWifiListClickedEventCallback(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_scan_ap_count) return;

    s_selected_ap_idx = idx;
    ui_wifi_ap_t &ap = s_scan_aps[idx];

    lv_label_set_text_fmt(ui_LabelScreenSettingVerificationSSID, "%s", ap.ssid);

    char saved_pass[64] = "";
    bool has_saved = lookup_saved_wifi_pass(ap.ssid, saved_pass, sizeof(saved_pass));

    if (ui_TextAreaScreenSettingVerificationPassword) {
        lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
        if (s_label_verify_eye) lv_label_set_text(s_label_verify_eye, "Show");
        lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, has_saved ? saved_pass : "");
    }

    if (s_label_verify_meta) {
        lv_obj_set_style_text_color(s_label_verify_meta, lv_color_hex(0x475569), 0);
        if (ap.connected) {
            lv_label_set_text_fmt(s_label_verify_meta, "Connected (%s • %d dBm)", s_live_wifi_ip, ap.rssi);
        } else if (has_saved) {
            lv_label_set_text_fmt(s_label_verify_meta, "Saved Network (%d dBm) — Tap Connect or Edit Password", ap.rssi);
        } else if (!ap.secured) {
            lv_label_set_text_fmt(s_label_verify_meta, "Open Network (%d dBm) — Tap Connect", ap.rssi);
        } else {
            lv_label_set_text_fmt(s_label_verify_meta, "WPA2 Secured (%d dBm) — Enter Password", ap.rssi);
        }
    }

    if (s_btn_verify_forget) {
        if (ap.saved || ap.connected) {
            lv_obj_clear_flag(s_btn_verify_forget, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_btn_verify_forget, LV_OBJ_FLAG_HIDDEN);
        }
    }

    adapt_verification_screen_geometry();
    lv_scr_load(ui_ScreenSettingVerification);
}

void AppSettings::onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback( lv_event_t * e) {
    if (s_suppress_switch_cb) return;
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    bool en = (lv_obj_get_state(ui_SwitchPanelScreenSettingBLESwitch) & LV_STATE_CHECKED) != 0;
    app->_nvs_param_map[NVS_KEY_BLE_ENABLE] = en ? 1 : 0;
    if (en) {
        lexios_set_wifi_ble_power(true);
    }
}

void AppSettings::onSliderPanelVolumeSwitchValueChangeEventCallback( lv_event_t * e) {
    (void)e;
}

void AppSettings::onSliderPanelLightSwitchValueChangeEventCallback( lv_event_t * e) {
    brightness = lv_slider_get_value(ui_SliderPanelScreenSettingLightSwitch1);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (brightness != app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS]) {
        app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
        app->setNvsParam(NVS_KEY_DISPLAY_BRIGHTNESS, brightness);
    }
    lexios_set_user_brightness_pct(brightness);
    if (s_label_light_pct) {
        lv_label_set_text_fmt(s_label_light_pct, "%d%%", brightness);
    }

end:
    return;
}

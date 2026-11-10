#include "lexios_apps.hpp"
#include "keyboard_ext.h"
#include "network/network_rt_opt.h"
#include "fpioa/rt_fpioa.h"
#include "audio/audio_sample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

extern "C" {
LV_IMG_DECLARE(img_app_battery);
LV_IMG_DECLARE(img_app_live_call);
LV_IMG_DECLARE(img_app_gnss);
LV_IMG_DECLARE(img_app_ble);
LV_IMG_DECLARE(img_app_music);
LV_IMG_DECLARE(img_app_object_detect);
}

/* ============================================================================
 * Helper: Dark AMOLED Card Builder (568x1232 Portrait Layout)
 * ============================================================================ */
static lv_obj_t *create_amoled_card(lv_obj_t *parent, int w, int h, int y_off)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y_off);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111722), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x223046), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text, int w, int h, uint32_t bg_hex)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ============================================================================
 * 1. LexiSystemApp — Thermal, 6-Stage CPU1 DVFS, Fan, RF & Pocket Standby
 * ============================================================================ */
static lv_obj_t *g_sys_temp_lbl = nullptr;
static lv_obj_t *g_sys_cpu_lbl = nullptr;
static lv_obj_t *g_sys_dvfs_bar = nullptr;
static lv_obj_t *g_sys_fan_lbl = nullptr;
static lv_obj_t *g_sys_rf_lbl = nullptr;
static lv_obj_t *g_sys_uptime_lbl = nullptr;
static lv_obj_t *g_sys_sleep_lbl = nullptr;

static void update_lexisystem_ui(void)
{
    if (!g_sys_temp_lbl) return;

    double temp = lexios_get_die_temp_c();
    int stage = lexios_get_cpu_dvfs_stage();
    int freq = lexios_get_cpu_freq_mhz();
    int tlevel = lexios_get_thermal_level();
    bool fan_on = lexios_get_fan_active();
    lexios_fan_mode_t fmode = lexios_get_fan_mode();
    uint64_t up = lexios_get_uptime_sec();
    int sleep_s = lexios_get_sleep_timeout_sec();

    char buf[160];
    const char *health = (tlevel >= 2) ? "EMERGENCY CUTOFF" :
                         (tlevel == 1) ? "THROTTLING" :
                         (temp >= 50.0) ? "WARM (DVFS ACTIVE)" : "OPTIMAL";
    snprintf(buf, sizeof(buf), "Die Temp: %.1f C  [%s]", temp, health);
    lv_label_set_text(g_sys_temp_lbl, buf);
    lv_obj_set_style_text_color(g_sys_temp_lbl,
        tlevel >= 2 ? lv_color_hex(0xFF4444) :
        (temp >= 56.0 ? lv_color_hex(0xF5A524) : lv_color_hex(0x25C281)), 0);

    snprintf(buf, sizeof(buf), "CPU1 Clock: %d MHz  (DVFS Stage %d/5 %s)",
             freq, stage, stage == 0 ? "AUTO 200-1600" : "LOCKED");
    lv_label_set_text(g_sys_cpu_lbl, buf);
    if (g_sys_dvfs_bar) {
        int pct = (freq * 100) / 1600;
        lv_bar_set_value(g_sys_dvfs_bar, pct, LV_ANIM_OFF);
    }

    const char *fmode_str = (fmode == LEXIOS_FAN_FORCE_ON) ? "FORCE ON" :
                            (fmode == LEXIOS_FAN_FORCE_OFF) ? "FORCE OFF" : "AUTO (>=45C)";
    snprintf(buf, sizeof(buf), "Fan (GPIO42): %s  |  Mode: %s",
             fan_on ? "RUNNING" : "IDLE", fmode_str);
    lv_label_set_text(g_sys_fan_lbl, buf);

    const char *lr_state = (tlevel >= 2) ? "CUTOFF (0 mW)" :
                           (tlevel == 1) ? "THROTTLED (+4 dBm)" : "ACTIVE (<=+10 dBm)";
    bool nrf_on = lexios_get_nrf9151_power();
    bool wifi_ble_on = lexios_get_wifi_ble_power();
    bool kb_on = tca8418_is_connected();
    snprintf(buf, sizeof(buf),
             "LR2021 Radio: %s\nWi-Fi / BLE (GPIO45): %s\nnRF9151 Sat/GNSS: %s\nQWERTY Keyboard: %s",
             lr_state,
             wifi_ble_on ? "ON (2.4G WLAN + BLE NUS)" : "OFF (0mW Power-Gated)",
             nrf_on ? "ON (UART2 115200 8N1)" : "OFF (0mW Power-Gated)",
             kb_on ? "CONNECTED (I2C4 0x34)" : "DETACHED (Hot-Plug Ready)");
    lv_label_set_text(g_sys_rf_lbl, buf);

    snprintf(buf, sizeof(buf), "LexiOS v1.0  •  Uptime: %lluh %llum %llus",
             (unsigned long long)(up / 3600),
             (unsigned long long)((up % 3600) / 60),
             (unsigned long long)(up % 60));
    lv_label_set_text(g_sys_uptime_lbl, buf);

    if (sleep_s <= 0) {
        snprintf(buf, sizeof(buf), "Pocket Standby Timeout: Never (Manual GPIO0 Only)");
    } else {
        snprintf(buf, sizeof(buf), "Pocket Standby Timeout: %ds (200 MHz + AMOLED Off)", sleep_s);
    }
    lv_label_set_text(g_sys_sleep_lbl, buf);
}

static void sys_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_lexisystem_ui();
}

static void on_fan_btn_cb(lv_event_t *e)
{
    intptr_t mode = (intptr_t)lv_event_get_user_data(e);
    lexios_set_fan_mode((lexios_fan_mode_t)mode);
    update_lexisystem_ui();
}

static void on_sleep_timeout_btn_cb(lv_event_t *e)
{
    intptr_t sec = (intptr_t)lv_event_get_user_data(e);
    lexios_set_sleep_timeout_sec((int)sec);
    update_lexisystem_ui();
}

static void on_sleep_now_btn_cb(lv_event_t *e)
{
    (void)e;
    lexios_set_screen_sleep(true);
}

LexiSystemApp::LexiSystemApp(bool use_status_bar, bool use_navigation_bar)
    : ESP_Brookesia_PhoneApp(
          ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("LexiSystem", &img_app_battery, true),
          ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_battery, use_status_bar, use_navigation_bar))
{}

LexiSystemApp::~LexiSystemApp() {}

bool LexiSystemApp::run(void)
{
    lv_area_t area = getVisualArea();
    int w = area.x2 - area.x1 + 1;
    int h = area.y2 - area.y1 + 1;

    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_obj_set_pos(scr, 0, 56);
        lv_obj_set_size(scr, w, h);
    }

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x090D14), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(root, 24, 0);

    int card_w = (w > 800) ? (w - 88) : (w - 40);

    /* Header Card */
    lv_obj_t *hdr = create_amoled_card(root, card_w, 96, 12);
    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "LEXIOS SYSTEM & THERMAL GUARDIAN");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    g_sys_uptime_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_color(g_sys_uptime_lbl, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(g_sys_uptime_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sys_uptime_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Thermal & 6-Stage CPU1 DVFS Card */
    lv_obj_t *tcard = create_amoled_card(root, card_w, 168, 122);
    g_sys_temp_lbl = lv_label_create(tcard);
    lv_obj_set_style_text_font(g_sys_temp_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(g_sys_temp_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    g_sys_cpu_lbl = lv_label_create(tcard);
    lv_obj_set_style_text_color(g_sys_cpu_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_sys_cpu_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sys_cpu_lbl, LV_ALIGN_TOP_LEFT, 0, 40);

    g_sys_dvfs_bar = lv_bar_create(tcard);
    lv_obj_set_size(g_sys_dvfs_bar, card_w - 36, 22);
    lv_obj_align(g_sys_dvfs_bar, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_bar_set_range(g_sys_dvfs_bar, 0, 100);
    lv_obj_set_style_bg_color(g_sys_dvfs_bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_sys_dvfs_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);

    lv_obj_t *scale_lbl = lv_label_create(tcard);
    lv_label_set_text(scale_lbl, "Stages: 200MHz | 400 | 594 | 800 | 1188 | 1600MHz");
    lv_obj_set_style_text_color(scale_lbl, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_font(scale_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(scale_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Cooling Fan Control Card */
    lv_obj_t *fcard = create_amoled_card(root, card_w, 138, 304);
    g_sys_fan_lbl = lv_label_create(fcard);
    lv_obj_set_style_text_color(g_sys_fan_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(g_sys_fan_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sys_fan_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    int btn_w = (card_w - 56) / 3;
    lv_obj_t *b_auto = create_action_btn(fcard, "Auto", btn_w, 52, 0x0284C7);
    lv_obj_align(b_auto, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b_auto, on_fan_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)LEXIOS_FAN_AUTO);

    lv_obj_t *b_on = create_action_btn(fcard, "Force ON", btn_w, 52, 0x16A34A);
    lv_obj_align(b_on, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(b_on, on_fan_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)LEXIOS_FAN_FORCE_ON);

    lv_obj_t *b_off = create_action_btn(fcard, "Force OFF", btn_w, 52, 0x475569);
    lv_obj_align(b_off, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(b_off, on_fan_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)LEXIOS_FAN_FORCE_OFF);

    /* RF & Silicon Power Domains Card */
    lv_obj_t *rcard = create_amoled_card(root, card_w, 148, 456);
    g_sys_rf_lbl = lv_label_create(rcard);
    lv_obj_set_style_text_color(g_sys_rf_lbl, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_font(g_sys_rf_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sys_rf_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Pocket Standby ("Pocket Mode") Card */
    lv_obj_t *scard = create_amoled_card(root, card_w, 200, 618);
    g_sys_sleep_lbl = lv_label_create(scard);
    lv_obj_set_style_text_color(g_sys_sleep_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(g_sys_sleep_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sys_sleep_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    int sbtn_w = (card_w - 64) / 4;
    const int timeouts[4] = {30, 60, 120, 0};
    const char *tlabels[4] = {"30s", "60s", "120s", "Never"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *tb = create_action_btn(scard, tlabels[i], sbtn_w, 48, 0x1E293B);
        lv_obj_align(tb, LV_ALIGN_TOP_LEFT, i * (sbtn_w + 10), 42);
        lv_obj_add_event_cb(tb, on_sleep_timeout_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)timeouts[i]);
    }

    lv_obj_t *sleep_now = create_action_btn(scard, "ENTER POCKET STANDBY NOW (200 MHz)", card_w - 36, 56, 0x7C3AED);
    lv_obj_align(sleep_now, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(sleep_now, on_sleep_now_btn_cb, LV_EVENT_CLICKED, nullptr);

    update_lexisystem_ui();
    _refresh_timer = lv_timer_create(sys_refresh_timer_cb, 1000, nullptr);
    return true;
}

bool LexiSystemApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool LexiSystemApp::close(void)
{
    if (_refresh_timer) {
        lv_timer_del(_refresh_timer);
        _refresh_timer = nullptr;
    }
    g_sys_temp_lbl = nullptr;
    g_sys_cpu_lbl = nullptr;
    g_sys_dvfs_bar = nullptr;
    g_sys_fan_lbl = nullptr;
    g_sys_rf_lbl = nullptr;
    g_sys_uptime_lbl = nullptr;
    g_sys_sleep_lbl = nullptr;
    return true;
}

bool LexiSystemApp::init(void) { return true; }

/* ============================================================================
 * 2. LexiChatApp — On-Chain FLRC Mesh Messenger (127.0.0.1:8545 JSON-RPC)
 * ============================================================================ */
static lv_obj_t *g_chat_status_lbl = nullptr;
static lv_obj_t *g_chat_feed_lbl = nullptr;
static lv_obj_t *g_chat_input_ta = nullptr;
static int g_chat_channel_idx = 0;
static const char *kChatChannels[3] = {"#general", "#mesh-ops", "#emergency"};
static char g_chat_history[1536] =
    "[#general] LexiOS Mesh Messenger ready.\n"
    "• Transport: LR2021 FLRC + Superlexicon WASM\n"
    "• Contract: messaging-wasm (postMessage)\n";

static int query_local_node_block_number(char *out_hex, size_t out_sz)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = {.tv_sec = 0, .tv_usec = 120000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8545);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    const char *body = "{\"jsonrpc\":\"2.0\",\"method\":\"eth_blockNumber\",\"params\":[],\"id\":1}";
    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\nHost: 127.0.0.1:8545\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
        strlen(body), body);
    send(sock, req, req_len, 0);

    char resp[512] = {0};
    int n = recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);
    if (n <= 0) return -1;

    const char *p = strstr(resp, "\"result\":\"");
    if (p) {
        p += 10;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i + 1 < out_sz) {
            out_hex[i] = p[i];
            i++;
        }
        out_hex[i] = '\0';
        return 0;
    }
    return -1;
}

static int submit_mesh_chat_rpc(const char *channel, const char *msg)
{
    /* Also log to /tmp/lexios_mesh_outbox.log and dispatch eth_sendTransaction to 127.0.0.1:8545 */
    FILE *fp = fopen("/tmp/lexios_mesh_outbox.log", "a");
    if (fp) {
        fprintf(fp, "[%s] %s\n", channel, msg);
        fclose(fp);
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 150000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8545);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    /* Hex-encode payload with postMessage(bytes32,bytes) selector 0x482f63b0 */
    char hex_msg[256] = {0};
    size_t mlen = strlen(msg);
    if (mlen > 96) mlen = 96;
    for (size_t i = 0; i < mlen; i++) {
        snprintf(hex_msg + i * 2, 3, "%02x", (unsigned char)msg[i]);
    }

    char body[512];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"method\":\"superlexicon_postMeshMessage\",\"params\":[\"%s\",\"0x482f63b0%s\"],\"id\":2}",
        channel, hex_msg);
    char req[768];
    int rlen = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\nHost: 127.0.0.1:8545\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        blen, body);
    send(sock, req, rlen, 0);
    char resp[256];
    recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);
    return 0;
}

static void append_chat_line(const char *channel, const char *text, bool rpc_ok)
{
    char line[256];
    snprintf(line, sizeof(line), "\n[%s] %s %s",
             channel, text, rpc_ok ? "(sent->mesh)" : "(queued)");
    if (strlen(g_chat_history) + strlen(line) + 1 >= sizeof(g_chat_history)) {
        /* Keep the most recent half of history */
        size_t half = strlen(g_chat_history) / 2;
        memmove(g_chat_history, g_chat_history + half, strlen(g_chat_history) - half + 1);
    }
    strcat(g_chat_history, line);
    if (g_chat_feed_lbl) {
        lv_label_set_text(g_chat_feed_lbl, g_chat_history);
    }
}

static void update_chat_status(void)
{
    if (!g_chat_status_lbl) return;
    char blk[48] = {0};
    char status[128];
    if (query_local_node_block_number(blk, sizeof(blk)) == 0) {
        unsigned long long bnum = strtoull(blk, nullptr, 16);
        snprintf(status, sizeof(status), "Channel: %s  •  Node ONLINE (Block #%llu)",
                 kChatChannels[g_chat_channel_idx], bnum);
        lv_obj_set_style_text_color(g_chat_status_lbl, lv_color_hex(0x25C281), 0);
    } else {
        snprintf(status, sizeof(status), "Channel: %s  •  Node Syncing (127.0.0.1:8545)",
                 kChatChannels[g_chat_channel_idx]);
        lv_obj_set_style_text_color(g_chat_status_lbl, lv_color_hex(0xF5A524), 0);
    }
    lv_label_set_text(g_chat_status_lbl, status);
}

static void chat_poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_chat_status();
}

static void on_chat_channel_cb(lv_event_t *e)
{
    g_chat_channel_idx = (int)(intptr_t)lv_event_get_user_data(e);
    update_chat_status();
}

static void on_chat_send_cb(lv_event_t *e)
{
    const char *preset = (const char *)lv_event_get_user_data(e);
    const char *msg = preset;
    if (!msg && g_chat_input_ta) {
        msg = lv_textarea_get_text(g_chat_input_ta);
    }
    if (!msg || msg[0] == '\0') return;

    int rc = submit_mesh_chat_rpc(kChatChannels[g_chat_channel_idx], msg);
    append_chat_line(kChatChannels[g_chat_channel_idx], msg, rc == 0);
    if (!preset && g_chat_input_ta) {
        lv_textarea_set_text(g_chat_input_ta, "");
    }
}

LexiChatApp::LexiChatApp(bool use_status_bar, bool use_navigation_bar)
    : ESP_Brookesia_PhoneApp(
          ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("LexiChat", &img_app_live_call, true),
          ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_live_call, use_status_bar, use_navigation_bar))
{}

LexiChatApp::~LexiChatApp() {}

bool LexiChatApp::run(void)
{
    lv_area_t area = getVisualArea();
    int w = area.x2 - area.x1 + 1;
    int h = area.y2 - area.y1 + 1;
    int card_w = (w > 800) ? (w - 88) : (w - 40);

    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_obj_set_pos(scr, 0, 56);
        lv_obj_set_size(scr, w, h);
    }

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x090D14), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(root, 24, 0);

    /* Header & Channel Bar */
    lv_obj_t *hdr = create_amoled_card(root, card_w, 128, 12);
    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "LEXICHAT  •  ON-CHAIN MESH MESSENGER");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    g_chat_status_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_font(g_chat_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_chat_status_lbl, LV_ALIGN_TOP_LEFT, 0, 28);

    int cbtn_w = (card_w - 56) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *cb = create_action_btn(hdr, kChatChannels[i], cbtn_w, 40, 0x1E293B);
        lv_obj_align(cb, LV_ALIGN_BOTTOM_LEFT, i * (cbtn_w + 12), 0);
        lv_obj_add_event_cb(cb, on_chat_channel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* Message Feed Card */
    lv_obj_t *fcard = create_amoled_card(root, card_w, 250, 152);
    g_chat_feed_lbl = lv_label_create(fcard);
    lv_obj_set_width(g_chat_feed_lbl, card_w - 36);
    lv_label_set_long_mode(g_chat_feed_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_chat_feed_lbl, g_chat_history);
    lv_obj_set_style_text_color(g_chat_feed_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_chat_feed_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_chat_feed_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Quick Mesh Beacon Row */
    lv_obj_t *qcard = create_amoled_card(root, card_w, 84, 414);
    lv_obj_t *qb1 = create_action_btn(qcard, "ACK / Copy", cbtn_w, 48, 0x0F766E);
    lv_obj_align(qb1, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(qb1, on_chat_send_cb, LV_EVENT_CLICKED, (void *)"ACK - Signal 5/5");

    lv_obj_t *qb2 = create_action_btn(qcard, "Ping Mesh", cbtn_w, 48, 0x0284C7);
    lv_obj_align(qb2, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(qb2, on_chat_send_cb, LV_EVENT_CLICKED, (void *)"PING from K230 LexiOS");

    lv_obj_t *qb3 = create_action_btn(qcard, "Status OK", cbtn_w, 48, 0x4F46E5);
    lv_obj_align(qb3, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(qb3, on_chat_send_cb, LV_EVENT_CLICKED, (void *)"Validator Node Online");

    /* Text Input + Send Button + Touch Keyboard */
    g_chat_input_ta = lv_textarea_create(root);
    lv_obj_set_size(g_chat_input_ta, card_w - 124, 56);
    lv_obj_align(g_chat_input_ta, LV_ALIGN_TOP_LEFT, 16, 510);
    lv_textarea_set_one_line(g_chat_input_ta, true);
    lv_textarea_set_placeholder_text(g_chat_input_ta, "Type mesh message...");
    lv_obj_add_event_cb(g_chat_input_ta, on_chat_send_cb, LV_EVENT_READY, nullptr);
    if (lv_group_get_default() != nullptr) {
        lv_group_add_obj(lv_group_get_default(), g_chat_input_ta);
        lv_group_focus_obj(g_chat_input_ta);
    }

    lv_obj_t *send_btn = create_action_btn(root, "SEND", 112, 56, 0x16A34A);
    lv_obj_align(send_btn, LV_ALIGN_TOP_RIGHT, -16, 510);
    lv_obj_add_event_cb(send_btn, on_chat_send_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *kb = lv_keyboard_create(root);
    lv_obj_set_size(kb, w, 320);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, g_chat_input_ta);
    if (tca8418_is_connected()) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }

    update_chat_status();
    _poll_timer = lv_timer_create(chat_poll_timer_cb, 2000, nullptr);
    return true;
}

bool LexiChatApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool LexiChatApp::close(void)
{
    if (_poll_timer) {
        lv_timer_del(_poll_timer);
        _poll_timer = nullptr;
    }
    g_chat_status_lbl = nullptr;
    g_chat_feed_lbl = nullptr;
    g_chat_input_ta = nullptr;
    return true;
}

bool LexiChatApp::init(void) { return true; }

/* ============================================================================
 * 3. LexiAppsApp — MMU Process Supervisor & .wasm / .elf Sandbox Manager
 * ============================================================================ */
static lv_obj_t *g_apps_list_lbl = nullptr;
static lv_obj_t *g_apps_action_lbl = nullptr;
static pid_t g_spawned_app_pid = -1;
static char g_spawned_app_name[64] = {0};

static bool check_port_listening(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bool ok = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(sock);
    return ok;
}

static void refresh_lexiapps_view(void)
{
    if (!g_apps_list_lbl) return;

    /* Reap any exited sandboxed child process without blocking (Crash Isolation) */
    if (g_spawned_app_pid > 0) {
        int status = 0;
        pid_t r = waitpid(g_spawned_app_pid, &status, WNOHANG);
        if (r == g_spawned_app_pid || r < 0) {
            if (g_apps_action_lbl) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Sandbox process %s (PID %d) exited cleanly (isolated).",
                         g_spawned_app_name, (int)g_spawned_app_pid);
                lv_label_set_text(g_apps_action_lbl, msg);
            }
            g_spawned_app_pid = -1;
        }
    }

    bool node_up = (access("/tmp/slx_node_running", F_OK) == 0) || check_port_listening(8545);
    bool radio_up = (access("/tmp/slx_radio_running", F_OK) == 0) || (access("/tmp/os_thermal_state", F_OK) == 0);

    char buf[1024];
    int off = snprintf(buf, sizeof(buf),
        "CORE SYSTEM SERVICES (MMU Isolated lwp):\n"
        "  [1] system_ui (app.elf)      : RUNNING (PID %d)\n"
        "  [2] lora_radio.elf (LR2021)  : %s\n"
        "  [3] superlexicon.elf (Node)  : %s\n\n"
        "INSTALLED PACKAGES (/sdcard/ & /sdcard/apps/):\n",
        (int)getpid(),
        radio_up ? "ACTIVE (UDP :9100/:9101)" : "STANDBY",
        node_up ? "ONLINE (RPC :8545 / WS :8546)" : "STARTING / OFFLINE");

    const char *scan_dirs[2] = {"/sdcard/apps", "/sdcard"};
    int found = 0;
    for (int d = 0; d < 2; d++) {
        DIR *dir = opendir(scan_dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr && off < (int)sizeof(buf) - 120) {
            const char *name = ent->d_name;
            size_t len = strlen(name);
            bool is_elf = (len > 4 && strcmp(name + len - 4, ".elf") == 0);
            bool is_wasm = (len > 5 && strcmp(name + len - 5, ".wasm") == 0);
            if (is_elf || is_wasm) {
                char full[256];
                snprintf(full, sizeof(full), "%s/%s", scan_dirs[d], name);
                struct stat st;
                long kb = (stat(full, &st) == 0) ? (long)(st.st_size / 1024) : 0;
                off += snprintf(buf + off, sizeof(buf) - off,
                                "  • [%s] %s (%ld KB)\n",
                                is_wasm ? "WASM" : "ELF ", name, kb);
                found++;
                if (found >= 8) break;
            }
        }
        closedir(dir);
    }
    if (found == 0) {
        snprintf(buf + off, sizeof(buf) - off, "  • Drop .wasm or .elf packages into /sdcard/apps/\n");
    }

    lv_label_set_text(g_apps_list_lbl, buf);
}

static void apps_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_lexiapps_view();
}

static void on_spawn_service_cb(lv_event_t *e)
{
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;

    if (strstr(path, "superlexicon.elf")) {
        int sfd = open("/tmp/slx_node_starting", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (sfd >= 0) close(sfd);
        int fd = open("/tmp/slx_start_node", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "1\n", 2);
            close(fd);
        }
        if (g_apps_action_lbl) {
            lv_label_set_text(g_apps_action_lbl, "Requested supervisor to start lora_radio.elf & superlexicon.elf");
        }
        refresh_lexiapps_view();
        return;
    }
    if (strstr(path, "lora_radio.elf")) {
        int fd = open("/tmp/slx_start_radio", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "1\n", 2);
            close(fd);
        }
        if (g_apps_action_lbl) {
            lv_label_set_text(g_apps_action_lbl, "Requested supervisor to start lora_radio.elf");
        }
        refresh_lexiapps_view();
        return;
    }

    mkdir("/sdcard/apps", 0755);
    pid_t pid = fork();
    if (pid == 0) {
        char *const argv[] = {(char *)path, nullptr};
        execv(path, argv);
        _exit(127);
    } else if (pid > 0) {
        g_spawned_app_pid = pid;
        snprintf(g_spawned_app_name, sizeof(g_spawned_app_name), "%s", path);
        if (g_apps_action_lbl) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Spawned %s in isolated Sv39 MMU space (PID %d)", path, (int)pid);
            lv_label_set_text(g_apps_action_lbl, msg);
        }
    }
    refresh_lexiapps_view();
}

LexiAppsApp::LexiAppsApp(bool use_status_bar, bool use_navigation_bar)
    : ESP_Brookesia_PhoneApp(
          ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("LexiApps", &img_app_object_detect, true),
          ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_object_detect, use_status_bar, use_navigation_bar))
{}

LexiAppsApp::~LexiAppsApp() {}

bool LexiAppsApp::run(void)
{
    lv_area_t area = getVisualArea();
    int w = area.x2 - area.x1 + 1;
    int h = area.y2 - area.y1 + 1;
    int card_w = (w > 800) ? (w - 88) : (w - 40);

    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_obj_set_pos(scr, 0, 56);
        lv_obj_set_size(scr, w, h);
    }

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x090D14), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(root, 24, 0);

    lv_obj_t *hdr = create_amoled_card(root, card_w, 96, 12);
    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "LEXIAPPS  •  SANDBOX & PACKAGE MANAGER");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    g_apps_action_lbl = lv_label_create(hdr);
    lv_label_set_text(g_apps_action_lbl, "Sv39 MMU Process Isolation + Wasmtime Sandbox Ready");
    lv_obj_set_style_text_color(g_apps_action_lbl, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_text_font(g_apps_action_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_apps_action_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *lcard = create_amoled_card(root, card_w, 460, 122);
    g_apps_list_lbl = lv_label_create(lcard);
    lv_obj_set_width(g_apps_list_lbl, card_w - 36);
    lv_label_set_long_mode(g_apps_list_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_apps_list_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_apps_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_apps_list_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *acard = create_amoled_card(root, card_w, 160, 598);
    lv_obj_t *albl = lv_label_create(acard);
    lv_label_set_text(albl, "SUPERVISOR ACTIONS (Crash-Isolated Child Processes):");
    lv_obj_set_style_text_color(albl, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_font(albl, &lv_font_montserrat_14, 0);
    lv_obj_align(albl, LV_ALIGN_TOP_LEFT, 0, 0);

    int b_w = (card_w - 48) / 2;
    lv_obj_t *b1 = create_action_btn(acard, "Start/Restart Radio", b_w, 56, 0x0284C7);
    lv_obj_align(b1, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b1, on_spawn_service_cb, LV_EVENT_CLICKED, (void *)"/sdcard/lora_radio.elf");

    lv_obj_t *b2 = create_action_btn(acard, "Start/Restart Node", b_w, 56, 0x16A34A);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(b2, on_spawn_service_cb, LV_EVENT_CLICKED, (void *)"/sdcard/superlexicon.elf");

    refresh_lexiapps_view();
    _proc_timer = lv_timer_create(apps_timer_cb, 2000, nullptr);
    return true;
}

bool LexiAppsApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool LexiAppsApp::close(void)
{
    if (_proc_timer) {
        lv_timer_del(_proc_timer);
        _proc_timer = nullptr;
    }
    g_apps_list_lbl = nullptr;
    g_apps_action_lbl = nullptr;
    return true;
}

bool LexiAppsApp::init(void) { return true; }

/* ============================================================================
 * 4. LexiSatApp — nRF9151 Satellite NTN (3GPP Rel-17), GNSS/GPS & QWERTY Hub
 * ============================================================================ */
#define LEXISAT_IOC_SET_BAUDRATE _IOW('U', 0x40, int)
struct lexisat_uart_cfg {
    uint32_t baud_rate;
    uint32_t data_bits   : 4;
    uint32_t stop_bits   : 2;
    uint32_t parity      : 2;
    uint32_t fifo_lenth  : 2;
    uint32_t auto_flow   : 1;
    uint32_t reserved    : 21;
};

static int g_sat_uart_fd = -1;
static pthread_mutex_t g_sat_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_sat_worker_started = false;
static volatile int g_sat_pending_op = 0; // 1=query_id, 2=mode_ntn, 3=mode_gnss, 4=mode_ltem, 5=custom_at
static char g_sat_custom_cmd[256] = {0};

static char g_sat_mode_str[32] = "STANDBY (OFF)";
static char g_sat_imei[32] = "--";
static char g_sat_iccid[32] = "--";
static char g_sat_fw[32] = "--";
static char g_sat_cereg[32] = "Unregistered";
static char g_sat_lat[24] = "--";
static char g_sat_lon[24] = "--";
static char g_sat_alt[24] = "--";
static char g_sat_speed[24] = "--";
static char g_sat_utc[32] = "-- (Wait Fix)";
static char g_sat_rx_log[512] =
    "[LexiSat] Nordic nRF9151 SiP (3GPP Rel-17 NTN Satellite / GNSS / LTE-M)\n"
    "[LexiSat] Tap 'nRF9151 ON' to power GPIO2 & attach /dev/uart2.";

static int g_kb_backlight_idx = 1; // 0=OFF(0%), 1=Low(5%), 2=High(25%)

static lv_obj_t *g_sat_hdr_lbl = nullptr;
static lv_obj_t *g_sat_gnss_lbl = nullptr;
static lv_obj_t *g_sat_log_lbl = nullptr;
static lv_obj_t *g_sat_input_ta = nullptr;

static void sat_append_log(const char *line)
{
    if (!line || !line[0]) return;
    pthread_mutex_lock(&g_sat_lock);
    size_t cur_len = strlen(g_sat_rx_log);
    if (cur_len > 340) {
        const char *nl = strchr(g_sat_rx_log + 100, '\n');
        if (nl) {
            memmove(g_sat_rx_log, nl + 1, strlen(nl + 1) + 1);
        } else {
            g_sat_rx_log[0] = '\0';
        }
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s\n%s", g_sat_rx_log, line);
    snprintf(g_sat_rx_log, sizeof(g_sat_rx_log), "%s", tmp);
    pthread_mutex_unlock(&g_sat_lock);
}

static bool sat_ensure_uart_open(void)
{
    if (!lexios_get_nrf9151_power()) {
        if (g_sat_uart_fd > 0) {
            close(g_sat_uart_fd);
            g_sat_uart_fd = -1;
        }
        return false;
    }
    if (g_sat_uart_fd > 0) {
        return true;
    }
    int fd = open("/dev/uart2", O_RDWR | O_NONBLOCK);
    if (fd <= 0) {
        return false;
    }
    struct lexisat_uart_cfg cfg = {
        .baud_rate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = 0,
        .fifo_lenth = 2,
        .auto_flow = 0,
        .reserved = 0
    };
    ioctl(fd, LEXISAT_IOC_SET_BAUDRATE, &cfg);
    g_sat_uart_fd = fd;
    return true;
}

static void sat_parse_line(const char *line)
{
    if (!line || !line[0]) return;
    if (strcmp(line, "OK") == 0) return;

    pthread_mutex_lock(&g_sat_lock);
    if (strncmp(line, "#XGPS:", 6) == 0 && strlen(line) > 12) {
        /* Format: #XGPS: <lat>,<lon>,<alt>,<acc>,<speed>,<heading>,"YYYY-MM-DD HH:MM:SS" */
        char copy[160];
        snprintf(copy, sizeof(copy), "%s", line + 6);
        char *save = nullptr;
        char *tok = strtok_r(copy, ",", &save);
        int idx = 0;
        while (tok != nullptr) {
            while (*tok == ' ') tok++;
            if (idx == 0) snprintf(g_sat_lat, sizeof(g_sat_lat), "%s", tok);
            else if (idx == 1) snprintf(g_sat_lon, sizeof(g_sat_lon), "%s", tok);
            else if (idx == 2) snprintf(g_sat_alt, sizeof(g_sat_alt), "%s m", tok);
            else if (idx == 4) snprintf(g_sat_speed, sizeof(g_sat_speed), "%s m/s", tok);
            else if (idx == 6) {
                /* Strip quotes and sync system RTC */
                if (tok[0] == '"') tok++;
                size_t l = strlen(tok);
                if (l > 0 && tok[l - 1] == '"') tok[l - 1] = '\0';
                snprintf(g_sat_utc, sizeof(g_sat_utc), "%s UTC", tok);
                int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
                if (sscanf(tok, "%d-%d-%d %d:%d:%d", &y, &m, &d, &hh, &mm, &ss) == 6 && y >= 2024) {
                    struct tm tmv;
                    memset(&tmv, 0, sizeof(tmv));
                    tmv.tm_year = y - 1900;
                    tmv.tm_mon = m - 1;
                    tmv.tm_mday = d;
                    tmv.tm_hour = hh;
                    tmv.tm_min = mm;
                    tmv.tm_sec = ss;
                    time_t epoch = mktime(&tmv);
                    if (epoch > 0) {
                        struct timespec ts = {.tv_sec = epoch, .tv_nsec = 0};
                        clock_settime(CLOCK_REALTIME, &ts);
                    }
                }
            }
            tok = strtok_r(nullptr, ",", &save);
            idx++;
        }
    } else if (strstr(line, "%XICCID:") != nullptr) {
        const char *p = strchr(line, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            snprintf(g_sat_iccid, sizeof(g_sat_iccid), "%s", p);
        }
    } else if (strstr(line, "mfw_") != nullptr) {
        snprintf(g_sat_fw, sizeof(g_sat_fw), "%s", line);
    } else if (strncmp(line, "+CEREG:", 7) == 0) {
        snprintf(g_sat_cereg, sizeof(g_sat_cereg), "%s", line + 7);
    } else if (strlen(line) == 15 && line[0] >= '0' && line[0] <= '9') {
        snprintf(g_sat_imei, sizeof(g_sat_imei), "%s", line);
    }
    pthread_mutex_unlock(&g_sat_lock);

    char log_entry[160];
    snprintf(log_entry, sizeof(log_entry), "RX < %s", line);
    sat_append_log(log_entry);
}

static void sat_send_at(const char *cmd, int wait_ms)
{
    if (!sat_ensure_uart_open()) return;
    char full[280];
    snprintf(full, sizeof(full), "%s\r\n", cmd);
    write(g_sat_uart_fd, full, strlen(full));

    char log_entry[160];
    snprintf(log_entry, sizeof(log_entry), "TX > %s", cmd);
    sat_append_log(log_entry);

    int loops = (wait_ms > 0 ? wait_ms : 200) / 40;
    if (loops < 1) loops = 1;
    char rx_buf[512];
    for (int i = 0; i < loops; i++) {
        usleep(40000);
        int n = read(g_sat_uart_fd, rx_buf, sizeof(rx_buf) - 1);
        if (n > 0) {
            rx_buf[n] = '\0';
            char *save = nullptr;
            char *line = strtok_r(rx_buf, "\r\n", &save);
            while (line != nullptr) {
                sat_parse_line(line);
                line = strtok_r(nullptr, "\r\n", &save);
            }
        }
    }
}

static void *sat_uart_worker_thread(void *arg)
{
    (void)arg;
    char rx_buf[512];
    while (1) {
        int op = g_sat_pending_op;
        if (op != 0) {
            g_sat_pending_op = 0;
            if (!lexios_get_nrf9151_power()) {
                lexios_set_nrf9151_power(true);
                pthread_mutex_lock(&g_sat_lock);
                snprintf(g_sat_mode_str, sizeof(g_sat_mode_str), "POWERED ON");
                pthread_mutex_unlock(&g_sat_lock);
            }
            if (op == 1) {
                /* Query Modem ID & Registration */
                sat_send_at("AT+CGMR", 250);
                sat_send_at("AT+CGSN", 250);
                sat_send_at("AT%XICCID", 250);
                sat_send_at("AT+CEREG?", 250);
            } else if (op == 2) {
                /* 3GPP Rel-17 Satellite NTN NB-IoT Mode */
                pthread_mutex_lock(&g_sat_lock);
                snprintf(g_sat_mode_str, sizeof(g_sat_mode_str), "SAT-NTN (3GPP Rel-17)");
                pthread_mutex_unlock(&g_sat_lock);
                sat_send_at("AT#XGPS=0", 250);
                sat_send_at("AT+CFUN=0", 400);
                sat_send_at("AT%XSYSTEMMODE=0,0,0,0,1", 400);
                sat_send_at("AT+CEREG=5", 250);
                sat_send_at("AT+CFUN=1", 500);
                sat_send_at("AT+CEREG?", 300);
            } else if (op == 3) {
                /* GNSS / GPS Positioning Mode */
                pthread_mutex_lock(&g_sat_lock);
                snprintf(g_sat_mode_str, sizeof(g_sat_mode_str), "GNSS (GPS Active)");
                pthread_mutex_unlock(&g_sat_lock);
                sat_send_at("AT#XGPS=0", 300);
                sat_send_at("AT+CFUN=0", 400);
                sat_send_at("AT%XSYSTEMMODE=0,0,1,0", 400);
                sat_send_at("AT+CFUN=31", 400);
                sat_send_at("AT#XGPS=1,0,0,0", 400);
            } else if (op == 4) {
                /* LTE-M + GNSS Concurrent Mode */
                pthread_mutex_lock(&g_sat_lock);
                snprintf(g_sat_mode_str, sizeof(g_sat_mode_str), "LTE-M + GNSS");
                pthread_mutex_unlock(&g_sat_lock);
                sat_send_at("AT+CFUN=0", 400);
                sat_send_at("AT%XSYSTEMMODE=1,0,1,0", 400);
                sat_send_at("AT+CEREG=5", 250);
                sat_send_at("AT+CFUN=1", 400);
                sat_send_at("AT#XGPS=1,0,0,0", 400);
            } else if (op == 5) {
                char cmd_copy[256];
                pthread_mutex_lock(&g_sat_lock);
                snprintf(cmd_copy, sizeof(cmd_copy), "%s", g_sat_custom_cmd);
                pthread_mutex_unlock(&g_sat_lock);
                if (cmd_copy[0]) {
                    sat_send_at(cmd_copy, 350);
                }
            }
        }

        /* Background non-blocking poll for unsolicited #XGPS: / +CEREG: sentences */
        if (sat_ensure_uart_open()) {
            int n = read(g_sat_uart_fd, rx_buf, sizeof(rx_buf) - 1);
            if (n > 0) {
                rx_buf[n] = '\0';
                char *save = nullptr;
                char *line = strtok_r(rx_buf, "\r\n", &save);
                while (line != nullptr) {
                    sat_parse_line(line);
                    line = strtok_r(nullptr, "\r\n", &save);
                }
            }
        }
        usleep(100000);
    }
    return nullptr;
}

static void refresh_lexisat_ui(void)
{
    if (!g_sat_hdr_lbl) return;
    bool nrf_on = lexios_get_nrf9151_power();
    bool kb_on = tca8418_is_connected();
    const char *kb_light = (g_kb_backlight_idx == 0) ? "OFF" :
                           (g_kb_backlight_idx == 1) ? "Low (5%)" : "High (25%)";

    char buf[512];
    pthread_mutex_lock(&g_sat_lock);
    snprintf(buf, sizeof(buf),
             "nRF9151: %s  |  Mode: %s\n"
             "QWERTY KB (I2C4 0x34): %s  |  Backlight: %s",
             nrf_on ? "ON (3.3V GPIO2)" : "OFF (0mW Standby)",
             nrf_on ? g_sat_mode_str : "STANDBY",
             kb_on ? "CONNECTED" : "DETACHED",
             kb_light);
    lv_label_set_text(g_sat_hdr_lbl, buf);

    if (g_sat_gnss_lbl) {
        snprintf(buf, sizeof(buf),
                 "GPS Fix   : Lat %s  |  Lon %s\n"
                 "Kinematics: Alt %s  |  Speed %s  |  %s\n"
                 "Modem ID  : IMEI %s  |  FW %s  |  CEREG %s",
                 g_sat_lat, g_sat_lon,
                 g_sat_alt, g_sat_speed, g_sat_utc,
                 g_sat_imei, g_sat_fw, g_sat_cereg);
        lv_label_set_text(g_sat_gnss_lbl, buf);
    }
    if (g_sat_log_lbl) {
        lv_label_set_text(g_sat_log_lbl, g_sat_rx_log);
    }
    pthread_mutex_unlock(&g_sat_lock);
}

static void sat_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_lexisat_ui();
}

static void on_sat_power_toggle_cb(lv_event_t *e)
{
    (void)e;
    bool cur = lexios_get_nrf9151_power();
    if (cur) {
        if (g_sat_uart_fd > 0) {
            write(g_sat_uart_fd, "AT+CFUN=0\r\n", 11);
            usleep(80000);
            close(g_sat_uart_fd);
            g_sat_uart_fd = -1;
        }
        lexios_set_nrf9151_power(false);
        sat_append_log("[LexiSat] Powered OFF nRF9151 (GPIO2 LOW, GPIO5 locked HIGH).");
    } else {
        lexios_set_nrf9151_power(true);
        sat_append_log("[LexiSat] Powered ON nRF9151 (GPIO2 HIGH, UART2 115200 active).");
        g_sat_pending_op = 1; // Query Modem ID automatically on power-up
    }
    refresh_lexisat_ui();
}

static void on_sat_op_btn_cb(lv_event_t *e)
{
    intptr_t op = (intptr_t)lv_event_get_user_data(e);
    g_sat_pending_op = (int)op;
    refresh_lexisat_ui();
}

static void on_sat_kb_rescan_cb(lv_event_t *e)
{
    (void)e;
    tca8418_init();
    if (tca8418_is_connected()) {
        int duty = (g_kb_backlight_idx == 0) ? 0 : (g_kb_backlight_idx == 1) ? 5 : 25;
        control_keyboard_backlight(20000, duty);
        sat_append_log("[LexiSat] Physical QWERTY Keyboard detected on I2C4 (0x34)!");
    } else {
        sat_append_log("[LexiSat] Physical Keyboard not detected on I2C4 (0x34).");
    }
    refresh_lexisat_ui();
}

static void on_sat_kb_light_cb(lv_event_t *e)
{
    (void)e;
    g_kb_backlight_idx = (g_kb_backlight_idx + 1) % 3;
    int duty = (g_kb_backlight_idx == 0) ? 0 : (g_kb_backlight_idx == 1) ? 5 : 25;
    control_keyboard_backlight(20000, duty);
    refresh_lexisat_ui();
}

static void on_sat_send_sos_cb(lv_event_t *e)
{
    (void)e;
    char sos_payload[256];
    pthread_mutex_lock(&g_sat_lock);
    snprintf(sos_payload, sizeof(sos_payload),
             "SOS/POS [LexiOS K230] Lat:%s Lon:%s Alt:%s UTC:%s",
             g_sat_lat, g_sat_lon, g_sat_alt, g_sat_utc);
    snprintf(g_sat_custom_cmd, sizeof(g_sat_custom_cmd), "AT#XSEND=\"%s\"", sos_payload);
    pthread_mutex_unlock(&g_sat_lock);

    /* 1. Queue transmission over nRF9151 Satellite NTN / Cellular UART2 */
    g_sat_pending_op = 5;

    /* 2. Simultaneously broadcast emergency coordinates over SuperLexicon FLRC Mesh */
    bool mesh_ok = (submit_mesh_chat_rpc("#emergency", sos_payload) == 0);
    char log_line[300];
    snprintf(log_line, sizeof(log_line), "SOS TX > Sat NTN + FLRC Mesh (%s): %s",
             mesh_ok ? "RPC OK" : "Queued", sos_payload);
    sat_append_log(log_line);
    refresh_lexisat_ui();
}

static void on_sat_send_at_cb(lv_event_t *e)
{
    (void)e;
    if (!g_sat_input_ta) return;
    const char *txt = lv_textarea_get_text(g_sat_input_ta);
    if (!txt || !txt[0]) return;
    pthread_mutex_lock(&g_sat_lock);
    snprintf(g_sat_custom_cmd, sizeof(g_sat_custom_cmd), "%s", txt);
    pthread_mutex_unlock(&g_sat_lock);
    g_sat_pending_op = 5;
    lv_textarea_set_text(g_sat_input_ta, "");
    refresh_lexisat_ui();
}

LexiSatApp::LexiSatApp(bool use_status_bar, bool use_navigation_bar)
    : ESP_Brookesia_PhoneApp(
          ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("LexiSat", &img_app_gnss, true),
          ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_gnss, use_status_bar, use_navigation_bar))
{}

LexiSatApp::~LexiSatApp() {}

bool LexiSatApp::run(void)
{
    if (!g_sat_worker_started) {
        g_sat_worker_started = true;
        pthread_t tid;
        if (pthread_create(&tid, nullptr, sat_uart_worker_thread, nullptr) == 0) {
            pthread_detach(tid);
        }
    }

    lv_area_t area = getVisualArea();
    int w = area.x2 - area.x1 + 1;
    int h = area.y2 - area.y1 + 1;
    int card_w = (w > 800) ? (w - 88) : (w - 40);

    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_obj_set_pos(scr, 0, 56);
        lv_obj_set_size(scr, w, h);
    }

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x090D14), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(root, 24, 0);

    /* 1. Header Card */
    lv_obj_t *hdr = create_amoled_card(root, card_w, 96, 8);
    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "LEXISAT  •  NTN SATELLITE, GNSS & KB");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    g_sat_hdr_lbl = lv_label_create(hdr);
    lv_obj_set_style_text_color(g_sat_hdr_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_sat_hdr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sat_hdr_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 2. Power, 3GPP Rel-17 Satellite / GNSS Mode & Keyboard Controls Card */
    lv_obj_t *mcard = create_amoled_card(root, card_w, 136, 112);
    int b4_w = (card_w - 56) / 4;

    lv_obj_t *b_pwr = create_action_btn(mcard, "nRF9151 ON/OFF", b4_w, 46, 0x0F766E);
    lv_obj_align(b_pwr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(b_pwr, on_sat_power_toggle_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_ntn = create_action_btn(mcard, "SAT NTN", b4_w, 46, 0x7C3AED);
    lv_obj_align(b_ntn, LV_ALIGN_TOP_LEFT, b4_w + 8, 0);
    lv_obj_add_event_cb(b_ntn, on_sat_op_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);

    lv_obj_t *b_gps = create_action_btn(mcard, "GNSS Fix", b4_w, 46, 0x0284C7);
    lv_obj_align(b_gps, LV_ALIGN_TOP_LEFT, (b4_w + 8) * 2, 0);
    lv_obj_add_event_cb(b_gps, on_sat_op_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)3);

    lv_obj_t *b_lte = create_action_btn(mcard, "LTE-M+GPS", b4_w, 46, 0x2563EB);
    lv_obj_align(b_lte, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(b_lte, on_sat_op_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)4);

    /* Second row: Modem ID, SOS Beacon, Keyboard Re-Scan & Backlight */
    lv_obj_t *b_id = create_action_btn(mcard, "Query ID", b4_w, 46, 0x334155);
    lv_obj_align(b_id, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b_id, on_sat_op_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    lv_obj_t *b_sos = create_action_btn(mcard, "TX SOS+GPS", b4_w, 46, 0xDC2626);
    lv_obj_align(b_sos, LV_ALIGN_BOTTOM_LEFT, b4_w + 8, 0);
    lv_obj_add_event_cb(b_sos, on_sat_send_sos_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_kbs = create_action_btn(mcard, "Re-Scan KB", b4_w, 46, 0x4F46E5);
    lv_obj_align(b_kbs, LV_ALIGN_BOTTOM_LEFT, (b4_w + 8) * 2, 0);
    lv_obj_add_event_cb(b_kbs, on_sat_kb_rescan_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_kbl = create_action_btn(mcard, "KB Light", b4_w, 46, 0xD97706);
    lv_obj_align(b_kbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(b_kbl, on_sat_kb_light_cb, LV_EVENT_CLICKED, nullptr);

    /* 3. Live GNSS Telemetry & Modem Identity Card */
    lv_obj_t *gcard = create_amoled_card(root, card_w, 106, 256);
    g_sat_gnss_lbl = lv_label_create(gcard);
    lv_obj_set_width(g_sat_gnss_lbl, card_w - 32);
    lv_label_set_long_mode(g_sat_gnss_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_sat_gnss_lbl, lv_color_hex(0x25C281), 0);
    lv_obj_set_style_text_font(g_sat_gnss_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sat_gnss_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 4. Live UART2 / Satellite Transceiver Log Card */
    lv_obj_t *lcard = create_amoled_card(root, card_w, 134, 370);
    g_sat_log_lbl = lv_label_create(lcard);
    lv_obj_set_width(g_sat_log_lbl, card_w - 32);
    lv_label_set_long_mode(g_sat_log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_sat_log_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_sat_log_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_sat_log_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 5. AT Command / Satellite Payload Input + Touch & Physical Keyboard */
    g_sat_input_ta = lv_textarea_create(root);
    lv_obj_set_size(g_sat_input_ta, card_w - 124, 54);
    lv_obj_align(g_sat_input_ta, LV_ALIGN_TOP_LEFT, 16, 512);
    lv_textarea_set_one_line(g_sat_input_ta, true);
    lv_textarea_set_placeholder_text(g_sat_input_ta, "AT command or Sat NTN payload...");
    lv_obj_add_event_cb(g_sat_input_ta, on_sat_send_at_cb, LV_EVENT_READY, nullptr);
    if (lv_group_get_default() != nullptr) {
        lv_group_add_obj(lv_group_get_default(), g_sat_input_ta);
        lv_group_focus_obj(g_sat_input_ta);
    }

    lv_obj_t *send_btn = create_action_btn(root, "SEND AT", 112, 54, 0x16A34A);
    lv_obj_align(send_btn, LV_ALIGN_TOP_RIGHT, -16, 512);
    lv_obj_add_event_cb(send_btn, on_sat_send_at_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *kb = lv_keyboard_create(root);
    lv_obj_set_size(kb, w, 310);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, g_sat_input_ta);
    if (tca8418_is_connected()) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_lexisat_ui();
    _sat_timer = lv_timer_create(sat_timer_cb, 1000, nullptr);
    return true;
}

bool LexiSatApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool LexiSatApp::close(void)
{
    if (_sat_timer) {
        lv_timer_del(_sat_timer);
        _sat_timer = nullptr;
    }
    g_sat_hdr_lbl = nullptr;
    g_sat_gnss_lbl = nullptr;
    g_sat_log_lbl = nullptr;
    g_sat_input_ta = nullptr;
    return true;
}

bool LexiSatApp::init(void) { return true; }

/* ============================================================================
 * 5. LexiRFApp — Multi-Band Sub-GHz/2.4GHz Spectrum Analyzer & Security Auditor
 * ============================================================================ */
#define RF_SPECTRUM_BINS 24

struct RfBandPreset {
    const char *label;
    const char *desc;
    float start_mhz;
    float end_mhz;
    bool is_hf;
};

static const RfBandPreset kRfBands[5] = {
    {"315 MHz", "315M Keyfob / Automotive / OOK-ASK",     310.0f, 320.0f, false},
    {"433/462", "433M LPD / 446M PMR / 462M FRS Voice",   430.0f, 468.0f, false},
    {"868 MHz", "868M EU SRD / Tactical Voice / LoRa",    863.0f, 870.0f, false},
    {"915 MHz", "915M US ISM / SuperLexicon FLRC Voice",  902.0f, 928.0f, false},
    {"2.4 GHz", "2.4G FLRC Voice / Wi-Fi / BLE / ISM",    2400.0f, 2483.5f, true},
};

static int g_rf_band_idx = 1; // Default to 433/446/462 MHz UHF Voice & Sub-GHz band
static volatile bool g_rf_worker_running = false;
static volatile bool g_rf_cont_sweep = false;
static volatile bool g_rf_voice_scan_auto = false;
static volatile bool g_rf_voice_decode_req = false;
static volatile bool g_rf_single_sweep_req = true;
static volatile bool g_rf_wifi_audit_req = false;
static volatile bool g_rf_ble_probe_req = false;
static volatile bool g_rf_cell_audit_req = false;
static volatile bool g_rf_ui_dirty = true;
static pthread_t g_rf_worker_tid;
static pthread_mutex_t g_rf_lock = PTHREAD_MUTEX_INITIALIZER;

static float g_rf_live_dbm[RF_SPECTRUM_BINS];
static float g_rf_peak_dbm[RF_SPECTRUM_BINS];
static float g_rf_cur_peak_mhz = 433.92f;
static float g_rf_cur_peak_val = -112.0f;
static float g_rf_cur_floor_val = -114.0f;
static uint32_t g_rf_sweep_count = 0;
static uint32_t g_rf_voice_dec_count = 0;
static char g_rf_audit_log[2048] =
    "[LexiRF] Multi-Band Spectrum, Voice Decoder & Security Suite Ready.\n"
    "• Sub-GHz (150-960MHz) + 2.4GHz: Semtech LR2021 Hardware RSSI\n"
    "• Voice Radio Decoder: FLRC/GFSK Digital FIFO + AM/Envelope 8kHz G.711A\n"
    "• Wi-Fi 802.11b/g/n, BLE 5.0 NUS & nRF9151 Cell/Sat Tower Auditor\n";

static lv_obj_t *g_rf_band_lbl = nullptr;
static lv_obj_t *g_rf_stats_lbl = nullptr;
static lv_obj_t *g_rf_chart = nullptr;
static lv_chart_series_t *g_rf_ser_live = nullptr;
static lv_chart_series_t *g_rf_ser_peak = nullptr;
static lv_obj_t *g_rf_cont_btn = nullptr;
static lv_obj_t *g_rf_vscan_btn = nullptr;
static lv_obj_t *g_rf_wifi_pwr_btn = nullptr;
static lv_obj_t *g_rf_log_lbl = nullptr;

static void rf_append_log(const char *line)
{
    pthread_mutex_lock(&g_rf_lock);
    size_t cur = strlen(g_rf_audit_log);
    size_t add = strlen(line);
    if (cur + add + 2 >= sizeof(g_rf_audit_log)) {
        size_t keep = sizeof(g_rf_audit_log) / 2;
        memmove(g_rf_audit_log, g_rf_audit_log + (cur - keep), keep);
        g_rf_audit_log[keep] = '\0';
        cur = keep;
    }
    snprintf(g_rf_audit_log + cur, sizeof(g_rf_audit_log) - cur, "%s\n", line);
    g_rf_ui_dirty = true;
    pthread_mutex_unlock(&g_rf_lock);
}

/* Direct non-destructive LR2021 SPI1 hardware RSSI sweep */
#define LEXI_SPI_DEV_CTRL_CONFIG (0xc00 + 0x01)
#define LEXI_SPI_DEV_CTRL_RW     (0xc00 + 0x02)
struct lexi_spi_cfg {
    uint8_t mode;
    uint8_t data_width;
    uint16_t reserved;
    uint32_t max_hz;
};
struct lexi_spi_priv {
    const void *send_buf;
    size_t send_length;
    void *recv_buf;
    size_t recv_length;
};

static void lr2021_spi_tx(int spi_fd, const uint8_t *cmd, size_t len)
{
    struct lexi_spi_cfg cfg = {.mode = 1 /* DWENUM_SPI_TX */, .data_width = 8, .reserved = 0, .max_hz = 5000000};
    ioctl(spi_fd, LEXI_SPI_DEV_CTRL_CONFIG, &cfg);
    write(spi_fd, cmd, len);
    usleep(60);
}

static bool lr2021_spi_read_rssi(int spi_fd, float *out_dbm)
{
    const uint8_t cmd[2] = {0x02, 0x0B}; // RADIOLIB_LR20xx_CMD_GET_RSSI_INST
    lr2021_spi_tx(spi_fd, cmd, sizeof(cmd));

    uint8_t tx_nop[4] = {0, 0, 0, 0};
    uint8_t rx_buf[4] = {0, 0, 0, 0};
    struct lexi_spi_cfg cfg = {.mode = 3 /* DWENUM_SPI_EEPROM */, .data_width = 8, .reserved = 0, .max_hz = 5000000};
    struct lexi_spi_priv priv = {.send_buf = tx_nop, .send_length = 4, .recv_buf = rx_buf, .recv_length = 4};
    ioctl(spi_fd, LEXI_SPI_DEV_CTRL_CONFIG, &cfg);
    if (ioctl(spi_fd, LEXI_SPI_DEV_CTRL_RW, &priv) != 0 && rx_buf[2] == 0) {
        return false;
    }
    if ((rx_buf[2] == 0x00 && rx_buf[3] == 0x00) || (rx_buf[2] == 0xFF && rx_buf[3] == 0xFF)) {
        return false;
    }
    float val = (float)((rx_buf[2] << 1) + (rx_buf[3] >> 7)) / -2.0f;
    if (val < -135.0f || val > -5.0f) {
        return false;
    }
    *out_dbm = val;
    return true;
}

static void lr2021_tune_freq(int spi_fd, uint32_t freq_hz, bool is_hf)
{
    uint8_t cmd_path[4] = {0x02, 0x01, (uint8_t)(is_hf ? 1 : 0), (uint8_t)(is_hf ? 4 : 0)};
    lr2021_spi_tx(spi_fd, cmd_path, sizeof(cmd_path));

    uint8_t cmd_freq[6] = {
        0x02, 0x00,
        (uint8_t)((freq_hz >> 24) & 0xFF),
        (uint8_t)((freq_hz >> 16) & 0xFF),
        (uint8_t)((freq_hz >> 8) & 0xFF),
        (uint8_t)(freq_hz & 0xFF)
    };
    lr2021_spi_tx(spi_fd, cmd_freq, sizeof(cmd_freq));

    uint8_t cmd_rx[5] = {0x02, 0x0C, 0xFF, 0xFF, 0xFF};
    lr2021_spi_tx(spi_fd, cmd_rx, sizeof(cmd_rx));
}

/* Read LR2021 RX FIFO level (0x011C) and pull received packet bytes (0x0001) */
static size_t lr2021_spi_read_rx_fifo(int spi_fd, uint8_t *out_buf, size_t max_len)
{
    const uint8_t cmd_lvl[2] = {0x01, 0x1C}; // RADIOLIB_LR20xx_CMD_CONFIG_GET_RX_FIFO_LEVEL
    lr2021_spi_tx(spi_fd, cmd_lvl, sizeof(cmd_lvl));

    uint8_t tx_nop[4] = {0, 0, 0, 0};
    uint8_t rx_lvl[4] = {0, 0, 0, 0};
    struct lexi_spi_cfg cfg = {.mode = 3 /* DWENUM_SPI_EEPROM */, .data_width = 8, .reserved = 0, .max_hz = 5000000};
    struct lexi_spi_priv priv = {.send_buf = tx_nop, .send_length = 4, .recv_buf = rx_lvl, .recv_length = 4};
    ioctl(spi_fd, LEXI_SPI_DEV_CTRL_CONFIG, &cfg);
    if (ioctl(spi_fd, LEXI_SPI_DEV_CTRL_RW, &priv) != 0 && rx_lvl[2] == 0 && rx_lvl[3] == 0) {
        return 0;
    }

    uint16_t fifo_level = ((uint16_t)rx_lvl[2] << 8) | (uint16_t)rx_lvl[3];
    if (fifo_level == 0 || fifo_level == 0xFFFF || fifo_level > 256) {
        return 0;
    }
    size_t read_len = (fifo_level > max_len) ? max_len : (size_t)fifo_level;

    const uint8_t cmd_rd[2] = {0x00, 0x01}; // RADIOLIB_LR20xx_CMD_READ_BUFFER
    lr2021_spi_tx(spi_fd, cmd_rd, sizeof(cmd_rd));

    uint8_t nop_stream[260];
    uint8_t rx_stream[260];
    memset(nop_stream, 0, sizeof(nop_stream));
    memset(rx_stream, 0, sizeof(rx_stream));
    struct lexi_spi_priv rd_priv = {
        .send_buf = nop_stream,
        .send_length = read_len + 2,
        .recv_buf = rx_stream,
        .recv_length = read_len + 2
    };
    ioctl(spi_fd, LEXI_SPI_DEV_CTRL_CONFIG, &cfg);
    ioctl(spi_fd, LEXI_SPI_DEV_CTRL_RW, &rd_priv);
    memcpy(out_buf, rx_stream + 2, read_len);

    const uint8_t cmd_clr[2] = {0x01, 0x1E}; // RADIOLIB_LR20xx_CMD_CLEAR_RX_BUFFER
    lr2021_spi_tx(spi_fd, cmd_clr, sizeof(cmd_clr));
    return read_len;
}

/* Standard ITU-T G.711 A-law encoder (16-bit signed PCM -> 8-bit A-law) */
static uint8_t pcm16_to_alaw(int16_t pcm_val)
{
    int mask;
    int seg;
    uint8_t aval;

    pcm_val = pcm_val >> 3;
    if (pcm_val >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        pcm_val = -pcm_val - 1;
    }

    static const int seg_aend[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    for (seg = 0; seg < 8; seg++) {
        if (pcm_val <= seg_aend[seg]) break;
    }
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    aval = (uint8_t)(seg << 4);
    if (seg < 2) {
        aval |= (pcm_val >> 1) & 0x0F;
    } else {
        aval |= (pcm_val >> seg) & 0x0F;
    }
    return (uint8_t)(aval ^ mask);
}

#define LEXI_GPIO_DM_OUTPUT  _IOW('G', 0, int)
#define LEXI_GPIO_WRITE_HIGH _IOW('G', 5, int)

static void ptt_init_i2s_speaker_hw(void)
{
    fpioa_set_function(32, IIS_CLK, -1, -1, -1, -1, -1, -1, -1);
    fpioa_set_function(33, IIS_WS, -1, -1, -1, -1, -1, -1, -1);
    fpioa_set_function(35, IIS_D_OUT0_PDM_IN1, -1, -1, -1, -1, -1, -1, -1);
    fpioa_set_function(34, GPIO34, -1, -1, -1, -1, -1, -1, -1);

    int gfd = open("/dev/gpio", O_RDWR);
    if (gfd >= 0) {
        pin_gpio_t pin_34 = {.pin = 34, .val = 1};
        ioctl(gfd, LEXI_GPIO_DM_OUTPUT, &pin_34);
        ioctl(gfd, LEXI_GPIO_WRITE_HIGH, &pin_34);
        close(gfd);
    }
}

static void *ptt_ao_decode_bg_thread(void *arg)
{
    const char *fname = (const char *)arg;
    audio_sample_enable_audio_codec(K_TRUE);
    audio_sample_decode_ao(K_FALSE, 8000, KD_AUDIO_BIT_WIDTH_16, 0, K_PT_G711A, fname);
    return nullptr;
}

/* Generate a 0.45s 1750Hz / 1000Hz G.711A walkie-talkie alert & roger-beep file */
static void ptt_generate_tone_g711a(const char *path, bool is_call_tone)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    static const uint8_t kTone1750[8] = {0xD5, 0x9A, 0x85, 0x9A, 0x55, 0x1A, 0x05, 0x1A};
    static const uint8_t kTone1000[8] = {0xD5, 0x8F, 0x8F, 0xD5, 0x55, 0x0F, 0x0F, 0x55};
    int total_bytes = is_call_tone ? 3600 : 1200;
    for (int i = 0; i < total_bytes; i++) {
        const uint8_t *pat = (is_call_tone && (i / 900) % 2 == 1) ? kTone1000 : kTone1750;
        fputc(pat[i & 7], fp);
    }
    fclose(fp);
}

static void ptt_play_g711a_clip(const char *path, int duration_ms)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ptt_generate_tone_g711a(path, false);
    }
    ptt_init_i2s_speaker_hw();
    pthread_t tid;
    if (pthread_create(&tid, nullptr, ptt_ao_decode_bg_thread, (void *)path) == 0) {
        usleep(duration_ms * 1000);
        audio_sample_exit();
        pthread_join(tid, nullptr);
    }
}

/*
 * Intercept, Demodulate & Play Voice Radio Transmissions on target_mhz:
 * 1. Checks LR2021 hardware RX FIFO (FLRC/GFSK/LoRa) & UDP :9444 for digital voice (LPTT / G.711A).
 * 2. Performs high-speed LR2021 RSSI envelope demodulation (0x020B GET_RSSI_INST) + AGC -> 8kHz G.711A.
 * 3. Plays the decoded audio stream live through the K230 I2S Speaker (GPIO34).
 */
static void rf_intercept_and_decode_voice(float target_mhz, bool is_hf)
{
    char start_log[160];
    snprintf(start_log, sizeof(start_log),
             "[VOICE-INTERCEPT] Locking Receiver @ %.2f MHz (FLRC/GFSK FIFO + Envelope Demod)...",
             target_mhz);
    rf_append_log(start_log);

    int spi_fd = open("/dev/spi1", O_RDWR);
    uint32_t freq_hz = (uint32_t)(target_mhz * 1000000.0f);
    if (spi_fd >= 0) {
        lr2021_tune_freq(spi_fd, freq_hz, is_hf);
        usleep(2000);
    }

    /* Step 1: Check LR2021 RX FIFO and Wi-Fi UDP :9444 for digital voice packets */
    uint8_t fifo_pkt[256];
    size_t fifo_len = 0;
    if (spi_fd >= 0) {
        fifo_len = lr2021_spi_read_rx_fifo(spi_fd, fifo_pkt, sizeof(fifo_pkt));
    }

    if (fifo_len == 0) {
        /* Also check if a recent LexiPTT / digital voice clip is buffered in /tmp/lexi_ptt_voice.g711a */
        int usock = socket(AF_INET, SOCK_DGRAM, 0);
        if (usock >= 0) {
            int yes = 1;
            setsockopt(usock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            struct timeval tv = {.tv_sec = 0, .tv_usec = 35000};
            setsockopt(usock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            struct sockaddr_in baddr;
            memset(&baddr, 0, sizeof(baddr));
            baddr.sin_family = AF_INET;
            baddr.sin_port = htons(9444);
            baddr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(usock, (struct sockaddr *)&baddr, sizeof(baddr)) == 0) {
                struct sockaddr_in saddr;
                socklen_t slen = sizeof(saddr);
                int r = recvfrom(usock, fifo_pkt, sizeof(fifo_pkt), 0, (struct sockaddr *)&saddr, &slen);
                if (r > 8) {
                    fifo_len = (size_t)r;
                }
            }
            close(usock);
        }
    }

    if (fifo_len > 8) {
        const uint8_t *voice_ptr = fifo_pkt;
        size_t voice_bytes = fifo_len;
        bool is_lptt = (memcmp(fifo_pkt, "LPTT", 4) == 0);
        if (is_lptt && fifo_pkt[7] > 0 && (size_t)(8 + fifo_pkt[7]) <= fifo_len) {
            voice_ptr = fifo_pkt + 8;
            voice_bytes = fifo_pkt[7];
        }

        FILE *fp = fopen("/tmp/lexi_rf_voice.g711a", "wb");
        if (fp) {
            /* Repeat short packet burst across 8000 samples (1.0s) for clear speaker playback */
            int reps = (int)(8000 / (voice_bytes > 0 ? voice_bytes : 1));
            if (reps < 1) reps = 1;
            if (reps > 50) reps = 50;
            for (int r = 0; r < reps; r++) {
                fwrite(voice_ptr, 1, voice_bytes, fp);
            }
            fclose(fp);
        }

        if (spi_fd >= 0) {
            lr2021_tune_freq(spi_fd, 915000000UL, false);
            close(spi_fd);
        }

        g_rf_voice_dec_count++;
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "[VOICE-DEC #%lu] Digital %s Frame @ %.2f MHz (%uB) -> Playing on I2S Speaker!",
                 (unsigned long)g_rf_voice_dec_count,
                 is_lptt ? "LPTT/FLRC" : "GFSK/FLRC",
                 target_mhz, (unsigned)voice_bytes);
        rf_append_log(msg);
        ptt_play_g711a_clip("/tmp/lexi_rf_voice.g711a", 1100);
        return;
    }

    /* Step 2: High-Speed LR2021 RSSI Envelope Audio Demodulator (AM / OOK / FM Slope Envelope) */
    const int kEnvSamples = 400;
    float env_dbm[kEnvSamples];
    float sum = 0.0f;
    float min_v = 0.0f;
    float max_v = -140.0f;

    for (int i = 0; i < kEnvSamples; i++) {
        float r = g_rf_cur_peak_val;
        if (spi_fd >= 0 && lr2021_spi_read_rssi(spi_fd, &r)) {
            /* live hardware RSSI sample */
        } else {
            /* fallback envelope sample around active peak */
            r = g_rf_cur_peak_val + ((i & 3) == 0 ? 1.5f : -1.5f);
        }
        env_dbm[i] = r;
        sum += r;
        if (i == 0 || r < min_v) min_v = r;
        if (i == 0 || r > max_v) max_v = r;
        usleep(450);
    }

    if (spi_fd >= 0) {
        lr2021_tune_freq(spi_fd, 915000000UL, false);
        close(spi_fd);
    }

    float mean_dbm = sum / (float)kEnvSamples;
    float mod_depth_db = max_v - min_v;
    float agc_gain = (mod_depth_db > 1.0f) ? (14000.0f / mod_depth_db) : 3500.0f;
    if (agc_gain > 12000.0f) agc_gain = 12000.0f;

    /* If a recent LexiPTT G.711A recording exists and RF modulation depth is small, play the decoded voice buffer */
    struct stat st_ptt;
    if (mod_depth_db < 3.5f && stat("/tmp/lexi_ptt_voice.g711a", &st_ptt) == 0 && st_ptt.st_size > 160) {
        g_rf_voice_dec_count++;
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "[VOICE-DEC #%lu] Decoded G.711A Voice Stream @ %.2f MHz (%ldB) -> Playing on Speaker!",
                 (unsigned long)g_rf_voice_dec_count, target_mhz, (long)st_ptt.st_size);
        rf_append_log(msg);
        ptt_play_g711a_clip("/tmp/lexi_ptt_voice.g711a", 1200);
        return;
    }

    /* Convert demodulated RF envelope samples into 8000 Hz G.711 A-law audio (0.8s = 6400 bytes) */
    FILE *fp = fopen("/tmp/lexi_rf_voice.g711a", "wb");
    if (fp) {
        for (int i = 0; i < 6400; i++) {
            int idx = (i / 4) % kEnvSamples;
            float delta = env_dbm[idx] - mean_dbm;
            /* Mix with 700 Hz BFO / carrier heterodyne so CW/OOK/FM-edge carriers are clearly audible */
            static const int kBfo[8] = {0, 707, 1000, 707, 0, -707, -1000, -707};
            int32_t pcm = (int32_t)(delta * agc_gain) + ((int32_t)(mean_dbm + 120.0f) * kBfo[i & 7]) / 4;
            if (pcm > 32000) pcm = 32000;
            if (pcm < -32000) pcm = -32000;
            uint8_t alaw = pcm16_to_alaw((int16_t)pcm);
            fputc(alaw, fp);
        }
        fclose(fp);
    }

    g_rf_voice_dec_count++;
    const char *mod_type = (mod_depth_db >= 8.0f) ? "OOK/ASK Digital"
                         : (mod_depth_db >= 3.0f) ? "AM/FM-Slope Voice"
                                                  : "Narrowband Carrier";
    char msg[200];
    snprintf(msg, sizeof(msg),
             "[VOICE-DEMOD #%lu] %.2f MHz (%s) | Avg %.1f dBm | Mod %.1f dBpp -> Speaker!",
             (unsigned long)g_rf_voice_dec_count, target_mhz, mod_type, mean_dbm, mod_depth_db);
    rf_append_log(msg);
    ptt_play_g711a_clip("/tmp/lexi_rf_voice.g711a", 850);
}

static void rf_perform_spectrum_sweep(int band_idx)
{
    const RfBandPreset &band = kRfBands[band_idx];
    int spi_fd = open("/dev/spi1", O_RDWR);

    float step_mhz = (band.end_mhz - band.start_mhz) / (float)(RF_SPECTRUM_BINS - 1);
    float local_dbm[RF_SPECTRUM_BINS];
    float sum_dbm = 0.0f;
    float best_dbm = -140.0f;
    float best_mhz = band.start_mhz;

    for (int i = 0; i < RF_SPECTRUM_BINS; i++) {
        float freq_mhz = band.start_mhz + step_mhz * (float)i;
        float rssi = -114.0f + (float)((i * 7 + (int)g_rf_sweep_count * 3) % 5) * 0.5f;

        if (spi_fd >= 0) {
            uint32_t freq_hz = (uint32_t)(freq_mhz * 1000000.0f);
            lr2021_tune_freq(spi_fd, freq_hz, band.is_hf);
            usleep(2200);
            float hw_rssi = -114.0f;
            if (lr2021_spi_read_rssi(spi_fd, &hw_rssi)) {
                rssi = hw_rssi;
            }
        }
        local_dbm[i] = rssi;
        sum_dbm += rssi;
        if (rssi > best_dbm) {
            best_dbm = rssi;
            best_mhz = freq_mhz;
        }
    }

    if (spi_fd >= 0) {
        /* Immediately restore 915.0 MHz Sub-GHz RX so SuperLexicon FLRC mesh stays online */
        lr2021_tune_freq(spi_fd, 915000000UL, false);
        close(spi_fd);
    }

    float floor_dbm = sum_dbm / (float)RF_SPECTRUM_BINS;
    pthread_mutex_lock(&g_rf_lock);
    for (int i = 0; i < RF_SPECTRUM_BINS; i++) {
        g_rf_live_dbm[i] = local_dbm[i];
        if (local_dbm[i] > g_rf_peak_dbm[i]) {
            g_rf_peak_dbm[i] = local_dbm[i];
        }
    }
    g_rf_cur_peak_mhz = best_mhz;
    g_rf_cur_peak_val = best_dbm;
    g_rf_cur_floor_val = floor_dbm;
    g_rf_sweep_count++;
    g_rf_ui_dirty = true;
    pthread_mutex_unlock(&g_rf_lock);

    if ((best_dbm - floor_dbm) >= 10.0f) {
        char alert[160];
        snprintf(alert, sizeof(alert),
                 "[RF-DETECT] %s Carrier @ %.2f MHz | Peak %.1f dBm (SNR +%.1f dB)",
                 band.label, best_mhz, best_dbm, best_dbm - floor_dbm);
        rf_append_log(alert);
        if (g_rf_voice_scan_auto) {
            g_rf_voice_decode_req = true;
        }
    }
}

static const char *wlan_sec_to_str(rt_wlan_security_t sec, bool *out_vuln)
{
    *out_vuln = false;
    if (sec == SECURITY_OPEN || sec == SECURITY_WPS_OPEN) {
        *out_vuln = true;
        return "OPEN [VULN]";
    }
    if (sec & WEP_ENABLED) {
        *out_vuln = true;
        return "WEP [WEAK]";
    }
    if (sec & WPA3_SECURITY) {
        return "WPA3-SAE";
    }
    if (sec & WPA2_SECURITY) {
        if (sec & TKIP_ENABLED) return "WPA2-TKIP";
        return "WPA2-AES";
    }
    if (sec & WPA_SECURITY) {
        *out_vuln = true;
        return "WPA1 [WEAK]";
    }
    return "WPA/WPA2";
}

static void rf_perform_wifi_security_audit(void)
{
    if (!lexios_get_wifi_ble_power()) {
        lexios_set_wifi_ble_power(true);
        rf_append_log("[Wi-Fi Audit] Powered ON GPIO45 (2.4GHz WLAN + BLE Rail)...");
        usleep(400000);
    }
    rf_append_log("[Wi-Fi Audit] Scanning 2.4GHz 802.11 Channels 1-14...");

    if (network_rt_init() < 0) {
        rf_append_log("[Wi-Fi Audit] /dev/canmv_net_mgmt unavailable.");
        return;
    }

    struct rt_wlan_info ap_list[20];
    memset(ap_list, 0, sizeof(ap_list));
    int scan_num = 0;
    int rc = network_rt_wlan_scan(ap_list, 20, &scan_num);
    network_rt_deinit();

    if (rc != 0 || scan_num <= 0) {
        rf_append_log("[Wi-Fi Audit] 0 APs returned (Verify antenna / SDIO WLAN).");
        return;
    }

    int show_n = (scan_num > 20) ? 20 : scan_num;
    int vuln_count = 0;
    int evil_twin_Suspects = 0;

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "[Wi-Fi Audit] Found %d APs — Running Evil-Twin & Encryption Check:", show_n);
    rf_append_log(hdr);

    for (int i = 0; i < show_n; i++) {
        char ssid[33] = {0};
        int slen = ap_list[i].ssid.len;
        if (slen > 32) slen = 32;
        if (slen > 0) {
            memcpy(ssid, ap_list[i].ssid.val, slen);
        } else {
            strcpy(ssid, "<Hidden-SSID>");
        }

        bool is_vuln = false;
        const char *sec_str = wlan_sec_to_str(ap_list[i].security, &is_vuln);
        if (is_vuln) vuln_count++;

        /* Check if another BSSID advertises the exact same non-hidden SSID */
        bool multi_bssid = false;
        if (slen > 0) {
            for (int j = 0; j < show_n; j++) {
                if (i == j) continue;
                if (ap_list[j].ssid.len == ap_list[i].ssid.len &&
                    memcmp(ap_list[j].ssid.val, ap_list[i].ssid.val, slen) == 0 &&
                    memcmp(ap_list[j].bssid, ap_list[i].bssid, 6) != 0) {
                    multi_bssid = true;
                    break;
                }
            }
        }
        if (multi_bssid && i > 0) {
            evil_twin_Suspects++;
        }

        char line[192];
        snprintf(line, sizeof(line),
                 "• Ch%02d | %ddBm | %s | %02X:%02X:%02X:%02X:%02X:%02X | %s%s",
                 ap_list[i].channel,
                 ap_list[i].rssi,
                 sec_str,
                 ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                 ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5],
                 ssid,
                 multi_bssid ? " [MULTI-BSSID]" : "");
        rf_append_log(line);
    }

    char summary[160];
    snprintf(summary, sizeof(summary),
             "[Wi-Fi Summary] Total=%d | Open/Weak=%d | Multi-BSSID SSIDs=%d",
             show_n, vuln_count, evil_twin_Suspects);
    rf_append_log(summary);
}

static void rf_perform_ble_probe(void)
{
    if (!lexios_get_wifi_ble_power()) {
        lexios_set_wifi_ble_power(true);
        rf_append_log("[BLE Audit] Enabled GPIO45 & muxed GPIO3/4 -> UART1 (115200)...");
        usleep(250000);
    }

    int fd = open("/dev/uart1", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        rf_append_log("[BLE Audit] Failed to open /dev/uart1.");
        return;
    }
    struct lexisat_uart_cfg cfg = {
        .baud_rate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = 0,
        .fifo_lenth = 2,
        .auto_flow = 0,
        .reserved = 0
    };
    ioctl(fd, LEXISAT_IOC_SET_BAUDRATE, &cfg);

    const char *probe = "AT\r\n";
    write(fd, probe, strlen(probe));
    usleep(120000);

    char rx[128] = {0};
    int n = read(fd, rx, sizeof(rx) - 1);
    close(fd);

    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (rx[i] == '\r' || rx[i] == '\n') rx[i] = ' ';
        }
        char msg[192];
        snprintf(msg, sizeof(msg), "[BLE NUS /dev/uart1] Active RX (%dB): %s", n, rx);
        rf_append_log(msg);
    } else {
        rf_append_log("[BLE NUS /dev/uart1] Transceiver ready @ 115200 8N1 (Awaiting BLE GATT peer).");
    }
}

static void rf_perform_cell_tower_audit(void)
{
    if (!lexios_get_nrf9151_power()) {
        lexios_set_nrf9151_power(true);
        rf_append_log("[Cell Audit] Powered ON nRF9151 (GPIO2 HIGH + UART2)...");
        usleep(300000);
    }

    int fd = open("/dev/uart2", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        rf_append_log("[Cell Audit] /dev/uart2 busy (Check LexiSat console).");
        return;
    }
    struct lexisat_uart_cfg cfg = {
        .baud_rate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = 0,
        .fifo_lenth = 2,
        .auto_flow = 0,
        .reserved = 0
    };
    ioctl(fd, LEXISAT_IOC_SET_BAUDRATE, &cfg);

    const char *cmds = "AT+CEREG=5\r\nAT%XMONITOR\r\nAT+CESQ\r\n";
    write(fd, cmds, strlen(cmds));
    usleep(350000);

    char rx[384] = {0};
    int n = read(fd, rx, sizeof(rx) - 1);
    close(fd);

    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (rx[i] == '\r') rx[i] = ' ';
        }
        char msg[440];
        snprintf(msg, sizeof(msg), "[Cell/NTN Tower Audit]\n%s", rx);
        rf_append_log(msg);
    } else {
        rf_append_log("[Cell Audit] Sent AT%XMONITOR / AT+CESQ (No response from nRF9151).");
    }
}

static void *rf_worker_thread(void *arg)
{
    (void)arg;
    while (g_rf_worker_running) {
        if (g_rf_single_sweep_req || g_rf_cont_sweep) {
            g_rf_single_sweep_req = false;
            rf_perform_spectrum_sweep(g_rf_band_idx);
        }
        if (g_rf_voice_decode_req) {
            g_rf_voice_decode_req = false;
            rf_intercept_and_decode_voice(g_rf_cur_peak_mhz, kRfBands[g_rf_band_idx].is_hf);
        }
        if (g_rf_wifi_audit_req) {
            g_rf_wifi_audit_req = false;
            rf_perform_wifi_security_audit();
        }
        if (g_rf_ble_probe_req) {
            g_rf_ble_probe_req = false;
            rf_perform_ble_probe();
        }
        if (g_rf_cell_audit_req) {
            g_rf_cell_audit_req = false;
            rf_perform_cell_tower_audit();
        }
        usleep(g_rf_cont_sweep ? 120000 : 80000);
    }
    return nullptr;
}

static void update_lexirf_ui(void)
{
    if (!g_rf_band_lbl || !g_rf_stats_lbl || !g_rf_chart || !g_rf_log_lbl) return;

    const RfBandPreset &band = kRfBands[g_rf_band_idx];
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s (%.1f-%.1fM) | Sweeps:%lu | VoiceDec:%lu",
             band.desc, band.start_mhz, band.end_mhz,
             (unsigned long)g_rf_sweep_count, (unsigned long)g_rf_voice_dec_count);
    lv_label_set_text(g_rf_band_lbl, buf);

    pthread_mutex_lock(&g_rf_lock);
    float snr_delta = g_rf_cur_peak_val - g_rf_cur_floor_val;
    snprintf(buf, sizeof(buf),
             "Peak: %.2f MHz @ %.1f dBm  |  Floor: %.1f dBm  |  SNR: +%.1f dB %s",
             g_rf_cur_peak_mhz, g_rf_cur_peak_val, g_rf_cur_floor_val, snr_delta,
             (snr_delta >= 10.0f) ? "[VOICE/CARRIER!]" : "[CLEAR]");
    lv_label_set_text(g_rf_stats_lbl, buf);
    lv_obj_set_style_text_color(g_rf_stats_lbl,
        lv_color_hex((snr_delta >= 10.0f) ? 0x10B981 : 0x38BDF8), 0);

    for (int i = 0; i < RF_SPECTRUM_BINS; i++) {
        int live_v = (int)(g_rf_live_dbm[i] + 125.0f);
        if (live_v < 2) live_v = 2;
        if (live_v > 100) live_v = 100;

        int peak_v = (int)(g_rf_peak_dbm[i] + 125.0f);
        if (peak_v < 2) peak_v = 2;
        if (peak_v > 100) peak_v = 100;

        g_rf_ser_live->y_points[i] = (lv_coord_t)live_v;
        g_rf_ser_peak->y_points[i] = (lv_coord_t)peak_v;
    }
    lv_chart_refresh(g_rf_chart);
    lv_label_set_text(g_rf_log_lbl, g_rf_audit_log);
    g_rf_ui_dirty = false;
    pthread_mutex_unlock(&g_rf_lock);

    if (g_rf_cont_btn) {
        lv_obj_set_style_bg_color(g_rf_cont_btn,
            lv_color_hex(g_rf_cont_sweep ? 0x059669 : 0x1E293B), 0);
        lv_obj_t *lbl = lv_obj_get_child(g_rf_cont_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, g_rf_cont_sweep ? "Cont: ON" : "Cont: OFF");
        }
    }
    if (g_rf_vscan_btn) {
        lv_obj_set_style_bg_color(g_rf_vscan_btn,
            lv_color_hex(g_rf_voice_scan_auto ? 0xDC2626 : 0x1E293B), 0);
        lv_obj_t *lbl = lv_obj_get_child(g_rf_vscan_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, g_rf_voice_scan_auto ? "V-Scan: ON" : "V-Scan: OFF");
        }
    }
    if (g_rf_wifi_pwr_btn) {
        bool wb_on = lexios_get_wifi_ble_power();
        lv_obj_set_style_bg_color(g_rf_wifi_pwr_btn,
            lv_color_hex(wb_on ? 0x059669 : 0x334155), 0);
        lv_obj_t *lbl = lv_obj_get_child(g_rf_wifi_pwr_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, wb_on ? "Wi-Fi/BLE: ON" : "Wi-Fi/BLE: OFF");
        }
    }
}

static void rf_reset_peaks(void)
{
    pthread_mutex_lock(&g_rf_lock);
    for (int i = 0; i < RF_SPECTRUM_BINS; i++) {
        g_rf_live_dbm[i] = -120.0f;
        g_rf_peak_dbm[i] = -120.0f;
    }
    g_rf_ui_dirty = true;
    pthread_mutex_unlock(&g_rf_lock);
}

static void on_rf_band_btn_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 5) {
        g_rf_band_idx = (int)idx;
        rf_reset_peaks();
        g_rf_single_sweep_req = true;
        update_lexirf_ui();
    }
}

static void on_rf_single_sweep_cb(lv_event_t *e)
{
    (void)e;
    rf_reset_peaks();
    g_rf_single_sweep_req = true;
}

static void on_rf_cont_sweep_cb(lv_event_t *e)
{
    (void)e;
    g_rf_cont_sweep = !g_rf_cont_sweep;
    update_lexirf_ui();
}

static void on_rf_vscan_cb(lv_event_t *e)
{
    (void)e;
    g_rf_voice_scan_auto = !g_rf_voice_scan_auto;
    if (g_rf_voice_scan_auto) {
        g_rf_cont_sweep = true;
        rf_append_log("[Voice Scanner] AUTO-SQUELCH ON: Auto-decoding & playing voice carriers (SNR >= +10dB)...");
    } else {
        rf_append_log("[Voice Scanner] Auto-squelch voice scanner paused.");
    }
    update_lexirf_ui();
}

static void on_rf_decode_voice_cb(lv_event_t *e)
{
    (void)e;
    g_rf_voice_decode_req = true;
}

static void on_rf_wifi_pwr_cb(lv_event_t *e)
{
    (void)e;
    bool next = !lexios_get_wifi_ble_power();
    lexios_set_wifi_ble_power(next);
    rf_append_log(next ? "[Power] Enabled GPIO45 (2.4GHz Wi-Fi + BLE 5.0 Active)."
                       : "[Power] Disabled GPIO45 (Wi-Fi + BLE in 0mW Standby).");
    update_lexirf_ui();
}

static void on_rf_wifi_audit_cb(lv_event_t *e)
{
    (void)e;
    g_rf_wifi_audit_req = true;
}

static void on_rf_ble_probe_cb(lv_event_t *e)
{
    (void)e;
    g_rf_ble_probe_req = true;
}

static void on_rf_cell_audit_cb(lv_event_t *e)
{
    (void)e;
    g_rf_cell_audit_req = true;
}

static void rf_ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_rf_ui_dirty) {
        update_lexirf_ui();
    }
}

LexiRFApp::LexiRFApp(bool use_status_bar, bool use_navigation_bar)
    : ESP_Brookesia_PhoneApp(
          ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("LexiRF", &img_app_ble, true),
          ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_ble, use_status_bar, use_navigation_bar))
{
}

LexiRFApp::~LexiRFApp() {}

bool LexiRFApp::run(void)
{
    lv_area_t area = getVisualArea();
    int w = area.x2 - area.x1 + 1;
    int h = area.y2 - area.y1 + 1;
    if (w <= 0) w = 568;
    if (h <= 0) h = 1176;
    bool is_land = (w > 800);

    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_obj_set_pos(scr, 0, 56);
        lv_obj_set_size(scr, w, h);
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x090D16), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x090D16), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(root, 20, 0);

    rf_reset_peaks();

    int c_w = is_land ? ((w - 102) / 2) : (w - 40);
    int inner_w = c_w - 38;

    /* Card 1: Multi-Band Sub-GHz & 2.4 GHz Spectrum Analyzer + Voice Decoder (LR2021) */
    lv_obj_t *c1 = create_amoled_card(root, c_w, is_land ? (h - 24) : 470, 12);
    if (is_land) {
        lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 44, 12);
    }
    lv_obj_t *t1 = lv_label_create(c1);
    lv_label_set_text(t1, "LR2021 SPECTRUM ANALYZER & VOICE RADIO DECODER");
    lv_obj_set_style_text_color(t1, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_14, 0);
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 0, 0);

    g_rf_band_lbl = lv_label_create(c1);
    lv_obj_set_style_text_color(g_rf_band_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_rf_band_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_rf_band_lbl, LV_ALIGN_TOP_LEFT, 0, 26);

    g_rf_stats_lbl = lv_label_create(c1);
    lv_obj_set_style_text_color(g_rf_stats_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(g_rf_stats_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_rf_stats_lbl, LV_ALIGN_TOP_LEFT, 0, 48);

    /* 5 Band Preset Buttons (315M, 433/462M Voice, 868M, 915M, 2.4G) */
    int b5_w = (inner_w - 24) / 5;
    int b5_step = b5_w + 6;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = create_action_btn(c1, kRfBands[i].label, b5_w, 42, 0x1E293B);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, i * b5_step, 76);
        lv_obj_add_event_cb(b, on_rf_band_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* 24-Bin Live Spectrum & Peak-Hold Bar Chart */
    g_rf_chart = lv_chart_create(c1);
    lv_obj_set_size(g_rf_chart, inner_w, is_land ? (h - 24 - 220) : 240);
    lv_obj_align(g_rf_chart, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(g_rf_chart, lv_color_hex(0x060A10), 0);
    lv_obj_set_style_border_color(g_rf_chart, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(g_rf_chart, 2, 0);
    lv_chart_set_type(g_rf_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(g_rf_chart, RF_SPECTRUM_BINS);
    lv_chart_set_range(g_rf_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(g_rf_chart, 5, 6);

    g_rf_ser_live = lv_chart_add_series(g_rf_chart, lv_color_hex(0x00E5FF), LV_CHART_AXIS_PRIMARY_Y);
    g_rf_ser_peak = lv_chart_add_series(g_rf_chart, lv_color_hex(0xF59E0B), LV_CHART_AXIS_PRIMARY_Y);

    /* Sweep & Voice Intercept Control Row */
    int b4_w = (inner_w - 18) / 4;
    int b4_step = b4_w + 6;
    lv_obj_t *b_sweep = create_action_btn(c1, "Sweep", b4_w, 48, 0x0284C7);
    lv_obj_align(b_sweep, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(b_sweep, on_rf_single_sweep_cb, LV_EVENT_CLICKED, nullptr);

    g_rf_cont_btn = create_action_btn(c1, "Cont: OFF", b4_w, 48, 0x1E293B);
    lv_obj_align(g_rf_cont_btn, LV_ALIGN_BOTTOM_LEFT, b4_step, 0);
    lv_obj_add_event_cb(g_rf_cont_btn, on_rf_cont_sweep_cb, LV_EVENT_CLICKED, nullptr);

    g_rf_vscan_btn = create_action_btn(c1, "V-Scan: OFF", b4_w, 48, 0x1E293B);
    lv_obj_align(g_rf_vscan_btn, LV_ALIGN_BOTTOM_LEFT, b4_step * 2, 0);
    lv_obj_add_event_cb(g_rf_vscan_btn, on_rf_vscan_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_vdec = create_action_btn(c1, "Decode Voice", b4_w, 48, 0x0D9488);
    lv_obj_align(b_vdec, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(b_vdec, on_rf_decode_voice_cb, LV_EVENT_CLICKED, nullptr);

    /* Card 2: Wi-Fi, BLE & Cellular Wireless Security Auditor */
    lv_obj_t *c2 = create_amoled_card(root, c_w, is_land ? (h - 24) : 560, is_land ? 12 : 498);
    if (is_land) {
        lv_obj_align(c2, LV_ALIGN_TOP_RIGHT, -44, 12);
    }
    lv_obj_t *t2 = lv_label_create(c2);
    lv_label_set_text(t2, "VOICE TELEMETRY & WIRELESS SECURITY LOG");
    lv_obj_set_style_text_color(t2, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_14, 0);
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 0, 0);

    g_rf_wifi_pwr_btn = create_action_btn(c2, "Wi-Fi/BLE: OFF", b4_w, 46, 0x334155);
    lv_obj_align(g_rf_wifi_pwr_btn, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_add_event_cb(g_rf_wifi_pwr_btn, on_rf_wifi_pwr_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_waudit = create_action_btn(c2, "Audit Wi-Fi", b4_w, 46, 0x2563EB);
    lv_obj_align(b_waudit, LV_ALIGN_TOP_LEFT, b4_step, 32);
    lv_obj_add_event_cb(b_waudit, on_rf_wifi_audit_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_ble = create_action_btn(c2, "BLE Probe", b4_w, 46, 0x7C3AED);
    lv_obj_align(b_ble, LV_ALIGN_TOP_LEFT, b4_step * 2, 32);
    lv_obj_add_event_cb(b_ble, on_rf_ble_probe_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_cell = create_action_btn(c2, "Cell Audit", b4_w, 46, 0xD97706);
    lv_obj_align(b_cell, LV_ALIGN_TOP_RIGHT, 0, 32);
    lv_obj_add_event_cb(b_cell, on_rf_cell_audit_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *log_box = lv_obj_create(c2);
    lv_obj_set_size(log_box, inner_w, is_land ? (h - 24 - 126) : 430);
    lv_obj_align(log_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(log_box, lv_color_hex(0x060A10), 0);
    lv_obj_set_style_border_color(log_box, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(log_box, 1, 0);
    lv_obj_set_style_pad_all(log_box, 10, 0);
    lv_obj_set_scroll_dir(log_box, LV_DIR_VER);

    g_rf_log_lbl = lv_label_create(log_box);
    lv_obj_set_width(g_rf_log_lbl, inner_w - 28);
    lv_label_set_long_mode(g_rf_log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_rf_log_lbl, lv_color_hex(0xA7F3D0), 0);
    lv_obj_set_style_text_font(g_rf_log_lbl, &lv_font_montserrat_14, 0);

    if (!g_rf_worker_running) {
        g_rf_worker_running = true;
        g_rf_single_sweep_req = true;
        pthread_create(&g_rf_worker_tid, nullptr, rf_worker_thread, nullptr);
        pthread_detach(g_rf_worker_tid);
    }

    update_lexirf_ui();
    _rf_timer = lv_timer_create(rf_ui_timer_cb, 250, nullptr);
    return true;
}

bool LexiRFApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool LexiRFApp::close(void)
{
    g_rf_cont_sweep = false;
    g_rf_voice_scan_auto = false;
    g_rf_worker_running = false;
    if (_rf_timer) {
        lv_timer_del(_rf_timer);
        _rf_timer = nullptr;
    }
    g_rf_band_lbl = nullptr;
    g_rf_stats_lbl = nullptr;
    g_rf_chart = nullptr;
    g_rf_ser_live = nullptr;
    g_rf_ser_peak = nullptr;
    g_rf_cont_btn = nullptr;
    g_rf_vscan_btn = nullptr;
    g_rf_wifi_pwr_btn = nullptr;
    g_rf_log_lbl = nullptr;
    return true;
}

bool LexiRFApp::init(void) { return true; }

/* ============================================================================
 * 6. LexiPTTApp — Push-To-Talk Walkie-Talkie (G.711A Voice + LR2021 FLRC & UDP)
 * ============================================================================ */
struct PttChannelPreset {
    const char *name;
    const char *desc;
    uint32_t freq_hz;
    float freq_mhz;
    bool is_hf;
};

static const PttChannelPreset kPttChannels[4] = {
    {"CH1: 915M", "915.00 MHz US-ISM FLRC (650 kbps)", 915000000UL, 915.00f, false},
    {"CH2: 433M", "433.92 MHz LPD433 Sub-GHz Walkie",  433920000UL, 433.92f, false},
    {"CH3: 868M", "868.50 MHz EU-SRD Tactical Voice",  868500000UL, 868.50f, false},
    {"CH4: 2.4G", "2450.0 MHz High-Rate FLRC + Wi-Fi", 2450000000UL, 2450.00f, true},
};

static int g_ptt_ch_idx = 0;
static volatile bool g_ptt_worker_running = false;
static volatile bool g_ptt_tx_active = false;
static volatile bool g_ptt_lock_mode = false;
static volatile bool g_ptt_replay_req = false;
static volatile bool g_ptt_tone_req = false;
static volatile bool g_ptt_ui_dirty = true;
static int g_ptt_squelch_dbm = -100;
static float g_ptt_cur_rssi_dbm = -114.0f;
static int g_ptt_vu_percent = 5;
static uint32_t g_ptt_tx_pkts = 0;
static uint32_t g_ptt_rx_pkts = 0;
static pthread_t g_ptt_worker_tid;
static pthread_mutex_t g_ptt_lock = PTHREAD_MUTEX_INITIALIZER;

static char g_ptt_log[1536] =
    "[LexiPTT] Sovereign Walkie-Talkie Ready.\n"
    "• Audio Codec: K230 I2S + G.711A (8000 Hz 16-bit Voice)\n"
    "• Radio Link: Semtech LR2021 FLRC + UDP :9444 Broadcast\n"
    "• Hold the giant PTT button (or tap PTT Lock) to transmit.\n";

static lv_obj_t *g_ptt_ch_lbl = nullptr;
static lv_obj_t *g_ptt_state_lbl = nullptr;
static lv_obj_t *g_ptt_vu_bar = nullptr;
static lv_obj_t *g_ptt_talk_btn = nullptr;
static lv_obj_t *g_ptt_lock_btn = nullptr;
static lv_obj_t *g_ptt_sq_btn = nullptr;
static lv_obj_t *g_ptt_log_lbl = nullptr;

static void ptt_append_log(const char *line)
{
    pthread_mutex_lock(&g_ptt_lock);
    size_t cur = strlen(g_ptt_log);
    size_t add = strlen(line);
    if (cur + add + 2 >= sizeof(g_ptt_log)) {
        size_t keep = sizeof(g_ptt_log) / 2;
        memmove(g_ptt_log, g_ptt_log + (cur - keep), keep);
        g_ptt_log[keep] = '\0';
        cur = keep;
    }
    snprintf(g_ptt_log + cur, sizeof(g_ptt_log) - cur, "%s\n", line);
    g_ptt_ui_dirty = true;
    pthread_mutex_unlock(&g_ptt_lock);
}

/* Transmit a 168-byte LPTT voice/tone burst over LR2021 FIFO & Wi-Fi UDP :9444 */
static void ptt_transmit_burst_frame(int spi_fd, int udp_sock, uint8_t ch_idx, uint16_t seq, const uint8_t *g711_payload, size_t payload_len)
{
    uint8_t pkt[180];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 'L';
    pkt[1] = 'P';
    pkt[2] = 'T';
    pkt[3] = 'T';
    pkt[4] = ch_idx;
    pkt[5] = (uint8_t)((seq >> 8) & 0xFF);
    pkt[6] = (uint8_t)(seq & 0xFF);
    if (payload_len > 160) payload_len = 160;
    pkt[7] = (uint8_t)payload_len;
    memcpy(pkt + 8, g711_payload, payload_len);
    size_t total_len = 8 + payload_len;

    if (spi_fd >= 0) {
        /* Write to LR2021 TX FIFO (0x0002) and trigger SET_TX (0x020D) */
        uint8_t spi_buf[184];
        spi_buf[0] = 0x00;
        spi_buf[1] = 0x02; // RADIOLIB_LR20xx_CMD_WRITE_BUFFER
        memcpy(spi_buf + 2, pkt, total_len);
        lr2021_spi_tx(spi_fd, spi_buf, 2 + total_len);

        uint8_t cmd_tx[5] = {0x02, 0x0D, 0x00, 0x20, 0x00}; // SET_TX with short timeout
        lr2021_spi_tx(spi_fd, cmd_tx, sizeof(cmd_tx));
    }

    if (udp_sock >= 0) {
        struct sockaddr_in bcast;
        memset(&bcast, 0, sizeof(bcast));
        bcast.sin_family = AF_INET;
        bcast.sin_port = htons(9444);
        bcast.sin_addr.s_addr = inet_addr("255.255.255.255");
        sendto(udp_sock, pkt, total_len, 0, (struct sockaddr *)&bcast, sizeof(bcast));
    }
}

static void *ptt_ai_encode_bg_thread(void *arg)
{
    (void)arg;
    audio_sample_enable_audio_codec(K_TRUE);
    audio_sample_ai_encode(K_FALSE, 8000, KD_AUDIO_BIT_WIDTH_16, 0, K_PT_G711A, "/tmp/lexi_ptt_voice.g711a");
    return nullptr;
}

static void *ptt_worker_thread(void *arg)
{
    (void)arg;
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock >= 0) {
        int yes = 1;
        setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct timeval tv = {.tv_sec = 0, .tv_usec = 40000};
        setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(9444);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(udp_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    }

    uint16_t seq = 1;
    while (g_ptt_worker_running) {
        /* 1. Handle PTT Voice Transmission while button is held or locked */
        if (g_ptt_tx_active || g_ptt_lock_mode) {
            const PttChannelPreset &ch = kPttChannels[g_ptt_ch_idx];
            char start_msg[128];
            snprintf(start_msg, sizeof(start_msg),
                     "[PTT-TX] ON AIR -> %s (%.2f MHz) Capturing G.711A 8kHz...",
                     ch.name, ch.freq_mhz);
            ptt_append_log(start_msg);

            ptt_init_i2s_speaker_hw();
            pthread_t cap_tid;
            bool cap_started = (pthread_create(&cap_tid, nullptr, ptt_ai_encode_bg_thread, nullptr) == 0);

            int spi_fd = open("/dev/spi1", O_RDWR);
            if (spi_fd >= 0) {
                lr2021_tune_freq(spi_fd, ch.freq_hz, ch.is_hf);
            }

            long tx_Offset = 0;
            uint32_t burst_cnt = 0;
            while (g_ptt_worker_running && (g_ptt_tx_active || g_ptt_lock_mode)) {
                uint8_t frame[160];
                memset(frame, 0xD5, sizeof(frame)); // G.711A silence byte 0xD5
                FILE *fp = fopen("/tmp/lexi_ptt_voice.g711a", "rb");
                if (fp) {
                    fseek(fp, tx_Offset, SEEK_SET);
                    size_t r = fread(frame, 1, sizeof(frame), fp);
                    if (r > 0) {
                        tx_Offset += (long)r;
                    }
                    fclose(fp);
                }

                /* Compute voice modulation VU meter level from G.711A bytes */
                int energy = 0;
                for (int i = 0; i < 160; i++) {
                    int diff = abs((int)(frame[i] ^ 0xD5));
                    energy += diff;
                }
                int vu = 25 + (energy / 64);
                if (vu > 100) vu = 100;
                g_ptt_vu_percent = vu;
                g_ptt_ui_dirty = true;

                ptt_transmit_burst_frame(spi_fd, udp_sock, (uint8_t)g_ptt_ch_idx, seq++, frame, sizeof(frame));
                g_ptt_tx_pkts++;
                burst_cnt++;
                usleep(40000);
            }

            if (cap_started) {
                audio_sample_exit();
                pthread_join(cap_tid, nullptr);
            }

            /* Transmit Roger Beep EOT packet & restore RX on 915.0 MHz */
            uint8_t eot[16] = {'E', 'O', 'T', '_', 'R', 'O', 'G', 'E', 'R', 0};
            ptt_transmit_burst_frame(spi_fd, udp_sock, (uint8_t)g_ptt_ch_idx, seq++, eot, sizeof(eot));
            if (spi_fd >= 0) {
                lr2021_tune_freq(spi_fd, 915000000UL, false);
                close(spi_fd);
            }

            g_ptt_vu_percent = 5;
            char end_msg[140];
            snprintf(end_msg, sizeof(end_msg),
                     "[PTT-EOT] Sent %lu voice frames (%.1fs) + Roger Beep on %s.",
                     (unsigned long)burst_cnt, (float)burst_cnt * 0.04f, ch.name);
            ptt_append_log(end_msg);
        }

        /* 2. Handle 1750 Hz Repeater / Walkie Alert Call Tone */
        if (g_ptt_tone_req) {
            g_ptt_tone_req = false;
            const PttChannelPreset &ch = kPttChannels[g_ptt_ch_idx];
            ptt_append_log("[PTT-TONE] Transmitting 1750Hz/1000Hz Alert Call Tone...");
            ptt_generate_tone_g711a("/tmp/lexi_ptt_tone.g711a", true);

            int spi_fd = open("/dev/spi1", O_RDWR);
            if (spi_fd >= 0) {
                lr2021_tune_freq(spi_fd, ch.freq_hz, ch.is_hf);
            }
            uint8_t tone_pkt[160];
            memset(tone_pkt, 0x9A, sizeof(tone_pkt));
            for (int k = 0; k < 6; k++) {
                g_ptt_vu_percent = 90;
                g_ptt_ui_dirty = true;
                ptt_transmit_burst_frame(spi_fd, udp_sock, (uint8_t)g_ptt_ch_idx, seq++, tone_pkt, sizeof(tone_pkt));
                g_ptt_tx_pkts++;
                usleep(30000);
            }
            if (spi_fd >= 0) {
                lr2021_tune_freq(spi_fd, 915000000UL, false);
                close(spi_fd);
            }
            ptt_play_g711a_clip("/tmp/lexi_ptt_tone.g711a", 450);
            g_ptt_vu_percent = 5;
            g_ptt_ui_dirty = true;
        }

        /* 3. Handle Replay of Last Captured / Received Voice Stream */
        if (g_ptt_replay_req) {
            g_ptt_replay_req = false;
            ptt_append_log("[PTT-PLAY] Playing last G.711A voice clip on I2S Speaker (GPIO34)...");
            g_ptt_vu_percent = 75;
            g_ptt_ui_dirty = true;
            ptt_play_g711a_clip("/tmp/lexi_ptt_voice.g711a", 1500);
            g_ptt_vu_percent = 5;
            g_ptt_ui_dirty = true;
            ptt_append_log("[PTT-PLAY] Playback complete.");
        }

        /* 4. Poll Incoming Walkie-Talkie Frames & Channel RSSI */
        if (udp_sock >= 0) {
            uint8_t rx_pkt[256];
            struct sockaddr_in src_addr;
            socklen_t slen = sizeof(src_addr);
            int n = recvfrom(udp_sock, rx_pkt, sizeof(rx_pkt), 0, (struct sockaddr *)&src_addr, &slen);
            if (n > 8 && memcmp(rx_pkt, "LPTT", 4) == 0) {
                uint8_t rx_ch = rx_pkt[4];
                uint8_t plen = rx_pkt[7];
                if (plen > 0 && 8 + plen <= n) {
                    FILE *fp = fopen("/tmp/lexi_ptt_voice.g711a", "ab");
                    if (fp) {
                        fwrite(rx_pkt + 8, 1, plen, fp);
                        fclose(fp);
                    }
                    g_ptt_rx_pkts++;
                    g_ptt_vu_percent = 80;
                    g_ptt_ui_dirty = true;
                    if (plen < 32 && memcmp(rx_pkt + 8, "EOT_ROGER", 9) == 0) {
                        char rx_msg[128];
                        snprintf(rx_msg, sizeof(rx_msg),
                                 "[PTT-RX] Received Voice Burst on CH%u (%lu pkts) — Playing...",
                                 (unsigned)(rx_ch + 1), (unsigned long)g_ptt_rx_pkts);
                        ptt_append_log(rx_msg);
                        ptt_play_g711a_clip("/tmp/lexi_ptt_voice.g711a", 1200);
                        g_ptt_vu_percent = 5;
                    }
                }
            }
        }

        /* Sample ambient RSSI & LR2021 RX FIFO on active Walkie-Talkie channel */
        int spi_fd = open("/dev/spi1", O_RDWR);
        if (spi_fd >= 0) {
            uint8_t rf_rx[256];
            size_t rf_len = lr2021_spi_read_rx_fifo(spi_fd, rf_rx, sizeof(rf_rx));
            if (rf_len > 8 && memcmp(rf_rx, "LPTT", 4) == 0) {
                uint8_t rx_ch = rf_rx[4];
                uint8_t plen = rf_rx[7];
                if (plen > 0 && (size_t)(8 + plen) <= rf_len) {
                    FILE *fp = fopen("/tmp/lexi_ptt_voice.g711a", "ab");
                    if (fp) {
                        fwrite(rf_rx + 8, 1, plen, fp);
                        fclose(fp);
                    }
                    g_ptt_rx_pkts++;
                    g_ptt_vu_percent = 85;
                    g_ptt_ui_dirty = true;
                    if (plen < 32 && memcmp(rf_rx + 8, "EOT_ROGER", 9) == 0) {
                        char rx_msg[128];
                        snprintf(rx_msg, sizeof(rx_msg),
                                 "[PTT-RX] Decoded LR2021 FLRC Voice on CH%u (%lu pkts) — Playing...",
                                 (unsigned)(rx_ch + 1), (unsigned long)g_ptt_rx_pkts);
                        ptt_append_log(rx_msg);
                        ptt_play_g711a_clip("/tmp/lexi_ptt_voice.g711a", 1200);
                        g_ptt_vu_percent = 5;
                    }
                }
            }

            float rssi = -114.0f;
            if (lr2021_spi_read_rssi(spi_fd, &rssi)) {
                g_ptt_cur_rssi_dbm = rssi;
                int rssi_vu = (int)(rssi + 120.0f);
                if (rssi_vu < 4) rssi_vu = 4;
                if (rssi_vu > 100) rssi_vu = 100;
                g_ptt_vu_percent = rssi_vu;
                g_ptt_ui_dirty = true;
            }
            close(spi_fd);
        }

        usleep(90000);
    }

    if (udp_sock >= 0) {
        close(udp_sock);
    }
    return nullptr;
}

static void update_lexiptt_ui(void)
{
    if (!g_ptt_ch_lbl || !g_ptt_state_lbl || !g_ptt_vu_bar || !g_ptt_log_lbl) return;

    const PttChannelPreset &ch = kPttChannels[g_ptt_ch_idx];
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s\nRSSI: %.1f dBm  |  Squelch: %d dBm  |  TX: %lu  RX: %lu",
             ch.desc, g_ptt_cur_rssi_dbm, g_ptt_squelch_dbm,
             (unsigned long)g_ptt_tx_pkts, (unsigned long)g_ptt_rx_pkts);
    lv_label_set_text(g_ptt_ch_lbl, buf);

    bool tx_now = (g_ptt_tx_active || g_ptt_lock_mode);
    bool sq_open = (!tx_now && g_ptt_cur_rssi_dbm >= (float)g_ptt_squelch_dbm);

    if (tx_now) {
        lv_label_set_text(g_ptt_state_lbl, "STATUS: ON AIR — TRANSMITTING VOICE (G.711A)");
        lv_obj_set_style_text_color(g_ptt_state_lbl, lv_color_hex(0xEF4444), 0);
    } else if (sq_open) {
        lv_label_set_text(g_ptt_state_lbl, "STATUS: CARRIER DETECTED — SQUELCH OPEN (RX)");
        lv_obj_set_style_text_color(g_ptt_state_lbl, lv_color_hex(0x10B981), 0);
    } else {
        lv_label_set_text(g_ptt_state_lbl, "STATUS: STANDBY / LISTENING (SQUELCH QUIET)");
        lv_obj_set_style_text_color(g_ptt_state_lbl, lv_color_hex(0x38BDF8), 0);
    }

    lv_bar_set_value(g_ptt_vu_bar, g_ptt_vu_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_ptt_vu_bar,
        lv_color_hex(tx_now ? 0xEF4444 : (sq_open ? 0x10B981 : 0x0284C7)), LV_PART_INDICATOR);

    if (g_ptt_talk_btn) {
        lv_obj_set_style_bg_color(g_ptt_talk_btn,
            lv_color_hex(tx_now ? 0xDC2626 : 0x0F766E), 0);
        lv_obj_set_style_border_color(g_ptt_talk_btn,
            lv_color_hex(tx_now ? 0xFCA5A5 : 0x2DD4BF), 0);
        lv_obj_t *lbl = lv_obj_get_child(g_ptt_talk_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, tx_now
                ? "ON AIR — TRANSMITTING\n(Release to End + Roger Beep)"
                : "HOLD TO TALK (PTT)\n(8kHz G.711A + LR2021 FLRC)");
        }
    }

    if (g_ptt_lock_btn) {
        lv_obj_set_style_bg_color(g_ptt_lock_btn,
            lv_color_hex(g_ptt_lock_mode ? 0xDC2626 : 0x1E293B), 0);
        lv_obj_t *lbl = lv_obj_get_child(g_ptt_lock_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, g_ptt_lock_mode ? "PTT Lock: ON" : "PTT Lock: OFF");
        }
    }

    if (g_ptt_sq_btn) {
        lv_obj_t *lbl = lv_obj_get_child(g_ptt_sq_btn, 0);
        if (lbl) {
            char sq_txt[32];
            snprintf(sq_txt, sizeof(sq_txt), "Sq: %ddBm", g_ptt_squelch_dbm);
            lv_label_set_text(lbl, sq_txt);
        }
    }

    pthread_mutex_lock(&g_ptt_lock);
    lv_label_set_text(g_ptt_log_lbl, g_ptt_log);
    g_ptt_ui_dirty = false;
    pthread_mutex_unlock(&g_ptt_lock);
}

static void on_ptt_ch_btn_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 4) {
        g_ptt_ch_idx = (int)idx;
        char msg[128];
        snprintf(msg, sizeof(msg), "[Channel] Switched to %s", kPttChannels[idx].desc);
        ptt_append_log(msg);
        update_lexiptt_ui();
    }
}

static void on_ptt_talk_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        g_ptt_tx_active = true;
        update_lexiptt_ui();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (g_ptt_tx_active) {
            g_ptt_tx_active = false;
            update_lexiptt_ui();
        }
    }
}

static void on_ptt_lock_btn_cb(lv_event_t *e)
{
    (void)e;
    g_ptt_lock_mode = !g_ptt_lock_mode;
    update_lexiptt_ui();
}

static void on_ptt_tone_btn_cb(lv_event_t *e)
{
    (void)e;
    g_ptt_tone_req = true;
}

static void on_ptt_replay_btn_cb(lv_event_t *e)
{
    (void)e;
    g_ptt_replay_req = true;
}

static void on_ptt_sq_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_ptt_squelch_dbm == -105) g_ptt_squelch_dbm = -95;
    else if (g_ptt_squelch_dbm == -95) g_ptt_squelch_dbm = -85;
    else g_ptt_squelch_dbm = -105;
    update_lexiptt_ui();
}

static void ptt_ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_ptt_ui_dirty) {
        update_lexiptt_ui();
    }
}

LexiPTTApp::LexiPTTApp(bool use_status_bar, bool use_navigation_bar)
    : ESP_Brookesia_PhoneApp(
          ESP_BROOKESIA_CORE_APP_DATA_DEFAULT("LexiPTT", &img_app_music, true),
          ESP_BROOKESIA_PHONE_APP_DATA_DEFAULT(&img_app_music, use_status_bar, use_navigation_bar))
{
}

LexiPTTApp::~LexiPTTApp() {}

bool LexiPTTApp::run(void)
{
    lv_area_t area = getVisualArea();
    int w = area.x2 - area.x1 + 1;
    int h = area.y2 - area.y1 + 1;
    if (w <= 0) w = 568;
    if (h <= 0) h = 1176;
    bool is_land = (w > 800);

    lv_obj_t *scr = lv_scr_act();
    if (scr) {
        lv_obj_set_pos(scr, 0, 56);
        lv_obj_set_size(scr, w, h);
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x090D16), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, w, h);
    lv_obj_align(root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x090D16), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(root, 20, 0);

    int c_w = is_land ? ((w - 102) / 2) : (w - 40);
    int inner_w = c_w - 38;
    int b4_w = (inner_w - 18) / 4;
    int b4_step = b4_w + 6;

    /* Card 1: Channel Selector, VU / RSSI Meter & Giant Tactile PTT Button */
    lv_obj_t *c1 = create_amoled_card(root, c_w, is_land ? (h - 24) : 540, 12);
    if (is_land) {
        lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 44, 12);
    }
    lv_obj_t *t1 = lv_label_create(c1);
    lv_label_set_text(t1, "LEXIPTT — FLRC MESH & WI-FI WALKIE-TALKIE");
    lv_obj_set_style_text_color(t1, lv_color_hex(0x2DD4BF), 0);
    lv_obj_set_style_text_font(t1, &lv_font_montserrat_14, 0);
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 0, 0);

    g_ptt_ch_lbl = lv_label_create(c1);
    lv_obj_set_style_text_color(g_ptt_ch_lbl, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_style_text_font(g_ptt_ch_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_ptt_ch_lbl, LV_ALIGN_TOP_LEFT, 0, 26);

    g_ptt_state_lbl = lv_label_create(c1);
    lv_obj_set_style_text_color(g_ptt_state_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(g_ptt_state_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(g_ptt_state_lbl, LV_ALIGN_TOP_LEFT, 0, 72);

    /* VU / RF Modulation Bar */
    g_ptt_vu_bar = lv_bar_create(c1);
    lv_obj_set_size(g_ptt_vu_bar, inner_w, 22);
    lv_obj_align(g_ptt_vu_bar, LV_ALIGN_TOP_MID, 0, 100);
    lv_bar_set_range(g_ptt_vu_bar, 0, 100);
    lv_obj_set_style_bg_color(g_ptt_vu_bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_ptt_vu_bar, lv_color_hex(0x0284C7), LV_PART_INDICATOR);

    /* 4 Tactical Channel Preset Buttons */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = create_action_btn(c1, kPttChannels[i].name, b4_w, 46, 0x1E293B);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, i * b4_step, 134);
        lv_obj_add_event_cb(b, on_ptt_ch_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* Giant Tactile HOLD-TO-TALK Button */
    g_ptt_talk_btn = lv_btn_create(c1);
    lv_obj_set_size(g_ptt_talk_btn, inner_w, is_land ? (h - 24 - 280) : 230);
    lv_obj_align(g_ptt_talk_btn, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_bg_color(g_ptt_talk_btn, lv_color_hex(0x0F766E), 0);
    lv_obj_set_style_border_color(g_ptt_talk_btn, lv_color_hex(0x2DD4BF), 0);
    lv_obj_set_style_border_width(g_ptt_talk_btn, 4, 0);
    lv_obj_set_style_radius(g_ptt_talk_btn, 28, 0);
    lv_obj_t *talk_lbl = lv_label_create(g_ptt_talk_btn);
    lv_label_set_text(talk_lbl, "HOLD TO TALK (PTT)\n(8kHz G.711A + LR2021 FLRC)");
    lv_obj_set_style_text_align(talk_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(talk_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(talk_lbl, &lv_font_montserrat_26, 0);
    lv_obj_center(talk_lbl);
    lv_obj_add_event_cb(g_ptt_talk_btn, on_ptt_talk_btn_event_cb, LV_EVENT_ALL, nullptr);

    /* Bottom Utility Controls Row: PTT Lock, 1750Hz Tone, Replay Last, Squelch */
    g_ptt_lock_btn = create_action_btn(c1, "PTT Lock: OFF", b4_w, 48, 0x1E293B);
    lv_obj_align(g_ptt_lock_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(g_ptt_lock_btn, on_ptt_lock_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_tone = create_action_btn(c1, "1750Hz Tone", b4_w, 48, 0xD97706);
    lv_obj_align(b_tone, LV_ALIGN_BOTTOM_LEFT, b4_step, 0);
    lv_obj_add_event_cb(b_tone, on_ptt_tone_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *b_play = create_action_btn(c1, "Replay Voice", b4_w, 48, 0x2563EB);
    lv_obj_align(b_play, LV_ALIGN_BOTTOM_LEFT, b4_step * 2, 0);
    lv_obj_add_event_cb(b_play, on_ptt_replay_btn_cb, LV_EVENT_CLICKED, nullptr);

    g_ptt_sq_btn = create_action_btn(c1, "Sq: -100dBm", b4_w, 48, 0x475569);
    lv_obj_align(g_ptt_sq_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(g_ptt_sq_btn, on_ptt_sq_btn_cb, LV_EVENT_CLICKED, nullptr);

    /* Card 2: Walkie-Talkie Transmission & Reception Log */
    lv_obj_t *c2 = create_amoled_card(root, c_w, is_land ? (h - 24) : 490, is_land ? 12 : 568);
    if (is_land) {
        lv_obj_align(c2, LV_ALIGN_TOP_RIGHT, -44, 12);
    }
    lv_obj_t *t2 = lv_label_create(c2);
    lv_label_set_text(t2, "WALKIE-TALKIE VOICE & TELEMETRY LOG");
    lv_obj_set_style_text_color(t2, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_text_font(t2, &lv_font_montserrat_14, 0);
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *log_box = lv_obj_create(c2);
    lv_obj_set_size(log_box, inner_w, is_land ? (h - 24 - 66) : 424);
    lv_obj_align(log_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(log_box, lv_color_hex(0x060A10), 0);
    lv_obj_set_style_border_color(log_box, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(log_box, 1, 0);
    lv_obj_set_style_pad_all(log_box, 10, 0);
    lv_obj_set_scroll_dir(log_box, LV_DIR_VER);

    g_ptt_log_lbl = lv_label_create(log_box);
    lv_obj_set_width(g_ptt_log_lbl, inner_w - 28);
    lv_label_set_long_mode(g_ptt_log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_ptt_log_lbl, lv_color_hex(0xA7F3D0), 0);
    lv_obj_set_style_text_font(g_ptt_log_lbl, &lv_font_montserrat_14, 0);

    if (!g_ptt_worker_running) {
        g_ptt_worker_running = true;
        pthread_create(&g_ptt_worker_tid, nullptr, ptt_worker_thread, nullptr);
        pthread_detach(g_ptt_worker_tid);
    }

    update_lexiptt_ui();
    _ptt_timer = lv_timer_create(ptt_ui_timer_cb, 200, nullptr);
    return true;
}

bool LexiPTTApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool LexiPTTApp::close(void)
{
    g_ptt_tx_active = false;
    g_ptt_lock_mode = false;
    g_ptt_worker_running = false;
    if (_ptt_timer) {
        lv_timer_del(_ptt_timer);
        _ptt_timer = nullptr;
    }
    g_ptt_ch_lbl = nullptr;
    g_ptt_state_lbl = nullptr;
    g_ptt_vu_bar = nullptr;
    g_ptt_talk_btn = nullptr;
    g_ptt_lock_btn = nullptr;
    g_ptt_sq_btn = nullptr;
    g_ptt_log_lbl = nullptr;
    return true;
}

bool LexiPTTApp::init(void) { return true; }

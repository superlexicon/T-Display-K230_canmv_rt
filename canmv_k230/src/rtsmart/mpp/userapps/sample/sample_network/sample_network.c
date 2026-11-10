#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "network_rt_opt.h"

#define GPIO_DM_OUTPUT       _IOW('G', 0, int)
#define GPIO_WRITE_LOW       _IOW('G', 4, int)
#define GPIO_WRITE_HIGH      _IOW('G', 5, int)

#define CTRL_WDT_SET_TIMEOUT _IOW('W', 2, int)
#define CTRL_WDT_START       _IOW('W', 5, int)

#define REMOTED_TCP_PORT     2222
#define REMOTED_UDP_PORT     2223
#define KERNEL_RAW_OFFSET    (10ULL * 1024ULL * 1024ULL)

typedef struct {
    unsigned short pin;
    unsigned short val;
} pin_gpio_t;

typedef struct {
    char ssid[64];
    char pass[64];
} wifi_cred_t;

static wifi_cred_t g_wifi_creds[4];
static int g_wifi_cred_count = 0;

static uint8_t g_mac[6] = {0};
static char g_mac_str[24] = "00:00:00:00:00:00";
static char g_device_name[80] = "SuperLexicon-K230";
static char g_node_privkey[96] = "";
static char g_current_ssid[64] = "";
static char g_current_ip[24] = "0.0.0.0";
static char g_current_gw[24] = "0.0.0.0";
static char g_current_mask[24] = "0.0.0.0";
static volatile int g_wifi_connected = 0;
static time_t g_boot_time = 0;
static FILE *g_log_fp = NULL;

static void remoted_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    printf("[k230_remoted] %s\n", buf);
    FILE *fp = fopen("/sdcard/remoted.log", "a");
    if (fp) {
        fprintf(fp, "[%ld] %s\n", (long)(time(NULL) - g_boot_time), buf);
        fclose(fp);
    }
}

static uint32_t calc_crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

static void trigger_hardware_reboot(void) {
    remoted_log("Triggering hardware watchdog reboot (power-cycling WIFI_3V3 for clean SDIO reset)...");
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    sync();
    int gfd = open("/dev/gpio", O_RDWR);
    if (gfd >= 0) {
        pin_gpio_t p45 = {.pin = 45, .val = 0};
        ioctl(gfd, GPIO_DM_OUTPUT, &p45);
        ioctl(gfd, GPIO_WRITE_LOW, &p45);
        usleep(150000);
        p45.val = 1;
        ioctl(gfd, GPIO_WRITE_HIGH, &p45);
        close(gfd);
    }
    const char *wdt_devs[2] = {"/dev/watchdog1", "/dev/watchdog0"};
    for (int i = 0; i < 2; i++) {
        int wfd = open(wdt_devs[i], O_RDWR);
        if (wfd >= 0) {
            unsigned int timeout = 1;
            ioctl(wfd, CTRL_WDT_SET_TIMEOUT, &timeout);
            ioctl(wfd, CTRL_WDT_START, NULL);
            sleep(3);
            close(wfd);
        }
    }
}

static void load_wifi_conf(void) {
    g_wifi_cred_count = 0;
    FILE *fp = fopen("/sdcard/wifi.conf", "r");
    if (fp) {
        char line[160];
        char cur_ssid[64] = "";
        char cur_pass[64] = "";
        while (fgets(line, sizeof(line), fp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (line[0] == '#' || line[0] == '\0') continue;
            if (strncmp(line, "SSID=", 5) == 0) {
                snprintf(cur_ssid, sizeof(cur_ssid), "%s", line + 5);
            } else if (strncmp(line, "PASS=", 5) == 0) {
                snprintf(cur_pass, sizeof(cur_pass), "%s", line + 5);
                if (cur_ssid[0] != '\0' && g_wifi_cred_count < 4) {
                    snprintf(g_wifi_creds[g_wifi_cred_count].ssid, sizeof(g_wifi_creds[0].ssid), "%s", cur_ssid);
                    snprintf(g_wifi_creds[g_wifi_cred_count].pass, sizeof(g_wifi_creds[0].pass), "%s", cur_pass);
                    g_wifi_cred_count++;
                    cur_ssid[0] = '\0';
                }
            }
        }
        fclose(fp);
    }

    if (g_wifi_cred_count == 0) {
        snprintf(g_wifi_creds[0].ssid, sizeof(g_wifi_creds[0].ssid), "mano");
        snprintf(g_wifi_creds[0].pass, sizeof(g_wifi_creds[0].pass), "");
        snprintf(g_wifi_creds[1].ssid, sizeof(g_wifi_creds[1].ssid), "");
        snprintf(g_wifi_creds[1].pass, sizeof(g_wifi_creds[1].pass), "");
        g_wifi_cred_count = 2;
    }
}

static void replace_json_string_field(const char *src, const char *key, const char *new_val, char *dst, size_t dst_sz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(src, pattern);
    if (!p) {
        snprintf(dst, dst_sz, "%s", src);
        return;
    }
    const char *colon = strchr(p + strlen(pattern), ':');
    if (!colon) {
        snprintf(dst, dst_sz, "%s", src);
        return;
    }
    const char *q1 = strchr(colon + 1, '"');
    if (!q1) {
        snprintf(dst, dst_sz, "%s", src);
        return;
    }
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) {
        snprintf(dst, dst_sz, "%s", src);
        return;
    }
    size_t prefix_len = (size_t)(q1 + 1 - src);
    if (prefix_len + strlen(new_val) + strlen(q2) + 1 >= dst_sz) {
        snprintf(dst, dst_sz, "%s", src);
        return;
    }
    memcpy(dst, src, prefix_len);
    dst[prefix_len] = '\0';
    strcat(dst, new_val);
    strcat(dst, q2);
}

static void provision_unique_device_identity(void) {
    char saved_mac[32] = "";
    char saved_key[80] = "";
    char saved_name[64] = "";

    FILE *id_fp = fopen("/sdcard/k230-device.id", "r");
    if (id_fp) {
        char line[160];
        while (fgets(line, sizeof(line), id_fp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (strncmp(line, "MAC=", 4) == 0) {
                snprintf(saved_mac, sizeof(saved_mac), "%s", line + 4);
            } else if (strncmp(line, "PRIVKEY=", 8) == 0) {
                snprintf(saved_key, sizeof(saved_key), "%s", line + 8);
            } else if (strncmp(line, "NAME=", 5) == 0) {
                snprintf(saved_name, sizeof(saved_name), "%s", line + 5);
            }
        }
        fclose(id_fp);
    }

    bool mac_valid = !(g_mac[0] == 0 && g_mac[1] == 0 && g_mac[2] == 0 &&
                       g_mac[3] == 0 && g_mac[4] == 0 && g_mac[5] == 0);

    if (saved_key[0] != '\0' && strlen(saved_key) == 64 &&
        (!mac_valid || strcmp(saved_mac, g_mac_str) == 0)) {
        snprintf(g_node_privkey, sizeof(g_node_privkey), "%s", saved_key);
        if (saved_name[0] != '\0') {
            snprintf(g_device_name, sizeof(g_device_name), "%s", saved_name);
        }
        remoted_log("Loaded existing identity: %s (MAC %s)", g_device_name, g_mac_str);
        return;
    }

    uint8_t entropy[32] = {0};
    int ufd = open("/dev/urandom", O_RDONLY);
    if (ufd >= 0) {
        int r = read(ufd, entropy, sizeof(entropy));
        (void)r;
        close(ufd);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t seed = ((uint64_t)tv.tv_sec << 32) ^ (uint64_t)tv.tv_usec ^ (uint64_t)getpid();
    for (int i = 0; i < 6; i++) {
        seed = (seed * 1099511628211ULL) ^ (uint64_t)g_mac[i];
    }

    uint8_t key_bytes[32];
    for (int i = 0; i < 32; i++) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        uint64_t mixed = seed * 2685821657736338717ULL;
        key_bytes[i] = (uint8_t)((mixed >> ((i % 8) * 8)) ^ entropy[i] ^ g_mac[i % 6]);
    }
    // Ensure non-zero valid secp256k1 scalar (< curve order)
    key_bytes[0] = (key_bytes[0] & 0x7F) | 0x10;
    if (mac_valid) {
        key_bytes[26] = g_mac[0];
        key_bytes[27] = g_mac[1];
        key_bytes[28] = g_mac[2];
        key_bytes[29] = g_mac[3];
        key_bytes[30] = g_mac[4];
        key_bytes[31] = g_mac[5];
    }

    for (int i = 0; i < 32; i++) {
        snprintf(g_node_privkey + (i * 2), 3, "%02x", key_bytes[i]);
    }
    g_node_privkey[64] = '\0';

    if (mac_valid) {
        snprintf(g_device_name, sizeof(g_device_name), "SuperLexicon-K230-%02X%02X",
                 g_mac[4], g_mac[5]);
    } else {
        snprintf(g_device_name, sizeof(g_device_name), "SuperLexicon-K230-%02X%02X",
                 key_bytes[30], key_bytes[31]);
    }

    FILE *out_id = fopen("/sdcard/k230-device.id", "w");
    if (out_id) {
        fprintf(out_id, "MAC=%s\nNAME=%s\nPRIVKEY=%s\n", g_mac_str, g_device_name, g_node_privkey);
        fflush(out_id);
        fclose(out_id);
    }

    // Update /sdcard/k230-config.json with unique private_key and app_name
    FILE *cfg_in = fopen("/sdcard/k230-config.json", "r");
    if (cfg_in) {
        char *cfg_buf = (char *)calloc(1, 16384);
        char *tmp_buf1 = (char *)calloc(1, 16384);
        char *tmp_buf2 = (char *)calloc(1, 16384);
        if (cfg_buf && tmp_buf1 && tmp_buf2) {
            size_t n = fread(cfg_buf, 1, 16383, cfg_in);
            cfg_buf[n] = '\0';
            fclose(cfg_in);
            cfg_in = NULL;

            replace_json_string_field(cfg_buf, "private_key", g_node_privkey, tmp_buf1, 16384);
            replace_json_string_field(tmp_buf1, "app_name", g_device_name, tmp_buf2, 16384);

            FILE *cfg_out = fopen("/sdcard/k230-config.json", "w");
            if (cfg_out) {
                fwrite(tmp_buf2, 1, strlen(tmp_buf2), cfg_out);
                fflush(cfg_out);
                fclose(cfg_out);
            }
        }
        if (cfg_buf) free(cfg_buf);
        if (tmp_buf1) free(tmp_buf1);
        if (tmp_buf2) free(tmp_buf2);
    }
    if (cfg_in) fclose(cfg_in);

    // Remove any pre-existing consensus.db from template image on first provisioning
    unlink("/sdcard/consensus.db");
    remoted_log("Provisioned unique device identity: %s (MAC %s)", g_device_name, g_mac_str);
}

static void read_thermal_info(int *out_level, double *out_temp) {
    *out_level = 0;
    *out_temp = 0.0;
    FILE *fp = fopen("/tmp/os_thermal_state", "r");
    if (fp) {
        if (fscanf(fp, "%d %lf", out_level, out_temp) != 2) {
            *out_level = 0;
            *out_temp = 0.0;
        }
        fclose(fp);
    }
}

static void write_wifi_status_file(void) {
    FILE *fp = fopen("/tmp/wifi_status.txt.tmp", "w");
    if (fp) {
        if (g_wifi_connected && strcmp(g_current_ip, "0.0.0.0") != 0 && g_current_ip[0] != '\0') {
            fprintf(fp, "CONNECTED %s %s %s\n", g_current_ip, g_current_ssid, g_mac_str);
        } else {
            fprintf(fp, "DISCONNECTED 0.0.0.0 %s\n", g_mac_str);
        }
        fclose(fp);
        unlink("/tmp/wifi_status.txt");
        rename("/tmp/wifi_status.txt.tmp", "/tmp/wifi_status.txt");
    }
}

static void build_status_json(char *out, size_t out_sz) {
    int t_level = 0;
    double t_temp = 0.0;
    read_thermal_info(&t_level, &t_temp);

    char boot_flags[128] = "roaming";
    FILE *bf = fopen("/tmp/slx_boot_flags.txt", "r");
    if (bf) {
        if (fgets(boot_flags, sizeof(boot_flags), bf)) {
            char *nl = strpbrk(boot_flags, "\r\n");
            if (nl) *nl = '\0';
        }
        fclose(bf);
    }

    struct stat st_slx, st_app, st_rad;
    long sz_slx = (stat("/sdcard/superlexicon.elf", &st_slx) == 0) ? (long)st_slx.st_size : -1;
    long sz_app = (stat("/sdcard/app.elf", &st_app) == 0) ? (long)st_app.st_size : -1;
    long sz_rad = (stat("/sdcard/lora_radio.elf", &st_rad) == 0) ? (long)st_rad.st_size : -1;

    snprintf(out, out_sz,
             "{\"device\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\",\"netmask\":\"%s\","
             "\"mac\":\"%s\",\"ssid\":\"%s\",\"port\":%d,\"uptime_s\":%ld,"
             "\"temp_c\":%.1f,\"thermal_level\":%d,\"mode\":\"%s\","
             "\"superlexicon_elf\":%ld,\"app_elf\":%ld,\"lora_radio_elf\":%ld}",
             g_device_name, g_current_ip, g_current_gw, g_current_mask,
             g_mac_str, g_current_ssid, REMOTED_TCP_PORT,
             (long)(time(NULL) - g_boot_time), t_temp, t_level, boot_flags,
             sz_slx, sz_app, sz_rad);
}

static void *udp_discovery_thread(void *arg) {
    (void)arg;
    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) return NULL;

    int opt = 1;
    setsockopt(ufd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    setsockopt(ufd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(REMOTED_UDP_PORT);
    bind(ufd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    time_t last_bcast = 0;
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ufd, &rfds);
        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
        int r = select(ufd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(ufd, &rfds)) {
            char rx[128] = {0};
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            ssize_t n = recvfrom(ufd, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&peer, &plen);
            if (n > 0 && strncmp(rx, "DISCOVER", 8) == 0) {
                char json[512];
                build_status_json(json, sizeof(json));
                sendto(ufd, json, strlen(json), 0, (struct sockaddr *)&peer, plen);
            } else if (n > 0 && strncmp(rx, "REBOOT", 6) == 0) {
                sendto(ufd, "OK REBOOTING\n", 13, 0, (struct sockaddr *)&peer, plen);
                usleep(150000);
                trigger_hardware_reboot();
            }
        }

        time_t now = time(NULL);
        if (g_wifi_connected && now - last_bcast >= 5) {
            last_bcast = now;
            char json[512];
            build_status_json(json, sizeof(json));

            struct sockaddr_in baddr;
            memset(&baddr, 0, sizeof(baddr));
            baddr.sin_family = AF_INET;
            baddr.sin_port = htons(REMOTED_UDP_PORT);
            baddr.sin_addr.s_addr = inet_addr("255.255.255.255");
            sendto(ufd, json, strlen(json), 0, (struct sockaddr *)&baddr, sizeof(baddr));

            // Also send to subnet broadcast (e.g. 192.168.50.255)
            unsigned int a = 0, b = 0, c = 0, d = 0;
            if (sscanf(g_current_ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a != 0) {
                char sub_bcast[32];
                snprintf(sub_bcast, sizeof(sub_bcast), "%u.%u.%u.255", a, b, c);
                baddr.sin_addr.s_addr = inet_addr(sub_bcast);
                sendto(ufd, json, strlen(json), 0, (struct sockaddr *)&baddr, sizeof(baddr));
            }
        }
    }
    close(ufd);
    return NULL;
}

static void *wifi_manager_thread(void *arg) {
    (void)arg;
    load_wifi_conf();

    int cred_idx = 0;
    while (1) {
        int status = 0;
        network_rt_wlan_isconnected(&status);
        if (status == 1) {
            char ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};
            if (get_net_ifconfig(0, ip, mask, gw, dns) == 0 &&
                ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                if (!g_wifi_connected || strcmp(g_current_ip, ip) != 0) {
                    snprintf(g_current_ip, sizeof(g_current_ip), "%s", ip);
                    snprintf(g_current_mask, sizeof(g_current_mask), "%s", mask);
                    snprintf(g_current_gw, sizeof(g_current_gw), "%s", gw);
                    g_wifi_connected = 1;
                    write_wifi_status_file();
                    remoted_log("Wi-Fi CONNECTED: SSID=%s IP=%s GW=%s MAC=%s",
                                g_current_ssid, g_current_ip, g_current_gw, g_mac_str);
                }
                sleep(4);
                continue;
            } else {
                set_net_ifconfig_dhcp(0);
                sleep(2);
                if (get_net_ifconfig(0, ip, mask, gw, dns) == 0 &&
                    ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                    snprintf(g_current_ip, sizeof(g_current_ip), "%s", ip);
                    snprintf(g_current_mask, sizeof(g_current_mask), "%s", mask);
                    snprintf(g_current_gw, sizeof(g_current_gw), "%s", gw);
                    g_wifi_connected = 1;
                    write_wifi_status_file();
                    remoted_log("Wi-Fi DHCP LEASED: SSID=%s IP=%s GW=%s",
                                g_current_ssid, g_current_ip, g_current_gw);
                    sleep(4);
                    continue;
                }
            }
        }

        g_wifi_connected = 0;
        write_wifi_status_file();

        wifi_cred_t *cred = &g_wifi_creds[cred_idx % g_wifi_cred_count];
        cred_idx++;
        remoted_log("Connecting to Wi-Fi SSID '%s'...", cred->ssid);
        int rc = network_rt_wlan_connect(cred->ssid, cred->pass);
        if (rc == 0) {
            snprintf(g_current_ssid, sizeof(g_current_ssid), "%s", cred->ssid);
            set_net_ifconfig_dhcp(0);
            for (int wait_i = 0; wait_i < 10; wait_i++) {
                sleep(1);
                char ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};
                if (get_net_ifconfig(0, ip, mask, gw, dns) == 0 &&
                    ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                    snprintf(g_current_ip, sizeof(g_current_ip), "%s", ip);
                    snprintf(g_current_mask, sizeof(g_current_mask), "%s", mask);
                    snprintf(g_current_gw, sizeof(g_current_gw), "%s", gw);
                    g_wifi_connected = 1;
                    write_wifi_status_file();
                    remoted_log("Wi-Fi READY: SSID=%s IP=%s GW=%s MAC=%s",
                                g_current_ssid, g_current_ip, g_current_gw, g_mac_str);
                    break;
                }
            }
        } else {
            remoted_log("Wi-Fi connect to '%s' returned %d; retrying...", cred->ssid, rc);
            sleep(3);
        }
    }
    return NULL;
}

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_line(int fd, char *line, size_t max_len) {
    size_t pos = 0;
    while (pos + 1 < max_len) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {.tv_sec = 45, .tv_usec = 0};
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            return -1;
        }
        char c = 0;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') {
            line[pos] = '\0';
            if (pos > 0 && line[pos - 1] == '\r') {
                line[pos - 1] = '\0';
            }
            return (int)pos;
        }
        line[pos++] = c;
    }
    line[pos] = '\0';
    return (int)pos;
}

static void handle_client_session(int cfd) {
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char cmd_line[512];
    while (recv_line(cfd, cmd_line, sizeof(cmd_line)) >= 0) {
        if (cmd_line[0] == '\0') continue;

        if (strcmp(cmd_line, "PING") == 0) {
            char resp[160];
            snprintf(resp, sizeof(resp), "OK PONG %s %s %s\n", g_device_name, g_current_ip, g_mac_str);
            send_all(cfd, resp, strlen(resp));
        } else if (strcmp(cmd_line, "STATUS") == 0) {
            char json[640];
            build_status_json(json, sizeof(json));
            char resp[700];
            snprintf(resp, sizeof(resp), "OK %s\n", json);
            send_all(cfd, resp, strlen(resp));
        } else if (strncmp(cmd_line, "LS", 2) == 0) {
            char dir_path[256] = "/sdcard";
            if (strlen(cmd_line) > 3) {
                sscanf(cmd_line + 3, "%255s", dir_path);
            }
            DIR *d = opendir(dir_path);
            if (!d) {
                char err[320];
                snprintf(err, sizeof(err), "ERR cannot open %s\n", dir_path);
                send_all(cfd, err, strlen(err));
                continue;
            }
            send_all(cfd, "OK\n", 3);
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                char full[600];
                snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
                struct stat st;
                long sz = -1;
                long mt = 0;
                if (stat(full, &st) == 0) {
                    sz = (long)st.st_size;
                    mt = (long)st.st_mtime;
                }
                char entry[384];
                snprintf(entry, sizeof(entry), "%s\t%ld\t%ld\n", de->d_name, sz, mt);
                send_all(cfd, entry, strlen(entry));
            }
            closedir(d);
            send_all(cfd, "END\n", 4);
        } else if (strncmp(cmd_line, "GET ", 4) == 0) {
            char path[256] = {0};
            long offset = 0;
            long max_bytes = 0;
            int parsed = sscanf(cmd_line + 4, "%255s %ld %ld", path, &offset, &max_bytes);
            if (parsed < 1) {
                send_all(cfd, "ERR usage: GET <path> [offset] [max_bytes]\n", 43);
                continue;
            }
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                char err[300];
                snprintf(err, sizeof(err), "ERR cannot open %s\n", path);
                send_all(cfd, err, strlen(err));
                continue;
            }
            struct stat st;
            long fsize = 0;
            if (stat(path, &st) == 0 && st.st_size >= 0) {
                fsize = (long)st.st_size;
            } else {
                fseek(fp, 0, SEEK_END);
                fsize = ftell(fp);
            }
            if (fsize < 0) fsize = 0;
            if (offset < 0) {
                offset = fsize + offset;
                if (offset < 0) offset = 0;
            }
            if (offset > fsize) offset = fsize;
            long to_read = fsize - offset;
            if (max_bytes > 0 && to_read > max_bytes) {
                to_read = max_bytes;
            }
            fseek(fp, offset, SEEK_SET);

            uint32_t crc = 0;
            uint8_t *chunk = (uint8_t *)malloc(16384);
            if (!chunk) {
                fclose(fp);
                send_all(cfd, "ERR OOM\n", 8);
                continue;
            }
            long rem = to_read;
            while (rem > 0) {
                size_t step = (rem > 16384) ? 16384 : (size_t)rem;
                size_t got = fread(chunk, 1, step, fp);
                if (got == 0) break;
                crc = calc_crc32_update(crc, chunk, got);
                rem -= (long)got;
            }
            long actual_len = to_read - rem;
            fseek(fp, offset, SEEK_SET);

            char hdr[128];
            snprintf(hdr, sizeof(hdr), "OK %ld %08x\n", actual_len, crc);
            send_all(cfd, hdr, strlen(hdr));

            rem = actual_len;
            while (rem > 0) {
                size_t step = (rem > 16384) ? 16384 : (size_t)rem;
                size_t got = fread(chunk, 1, step, fp);
                if (got == 0) break;
                if (send_all(cfd, chunk, got) != 0) break;
                rem -= (long)got;
            }
            free(chunk);
            fclose(fp);
        } else if (strncmp(cmd_line, "PUT ", 4) == 0) {
            char path[256] = {0};
            long total_size = -1;
            if (sscanf(cmd_line + 4, "%255s %ld", path, &total_size) != 2 || total_size < 0) {
                send_all(cfd, "ERR usage: PUT <path> <size>\n", 29);
                continue;
            }
            char ota_path[280];
            char old_path[280];
            snprintf(ota_path, sizeof(ota_path), "%s.ota", path);
            snprintf(old_path, sizeof(old_path), "%s.old", path);
            unlink(ota_path);

            FILE *fp = fopen(ota_path, "wb");
            if (!fp) {
                char err[320];
                snprintf(err, sizeof(err), "ERR cannot create %s\n", ota_path);
                send_all(cfd, err, strlen(err));
                continue;
            }
            send_all(cfd, "READY\n", 6);

            uint8_t *chunk = (uint8_t *)malloc(16384);
            uint32_t crc = 0;
            long rcvd = 0;
            bool ok = true;
            while (rcvd < total_size) {
                size_t want = (total_size - rcvd > 16384) ? 16384 : (size_t)(total_size - rcvd);
                ssize_t n = recv(cfd, chunk, want, 0);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
                    ok = false;
                    break;
                }
                crc = calc_crc32_update(crc, chunk, (size_t)n);
                rcvd += (long)n;
            }
            free(chunk);
            fflush(fp);
            fclose(fp);

            if (!ok || rcvd != total_size) {
                unlink(ota_path);
                send_all(cfd, "ERR incomplete transfer\n", 24);
                break;
            }

            // Replace target file (if locked on FAT32 because running, move target to .old first)
            unlink(old_path);
            if (unlink(path) != 0) {
                rename(path, old_path);
                unlink(old_path);
            }
            if (rename(ota_path, path) != 0) {
                // Fallback direct copy if rename across locked entry fails
                FILE *src = fopen(ota_path, "rb");
                FILE *dst = fopen(path, "wb");
                if (src && dst) {
                    uint8_t *cbuf = (uint8_t *)malloc(16384);
                    size_t got;
                    while ((got = fread(cbuf, 1, 16384, src)) > 0) {
                        fwrite(cbuf, 1, got, dst);
                    }
                    free(cbuf);
                    fflush(dst);
                    fclose(dst);
                    fclose(src);
                    unlink(ota_path);
                } else {
                    if (src) fclose(src);
                    if (dst) fclose(dst);
                    send_all(cfd, "ERR rename failed\n", 18);
                    continue;
                }
            }
            remoted_log("PUT %s (%ld bytes, crc=%08x) complete", path, total_size, crc);
            char resp[128];
            snprintf(resp, sizeof(resp), "OK %ld %08x\n", total_size, crc);
            send_all(cfd, resp, strlen(resp));
        } else if (strncmp(cmd_line, "PUT_KERNEL ", 11) == 0) {
            long total_size = -1;
            if (sscanf(cmd_line + 11, "%ld", &total_size) != 1 || total_size <= 0 || total_size > 32 * 1024 * 1024) {
                send_all(cfd, "ERR invalid kernel size\n", 24);
                continue;
            }
            int kfd = open("/dev/sd1", O_RDWR);
            if (kfd < 0) {
                kfd = open("/dev/mmcblk0", O_RDWR);
            }
            if (kfd < 0) {
                send_all(cfd, "ERR cannot open /dev/sd1 raw block device\n", 42);
                continue;
            }
            if (lseek(kfd, (off_t)KERNEL_RAW_OFFSET, SEEK_SET) != (off_t)KERNEL_RAW_OFFSET) {
                close(kfd);
                send_all(cfd, "ERR lseek 10MiB failed\n", 23);
                continue;
            }
            send_all(cfd, "READY\n", 6);

            size_t blk_sz = 4096;
            uint8_t *blk = (uint8_t *)malloc(blk_sz);
            uint32_t crc = 0;
            long rcvd = 0;
            bool ok = true;
            while (rcvd < total_size) {
                memset(blk, 0, blk_sz);
                size_t want = (total_size - rcvd > (long)blk_sz) ? blk_sz : (size_t)(total_size - rcvd);
                size_t blk_got = 0;
                while (blk_got < want) {
                    ssize_t n = recv(cfd, blk + blk_got, want - blk_got, 0);
                    if (n <= 0) {
                        ok = false;
                        break;
                    }
                    blk_got += (size_t)n;
                }
                if (!ok) break;
                crc = calc_crc32_update(crc, blk, want);
                size_t write_len = ((want + 511) / 512) * 512;
                if (write(kfd, blk, write_len) != (ssize_t)write_len) {
                    ok = false;
                    break;
                }
                rcvd += (long)want;
            }
            free(blk);
            close(kfd);

            if (!ok || rcvd != total_size) {
                send_all(cfd, "ERR raw kernel write failed\n", 28);
                break;
            }
            remoted_log("PUT_KERNEL (%ld bytes, crc=%08x) flashed to /dev/sd1 @ 10MiB", total_size, crc);
            char resp[128];
            snprintf(resp, sizeof(resp), "OK %ld %08x\n", total_size, crc);
            send_all(cfd, resp, strlen(resp));
        } else if (strncmp(cmd_line, "RM ", 3) == 0) {
            char path[256] = {0};
            if (sscanf(cmd_line + 3, "%255s", path) == 1) {
                if (unlink(path) == 0) {
                    send_all(cfd, "OK\n", 3);
                } else {
                    send_all(cfd, "ERR unlink failed\n", 18);
                }
            }
        } else if (strncmp(cmd_line, "WIFI ", 5) == 0) {
            char ssid[64] = {0}, pass[64] = {0};
            if (sscanf(cmd_line + 5, "%63s %63s", ssid, pass) == 2) {
                FILE *wf = fopen("/sdcard/wifi.conf", "w");
                if (wf) {
                    fprintf(wf, "SSID=%s\nPASS=%s\nSSID=\nPASS=\n", ssid, pass);
                    fclose(wf);
                }
                load_wifi_conf();
                g_wifi_connected = 0;
                send_all(cfd, "OK\n", 3);
            } else {
                send_all(cfd, "ERR usage: WIFI <ssid> <pass>\n", 30);
            }
        } else if (strncmp(cmd_line, "MODE ", 5) == 0) {
            char mode[32] = {0};
            char subnet[32] = "00000001";
            long long freq = 915000000LL;
            sscanf(cmd_line + 5, "%31s %31s %lld", mode, subnet, &freq);
            if (strcmp(mode, "join") == 0 || strcmp(mode, "validator") == 0) {
                FILE *bf = fopen("/tmp/slx_boot_flags.txt", "w");
                if (bf) {
                    fprintf(bf, "--join-validator --subnet %s --freq-hz %lld\n", subnet, freq);
                    fclose(bf);
                }
                FILE *uc = fopen("/tmp/slx_ui_cmd.json", "w");
                if (uc) {
                    fprintf(uc, "{\"action\":\"join\",\"subnet\":\"%s\",\"freq_hz\":%lld}\n", subnet, freq);
                    fclose(uc);
                }
                send_all(cfd, "OK mode=join\n", 13);
            } else {
                FILE *bf = fopen("/tmp/slx_boot_flags.txt", "w");
                if (bf) {
                    fprintf(bf, "--roaming\n");
                    fclose(bf);
                }
                FILE *uc = fopen("/tmp/slx_ui_cmd.json", "w");
                if (uc) {
                    fprintf(uc, "{\"action\":\"roaming\"}\n");
                    fclose(uc);
                }
                send_all(cfd, "OK mode=roaming\n", 16);
            }
        } else if (strcmp(cmd_line, "REBOOT") == 0) {
            send_all(cfd, "OK REBOOTING\n", 13);
            close(cfd);
            usleep(200000);
            trigger_hardware_reboot();
            return;
        } else if (strcmp(cmd_line, "QUIT") == 0) {
            send_all(cfd, "OK BYE\n", 7);
            break;
        } else {
            send_all(cfd, "ERR unknown command\n", 20);
        }
    }
    close(cfd);
}

static void *client_session_worker(void *arg) {
    int cfd = (int)(intptr_t)arg;
    handle_client_session(cfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Let app.elf finish LVGL & home screen initialization without SD card FatFs contention */
    if (!(argc > 1 && strcmp(argv[1], "--provision") == 0)) {
        sleep(8);
    }
    unlink("/tmp/load_temp");
    g_boot_time = time(NULL);

    /* Provision unique device identity immediately on first boot before Wi-Fi init */
    provision_unique_device_identity();

    if (argc > 1 && strcmp(argv[1], "--provision") == 0) {
        return 0;
    }

    /* Spawn clean single-threaded delayed launcher for lora_radio.elf & superlexicon.elf
     * BEFORE opening Wi-Fi/socket FDs so children inherit zero network handles and never
     * contend with app.elf or k230_remoted.elf during boot. */
    {
        pid_t bg_pid = fork();
        if (bg_pid == 0) {
            sleep(12);
            chdir("/sdcard");
            struct stat st;
            if (stat("/sdcard/lora_radio.elf", &st) == 0) {
                pid_t rpid = fork();
                if (rpid == 0) {
                    char *rargv[] = {"/sdcard/lora_radio.elf", "ipc:///tmp/slx_ipc.bin", NULL};
                    execv("/sdcard/lora_radio.elf", rargv);
                    _exit(127);
                }
                sleep(3);
            }
            if (stat("/sdcard/superlexicon.elf", &st) == 0) {
                char *sargv[] = {
                    "/sdcard/superlexicon.elf",
                    "--config",
                    "/sdcard/k230-config.json",
                    "--roaming",
                    NULL
                };
                execv("/sdcard/superlexicon.elf", sargv);
            }
            _exit(0);
        }
    }

    /* U-Boot already pulls IO45_WIFI_EN (GPIO45) HIGH so U24 (WIFI_3V3) powers RTL8189 continuously from boot.
     * NEVER call ioctl(GPIO_DM_OUTPUT) on GPIO45 here, as DATA_OUTPUT defaults to 0 and glitches WIFI_3V3 LOW! */
    sleep(2);

    if (network_rt_init() < 0) {
        remoted_log("WARN: first network_rt_init failed, retrying in 2s...");
        sleep(2);
        network_rt_init();
    }

    for (int i = 0; i < 5; i++) {
        if (wlan_get_mac(g_mac) == 0 &&
            !(g_mac[0] == 0 && g_mac[1] == 0 && g_mac[2] == 0)) {
            break;
        }
        usleep(300000);
    }
    snprintf(g_mac_str, sizeof(g_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    pthread_t wifi_tid, udp_tid;
    pthread_create(&wifi_tid, NULL, wifi_manager_thread, NULL);
    pthread_detach(wifi_tid);

    pthread_create(&udp_tid, NULL, udp_discovery_thread, NULL);
    pthread_detach(udp_tid);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        remoted_log("FATAL: socket() failed");
        return 1;
    }
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(REMOTED_TCP_PORT);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        remoted_log("FATAL: bind(:%d) failed", REMOTED_TCP_PORT);
        close(sfd);
        return 1;
    }
    if (listen(sfd, 4) < 0) {
        remoted_log("FATAL: listen(:%d) failed", REMOTED_TCP_PORT);
        close(sfd);
        return 1;
    }

    remoted_log("Listening for remote OTA/control on TCP port %d (UDP discovery %d)",
                REMOTED_TCP_PORT, REMOTED_UDP_PORT);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cfd = accept(sfd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cfd < 0) {
            usleep(100000);
            continue;
        }
        pthread_t ctid;
        if (pthread_create(&ctid, NULL, client_session_worker, (void *)(intptr_t)cfd) == 0) {
            pthread_detach(ctid);
        } else {
            handle_client_session(cfd);
        }
    }

    close(sfd);
    return 0;
}

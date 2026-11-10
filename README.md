<!--

 * @Description: None
 * @version: V1.0.0
 * @Author: LILYGO
 * @Date: 2025-02-20 16:13:14
 * @LastEditors: Please set LastEditors
 * @LastEditTime: 2025-06-17 09:00:00
 * @License: GPL 3.0
-->
<h1 align = "center">T-Display-K230_canmv_rt</h1>

<p align="center" width="100%">
    <img src="image/k230_display_product.png" alt="T-Display K230 development board">
</p>

**English**


## Version iteration:
| Version                              | Update date                       |Update description|
| :-------------------------------: | :-------------------------------: | :-------------------------------: |
| T-Display-K230_canmv_rt_V1.0     | 2025-02-20      |Original version      |
| T-Display-K230_canmv_rt_V1.1     | 2025-03-25       | update lora model（sx1262） |
| T-Display-K230_canmv_rt_V1.2     | 2025-06-17       |  add ble  |
| T-Display-K230_canmv_rt_V1.3     | 2026-02-10       | add lr2021 battery keyboard gps lte-m fan speaker aht20  |
| T-Display-K230_canmv_rt_V1.4     | 2026-11-10       | RT-Smart kernel/LWP fixes, thermal DVFS, Pocket Standby, dynamic Landscape/Portrait rotation, rounded AMOLED safe areas, and Wi-Fi OTA daemon |

## About This Fork (SuperLexicon / LexiOS Edition)

This repository is an enhanced fork of [`Xinyuan-LilyGO/T-Display-K230_canmv_rt`](https://github.com/Xinyuan-LilyGO/T-Display-K230_canmv_rt) for the **LilyGO T-Display-K230** (`Kendryte K230` Dual-Core RISC-V, `RM69A10` 4.1" 568×1232 AMOLED, `GT9895` capacitive touch, `TCA8418` QWERTY keypad + `BQ27220` fuel gauge + `SY6970` PMIC + `AHT20` on `I2C4`, `Lierda LR2021` Dual-Band Sub-GHz/2.4 GHz LoRa/FLRC, `RTL8189FS` SDIO Wi-Fi, `TESEO-LIV3FL` GNSS, `SIM7080G` LTE-M, `ES8311` audio).

It resolves critical upstream RT-Smart kernel/driver bugs and adds full-screen Portrait/Landscape rotation, rounded AMOLED safe-area insets, thermal & power management, native hardware apps, and wireless OTA deployment.

### 1. Improvements & Fixes to Existing Upstream Features

| Subsystem | Upstream Issue (`Xinyuan-LilyGO` V1.3) | Fix / Improvement in This Fork | Key Files |
| :--- | :--- | :--- | :--- |
| **RT-Smart Kernel ELF Loader (`lwp_load`)** | Staged ELFs up to `21.33 MB` (`CONFIG_MEM_RTSMART_HEAP_SIZE / 3`) into `/tmp/load_temp` (`dfs_tmpfs`), which calls `rt_realloc()` in `64 KB` chunks on the `64 MB` kernel heap (`0x4000000`). ELFs between `16 MB` and `21.33 MB` exhausted contiguous kernel heap during `rt_realloc()`, leaving `fd` at a partial offset without unlinking `/tmp/load_temp` or rewinding `fd` to `0`. | Capped `/tmp/load_temp` staging to ELFs `<= 4 MB` (`CONFIG_MEM_RTSMART_HEAP_SIZE >= len * 8`), and added `unlink(tmp_file_name)` + `lseek(fd, 0, SEEK_SET)` in `_temp_failed:` so fallback demand-loading always succeeds. *(Note: keeping `app.elf > 21.33 MB` also bypasses `/tmp/load_temp` on pre-built stock kernels).* | [`lwp.c`](canmv_k230/src/rtsmart/rtsmart/kernel/rt-thread/components/lwp/lwp.c) |
| **RT-Smart Kernel Syscalls & `mmap`** | Missing `MAP_ANONYMOUS` + `PROT_EXEC` page allocation and `/dev/urandom`, preventing modern runtimes (Go, Rust, JITs) from starting on RT-Smart. | Added `PROT_EXEC` anonymous `mmap` support in [`lwp_user_mm.c`](canmv_k230/src/rtsmart/rtsmart/kernel/rt-thread/components/lwp/lwp_user_mm.c), RISC-V I/D-cache coherency syscalls in [`lwp_syscall.c`](canmv_k230/src/rtsmart/rtsmart/kernel/rt-thread/components/lwp/lwp_syscall.c), and hardware TRNG + ChaCha20 `/dev/urandom` & `/dev/random` in [`drv_ts.c`](canmv_k230/src/rtsmart/rtsmart/kernel/bsp/maix3/drivers/interdrv/tsensor/drv_ts.c). | [`lwp_user_mm.c`](canmv_k230/src/rtsmart/rtsmart/kernel/rt-thread/components/lwp/lwp_user_mm.c), [`lwp_syscall.c`](canmv_k230/src/rtsmart/rtsmart/kernel/rt-thread/components/lwp/lwp_syscall.c), [`drv_ts.c`](canmv_k230/src/rtsmart/rtsmart/kernel/bsp/maix3/drivers/interdrv/tsensor/drv_ts.c) |
| **Die Temperature Sensor (`/dev/tsensor`) & MPP Power Domains** | Uncalibrated raw register reads and all K230 AI/KPU/VPU/DPU power domains left powered on at boot, causing high idle heat (~65°C+). | Fixed K230 tsensor trim/calibration conversion in [`drv_ts.c`](canmv_k230/src/rtsmart/rtsmart/kernel/bsp/maix3/drivers/interdrv/tsensor/drv_ts.c) and exposed `pm_core` power-domain gating in [`pm_core.c`](canmv_k230/src/rtsmart/mpp/kernel/pm/src/pm_core.c) to power-gate unused AI/3D/Video domains at startup. | [`drv_ts.c`](canmv_k230/src/rtsmart/rtsmart/kernel/bsp/maix3/drivers/interdrv/tsensor/drv_ts.c), [`pm_core.c`](canmv_k230/src/rtsmart/mpp/kernel/pm/src/pm_core.c) |
| **Shared `I2C4` Bus (`SY6970`, `BQ27220`, `TCA8418`, `AHT20`)** | Multiple modules repeatedly opened/closed `/dev/i2c4` per poll cycle, and `BQ27220` data-memory NVM writes ran inside the LVGL timer, causing I2C bus lockups and UI freezes. | Consolidated all `/dev/i2c4` transactions (`0x6A`, `0x55`, `0x34`, `0x38`) onto a single persistent file descriptor (`s_fd_i2c4`) in [`keyboard_ext.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/keyboard_ext.c) and made fuel-gauge/PMIC polling completely non-blocking. Updated `Battery` and `Setting` apps to use the shared driver. | [`keyboard_ext.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/keyboard_ext.c), [`Battery.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/battery/Battery.cpp), [`Setting.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/setting/Setting.cpp) |
| **Touchpad (`GT9895`) & Keypad (`TCA8418`) Input Registration** | Registering `keypad_init()` before `touchpad_init()` caused `ESP_Brookesia_Phone` to bind the wrong default pointer device, breaking touch scrolling and gestures. | Registered `LV_INDEV_TYPE_POINTER` (`touchpad_init()`) first in [`lv_port_indev.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/port/lv_port_indev.c), added 0°/90°/180°/270° coordinate remapping, and wired touch/key events into the OS idle/wake state machine. | [`lv_port_indev.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/port/lv_port_indev.c) |
| **Display Pipeline (`RM69A10` 568×1232) & Landscape Mode** | Hardcoded to `568×1232` Portrait with a `480×480` wallpaper; rotating to Landscape left half the screen black (`568 px` clip) and apps remained stuck at Portrait dimensions. | Added hardware VO rotation (`vo_Reset_Rotation()` in [`cap_vio.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/cap_vio.c) & [`lv_port_disp.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/port/lv_port_disp.c)), RISC-V D-Cache flush ([`rt_cache.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/rt_cache.c)), a calibrated `1232×568` Landscape `ESP_Brookesia_PhoneStylesheet_t`, and `ESP_Brookesia_CoreManager::recalibrateAppsVisualArea()` so apps dynamically resize between `568×1176` and `1232×512`. | [`cap_vio.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/cap_vio.c), [`lv_port_disp.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/port/lv_port_disp.c), [`esp_brookesia_core_manager.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/core/esp_brookesia_core_manager.cpp) |
| **Rounded AMOLED Corner Truncation** | The 4.1" `RM69A10` glass panel has ~`54 px` physical rounded corners and a 3-pixel MIPI DSI horizontal mask (`PTLAR_H = 3..564`), which clipped the top-right battery icon, top-left status bar, and bottom corners (where LVGL `LV_USE_PERF_MONITOR` / `LV_USE_MEM_MONITOR` overlays were drawn). | Disabled LVGL bottom-corner debug monitors in [`lv_conf.h`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lvgl/lv_conf.h), increased StatusBar height to `56 px`, inset top-left and top-right status bar elements by `52–66 px`, and added horizontal safe-area margins (`44 px` Landscape / `18–20 px` Portrait). | [`sample_display.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/sample_display.cpp), [`lv_conf.h`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lvgl/lv_conf.h) |
| **ElmFatFs Boot Contention** | RT-Smart mounts `/sdcard` with ElmFatFs `_FS_REENTRANT = 0`; launching multiple large ELF binaries simultaneously in `init.sh` corrupted directory traversal during `user_gui_init`. | Simplified boot `init.sh` to start `app.elf` immediately while `k230_remoted.elf` waits 8 seconds for GUI init to finish before spawning background services sequentially. | [`sample_network.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_network/sample_network.c) |

---

### 2. New Features Added in This Fork

1. **System-Wide Quick Settings Pull-Down Drawer ([`sample_display.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/sample_display.cpp)):**
   - Accessible from any screen via the top-left status bar pill (`LexiOS ▾`) or a short press on the physical `'0'` button (`GPIO0` / `SW3`).
   - Provides live `BQ27220` battery % and K230 die temperature readouts, interactive **Display Brightness** slider, **Orientation Toggle** (`Portrait 568×1232` ↔ `Landscape 1232×568`), **Auto-Sleep Timeout** selector (`1m`, `2m`, `5m`, `Never`), **Keyboard Backlight** toggle, one-tap **Pocket Standby** screen blanking, and two-step confirmed **Hardware Power Off** (`SY6970` `BATFET_DIS` Ship Mode).
2. **Physical `'0'` Side Button (`GPIO0` / `SW3`) Multi-Function Controller ([`sample_display.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/sample_display.cpp)):**
   - **Short press (`< 1.5s`):** Wakes the display from Pocket Standby, or toggles the Quick Settings drawer when awake.
   - **Medium hold (`1.5s – 3.9s`):** Cycles screen orientation between Portrait (`568×1232`) and Landscape (`1232×568`) and persists the choice to `/sdcard/display_rot.conf`.
   - **Long hold (`>= 4.0s`):** Executes a clean filesystem `sync()` and powers off the device completely via `SY6970` `REG09` `BATFET_DIS` (Ship Mode).
3. **6-Stage Thermal DVFS & Pocket Standby Power Manager ([`sample_display.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/sample_display.cpp)):**
   - Continuously monitors `/dev/tsensor` and automatically scales CPU1 DVFS frequency (`1600 MHz` → `1200 MHz` → `800 MHz` → `594 MHz` → `400 MHz` → `200 MHz`) and active cooling fan (`GPIO12`) across 6 thermal thresholds.
   - Automatically dims the AMOLED panel prior to sleep and blanks `RM69A10` in **Pocket Standby** while preserving `PM_GOVERNOR_AUTO` so `RTL8189FS` SDIO Wi-Fi remains connected in standby.
4. **New Native LexiOS Hardware & Radio Applications ([`lexios_apps.cpp`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lexios_apps.cpp)):**
   - **`LexiSystem` (`System & Power`):** Real-time dashboard for K230 die temperature, 6-stage DVFS thermal state, `BQ27220` battery pack metrics (`mV`, `mA`, `SoC %`, remaining/full `mAh`, temperature), `SY6970` charger state, cooling fan override, and Ship Mode power-off.
   - **`LexiSat` (`GNSS & Cellular`):** Live `TESEO-LIV3FL` GNSS receiver telemetry (fix status, satellites in view, latitude/longitude, altitude, speed, HDOP) and `SIM7080G` Cat-M1 / NB-IoT modem diagnostics.
   - **`LexiRF` (`LR2021 Sub-GHz / 2.4 GHz Radio Analyzer`):** Interactive `Lierda LR2021` transceiver console supporting Sub-GHz (`915 MHz`) and `2.4 GHz` LoRa / FLRC modulation, RSSI/SNR live monitoring, packet sniffer log, and periodic test beacon TX (with responsive Portrait & 2-column Landscape layouts).
   - **`LexiPTT` (`LoRa / FLRC Digital Walkie-Talkie`):** Half-duplex Push-To-Talk digital voice transceiver over `LR2021` high-bitrate FLRC + `ES8311` audio codec, featuring channel selection, TX lock mode, squelch control, and live audio VU bar.
5. **Multi-Threaded Wi-Fi OTA & Remote Management Daemon ([`sample_network.c`](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_network/sample_network.c) → `k230_remoted.elf`):**
   - Connects automatically using `/sdcard/wifi.conf`, broadcasts UDP discovery beacons on port `2223`, and serves a multi-threaded TCP control/OTA protocol on port `2222` (`STATUS`, `PING`, `PUT <path> <size>`, `GET <path>`, `CRC32 <path>`, `LOG <path>`, `EXEC`, `REBOOT`).
   - Pulses `GPIO45` (`WIFI_3V3`) LOW for `150 ms` and HIGH right before hardware watchdog reset (`/dev/watchdog1`) so the `RTL8189FS` SDIO Wi-Fi chip cleanly power-cycles and re-enumerates after every wireless OTA reboot without needing a physical `RST` press.
## PurchaseLink

| Product                     | SOC           |  FLASH  |  LPDDR   | Link                   |
| :------------------------: | :-----------: |:-------: | :---------: | :------------------: |
| T-Display-K230_canmv_rt| k230 |   SD Card   | 1GiB | [LILYGO Mall](https://lilygo.cc/products/t-display-k230?_pos=2&_psq=k230&_ss=e&_v=1.0&variant=45197449953461)  |

## Directory
- [Describe](#describe)
- [Preview](#preview)
- [Module](#module)
- [QuickStart](#quickstart)
- [PinOverview](#pinoverview)
- [RelatedTests](#RelatedTests)
- [FAQ](#faq)
- [Project](#project)
- [Information](#information)
- [DependentLibraries](#dependentlibraries)

## Description

T-Display-K230 is a development board featuring a high-definition AMOLED display, based on the k230. <!--designed for standalone battery connectivity.-->

### Preview

#### Actual Product Image

<p align="center" width="100%">
    <img src="image/k230_display_product.png" alt="">
</p>


## Module

### 1. MCU

* Chip: k230
* For more details, please visit    [k230 Datasheet](datasheet/K230_datasheet.pdf)

### 2. Screen

* Size: 4.1-inch AMOLED  screen
* Resolution: 568x1232px
* Screen type: AMOLED
* Driver chip: RM69A10

### 3. Touch

* Chip: GT9895
* Bus communication protocol: IIC

## QuickStart

### Examples Support



|                    Firmware                                                |                                    Description                               |
| ---------------------------------------------------------------------------| -----------------------------------------------------------------------------|
|   [firmware_V1.2](firmware/CanMV-K230-V3P0_rtsmart_releaseV1.2.zip)        |   update touch  add ble                                                      |
|   [firmware_V1.3](firmware/CanMV-K230-V3P0_rtsmart_releaseV1.3.zip)        |   update i2c  add lr2021 battery keyboard gps lte-m fan speaker aht20        |

|             app_name             |             Description                 |                   path                 |
|----------------------------------| ----------------------------------------| -------------------------------------- |
|  lora                            |   sx1262/lr2021  send and receive                      |     [lora](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/lora)                                   |
|  clock                            |   stopwatch timer                      |     [clock](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/clock)                                   |
|  camera                            |support three  camera    preview                   |     [camera](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/camera)                                   |
|  Face_detect                            | face detect by camera                  |     [Face_detect](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/camera_face_detect)                                   |
|  Setting                            | 1.control screen backlight2.connect wifi3.see version                 |     [setting](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/setting)                                   |
|  music                            | play music    Listen to the sound through the headphones             |     [music](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/music)                                   |
|  calculator                            |   calc（+—*/%）          |     [calculator](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/calculator)                                   |
|  led                            |   control corlor led          |     [led](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/led)                                   |
|  Artboard                            |   test screen color/touch          |     [artboard](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/artboard)                                   |
|  Ble                            |   test ble（send/receive）  commit to your phone by bluebooth          |     [ble](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/ble)                                   |
|  Video                            |   Record and play videos through camera capture         |     [video](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/video)                                   |
|  Keyboard                           |   Inputting text through the keyboard and backlight control        |     [keyboard](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/keyboard)                                   |
|  LiveCall                           |    Realize remote video transmission through LR2021       |     [Livecall](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/live_call)                                   |
|  Gnss                           |     GPS satellite positioning      |     [gnss](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/gnss)                                   |
|  Cellular                           |     SIM information confirmation and acquisition      |     [cellular](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/cellular)                                   |
|  Battery                           |    Monitor the charging and discharging as well as the battery's power level information     |     [battery](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/battery)                                   |
|  game_2048                           |    2048 game     |     [game_2048](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/ui_brookesia/src/app_examples/phone/game_2048)                                   |
|  **LexiSystem** *(New)*            |    6-stage CPU DVFS thermal monitor, BQ27220 fuel gauge, SY6970 charger & Ship Mode power-off | [lexios_apps.cpp](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lexios_apps.cpp) |
|  **LexiSat** *(New)*               |    TESEO-LIV3FL GNSS satellite fix & SIM7080G Cat-M1/NB-IoT cellular diagnostics | [lexios_apps.cpp](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lexios_apps.cpp) |
|  **LexiRF** *(New)*                |    Dual-band Lierda LR2021 Sub-GHz / 2.4 GHz LoRa & FLRC radio analyzer and beacon transceiver | [lexios_apps.cpp](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lexios_apps.cpp) |
|  **LexiPTT** *(New)*               |    LR2021 FLRC + ES8311 digital Push-To-Talk walkie-talkie with live VU meter | [lexios_apps.cpp](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_display/lexios_apps.cpp) |
|  **k230_remoted** *(New)*          |    Multi-threaded TCP/UDP Wi-Fi OTA deployment daemon & GPIO45 SDIO reset supervisor | [sample_network.c](canmv_k230/src/rtsmart/mpp/userapps/sample/sample_network/sample_network.c) |
#### **k230**

## App Compilation

change to current dir canmv_k230


      cd canmv_k230/src/rtsmart/mpp
      source build_env.sh
      cd userapps/sample/sample_display
      make

in the directory sample/elf     generate sample_display.elf

default app: sample_display


rename sample_display.elf to app.elf  copy to sdcard, Power on again and start running by default.

## Advanced - Custom Firmware

### 1. Overview

Note

This chapter introduces how to develop on the K230 CanMV. If you have no custom requirements, you can skip this chapter.

The K230 CanMV is developed based on the K230 SDK 

### 2. Setting Up the Development Environment

| Host Environment            | Description                                                  |
| --------------------------- | ------------------------------------------------------------ |
| Ubuntu 20.04.4 LTS (x86_64) | The K230 CanMV compilation environment is suitable for Ubuntu 20.04 and above. |

Currently, K230 CanMV has only been verified to compile in a Linux environment. Other Linux versions have not been tested, so compatibility with other systems cannot be guaranteed.

#### 2.1 Local Build Environment

- Update APT sources (optional)

```sh
sudo bash -c 'cp /etc/apt/sources.list /etc/apt/sources_bak.list && \
  sed -i "s/archive.ubuntu.com/mirrors.tuna.tsinghua.edu.cn/g" /etc/apt/sources.list && \
  sed -i "s/security.ubuntu.com/mirrors.tuna.tsinghua.edu.cn/g" /etc/apt/sources.list'
```

- Install necessary dependencies

```sh
# Add support for i386 architecture
sudo bash -c 'dpkg --add-architecture i386 && \
  apt-get clean all && \
  apt-get update && \
  apt-get install -y --fix-broken --fix-missing --no-install-recommends \
    sudo vim wget curl git git-lfs openssh-client net-tools sed tzdata expect mtd-utils inetutils-ping locales \
    sed make cmake binutils build-essential diffutils gcc g++ bash patch gzip bzip2 perl tar cpio unzip rsync \
    file bc findutils dosfstools mtools bison flex autoconf automake python3 python3-pip python3-dev python-is-python3 \
    lib32z1 scons libncurses5-dev kmod fakeroot pigz tree doxygen gawk pkg-config libyaml-dev libconfuse2 libconfuse-dev \
    libssl-dev libc6-dev-i386 libncurses5:i386'
```



- Update PIP sources (optional)

```sh
pip3 config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple && \
pip3 config set global.extra-index-url "https://mirrors.aliyun.com/pypi/simple https://mirrors.cloud.tencent.com/pypi/simple"
```



- Install Python dependencies

```sh
pip3 install -U pyyaml pycryptodome gmssl jsonschema jinja2
```


- Install the repo tool

```sh
mkdir -p ~/.bin
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.bin/repo
chmod a+rx ~/.bin/repo
echo 'export PATH="${HOME}/.bin:${PATH}"' >> ~/.bashrc
source ~/.bashrc
```

### 3. Compilation Process

#### 3.1 Source Code Download

The source code of CanMV-K230 is hosted on Github. Users can download the source code using the repo tool.

```sh
# It's recommended to create a directory in the user's home directory before downloading the code
mkdir -p ~/canmv_k230_pro && cd ~/canmv_k230_pro

# Download the RT-Smart + CanMV project
git clone https://github.com/Xinyuan-LilyGO/T-Display-K230_canmv_rt.git
```


#### 3.2 Code Preparation

When compiling for the first time, you need to download the toolchain. The following command only needs to be executed once.

```sh
cd canmv_k230
# Download the toolchain when running for the first time
make dl_toolchain
```


#### 3.3 Compilation

Select the corresponding board configuration file according to actual needs, and then start compiling.

```sh
# List available configuration options
make list_def 
# Select the corresponding board configuration file
make k230_canmv_defconfig  # Replace with the appropriate defconfig for your board
# Start compilation
time make log
```


After compilation, the image files will be generated in the `canmv_k230_pro/canmv_k230/output/xxxx/xxx.img` directory.

## Firmware download

### 1. Flashing on Windows Platform

On a Windows system, you can use the Rufus tool to flash the firmware to a TF card. The download address for Rufus is: [Rufus Official Website](http://rufus.ie/downloads/).

1. Insert the TF card into your computer and start the Rufus tool. Click the “Select” button in the interface and choose the firmware file to be flashed. ![rufus-flash-from-file](https://www.kendryte.com/k230_canmv/en/main/_images/rufus_select.png)
2. Click the “Start” button, and Rufus will automatically proceed with the flashing. The progress bar will display the flashing progress, and the system will prompt “Ready” upon completion. ![rufus-flash](https://www.kendryte.com/k230_canmv/en/main/_images/rufus_start.png) ![rufus-sure](https://www.kendryte.com/k230_canmv/en/main/_images/rufus_sure.png) ![rufus-warning](https://www.kendryte.com/k230_canmv/en/main/_images/rufus_warning.png) ![rufus-finish](https://www.kendryte.com/k230_canmv/en/main/_images/rufus_finish.png)

### 2. Flashing on Linux Platform

Before inserting the TF card, first run the following command to check the current storage devices:

```sh
ls -l /dev/sd\*
```


Next, insert the TF card into the host machine and run the same command again to identify the newly added device node, which is the device node for the TF card.

Assuming the device node for the TF card is `/dev/sdc`, you can use the following command to flash the firmware to the TF card:

```sh
sudo dd if=sysimage-sdcard.img of=/dev/sdc bs=1M oflag=sync
```

## PinOverview

| AMOLED Screen Pin  | k230 Pin|
| :------------------: | :------------------:|
| LCD_RST   | GPIO22  |

| Touch Chip Pin  | k230 Pin |
| :------------------: | :------------------:|
| TP_RST   | GPIO24 |
| TP_SCL | GPIO36 |
| TP_SDA | GPIO37 |
| TP_INT | GPIO23 |

| HDMI Pin | k230 Pin |
| :------------------: | :------------------:|
| HDMI_RSTN | GPIO24  |
| HDMI_INT | GPIO22 |
| HDMI_CSCL | GPIO36 |
| HDMI_CSDA | GPIO37 |

| SD Card Pin  | k230 Pin |
| :------------------: | :------------------:|
| TFCARD_CMD  |  GPIO54  |
| TFCARD_CLK  |  GPIO55  |
|  TFCARD_D0  |  GPIO56  |
|  TFCARD_D1  |  GPIO57  |
|  TFCARD_D2  |  GPIO58  |
|  TFCARD_D3  |  GPIO59  |

## RelatedTests

<!--

### Power Dissipation

| Firmware | Program| Description | Picture |
| ------  | ------  | ------ | ------ |
| `[T-Display-k230-AMOLED-1.43_V1.0][Light_Sleep_Wake_Up]_firmware_V1.0.0.bin` | `Light Sleep Wake Up` | Power dissipation: 1282.8uA | <p align="center" width="10%"> <img src="image/13.jpg" alt="example" width="50%"> </p> |
| `[T-Display-k230-AMOLED-1.43_V1.0][Deep_Sleep_Wake_Up]_firmware_V1.0.0.bin` | `Deep Sleep Wake Up` | Power dissipation: 174.2uA |<p align="center" width="10%"> <img src="image/12.jpg" alt="example" width="50%"> </p> |

-->

## FAQ

* Q. After reading the above tutorials, I still don't know how to build a programming environment. What should I do?
* A. If you still don't understand how to build an environment after reading the above tutorials, you can refer to the [LilyGo-Document](https://github.com/Xinyuan-LilyGO/LilyGo-Document) document instructions to build it.

## Project
* [T-Display K230 V1.0.pdf](./schematic/T-Display K230 V1.0.pdf)

## Information
* [sx1262](./datasheet/HPDTEK_HPD16A_Series_规格书_V0.3.pdf)

  

## DependentLibraries
* [lvgl-8.4.1](https://lvgl.io)


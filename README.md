<p align="center">
  <img src="./assets/images/ethernet-tester.png" alt="Ethernet Tester icon" width="128" />
</p>

<div align="center">
    <h1>Ethernet Tester</h1>
</div>

<div align="center">
  <p>Read-only Ethernet diagnostics for CardputerZero, including gateway checks and on-demand local device discovery.</p>
</div>

<p align="center" style="margin: 20px 0; display: flex; justify-content: center; flex-wrap: wrap; gap: 12px;">
  <img src="https://img.shields.io/badge/SDL2-173B73?style=flat-square&logo=sdl&logoColor=white" alt="SDL2" />
  <img src="https://img.shields.io/badge/CMake-064F8C?style=flat-square&logo=cmake&logoColor=white" alt="CMake" />
  <img src="https://img.shields.io/badge/LVGL-2A9FD6?style=flat-square&logo=lvgl&logoColor=white" alt="LVGL" />
  <img src="https://img.shields.io/badge/Raspberry%20Pi-A22846?style=flat-square&logo=raspberrypi&logoColor=white" alt="Raspberry Pi" />
</p>

Desktop builds use an SDL hardware-accelerated compositor for UI preview, while device builds use the Linux/LVGL platform drivers. The tester inspects `eth0` and never configures it; ARP and mDNS discovery run only after an explicit user action.

## Project Overview

The application uses an MVVM structure around LVGL:

- **UI layer**: screens and widgets are implemented with LVGL objects. `BaseScreen` owns the page root, title bar, and page content. Widgets derive from `BaseWidgets` and expose a `build()` entry point.
- **Data model**: `EthernetModel` stores the selected diagnostic category and result; `BaseModel` also stores theme state.
- **View model**: `BaseViewModel` publishes Ethernet state as LVGL subjects and dispatches tests to the asynchronous platform service.
- **Reactive/data binding**: `src/reactive` wraps LVGL observer subjects and common bindings so labels, styles, flags, widget values, and events can be connected to application state.
- **Data flow**: user input triggers widget callbacks, callbacks update `BaseViewModel`, view model publishes subjects, and bound UI objects refresh automatically through LVGL observers.
- **Platform layer**: `EthernetService` reads Linux interface information and runs bounded diagnostics only after an explicit user action. Keyboard shortcuts navigate the tabs and operate the selected action.

Diagnostic categories:

- **Summary**: three field-health indicators (Link, IP/DNS, and Listen), showing `NOT TESTED` until there is a measurement and the age of the latest result. `Enter` starts a manual Auto Test over Link, Configuration, Connectivity, DNS, and Switch; it does not renew DHCP or change network settings.
- **Link**: a five-second sample of carrier changes, speed/duplex and available RX/TX error, drop and CRC counters.
- **Configuration**: IPv4 address, MAC address and default gateway (read-only).
- **Connectivity**: one ICMP echo to the default gateway routed through `eth0`.
- **Performance**: a read-only snapshot of cumulative interface byte, packet and error counters; it is not a throughput measurement.
- **Discovery**: one bounded ARP scan of the IPv4 subnet attached to `eth0`, showing all responding peers captured within the output limit.
- **mDNS**: a bounded DNS-SD browse of services announced on the local link, reporting instance, type, host, port, addresses and TXT data when available.
- **Advanced**: driver and link metadata only; cable diagnostics are unsupported.
- **Switch**: a five-second passive LLDP/CDP listen on `eth0`; only fields present in received announcements are shown.
- **DNS**: one PTR lookup for the configured IPv4 gateway, sent to the first IPv4 resolver in `/etc/resolv.conf` through `eth0`.
- **DHCP**: read the current `systemd-networkd` lease metadata when available; other network managers are reported as unsupported. This does not request, renew, or release a lease.
- **Listen**: bounded passive observation for ARP, DHCP and STP frames on `eth0`; no PCAP file is created, and no broadcast request is sent.

Tests that depend on unavailable interfaces, drivers or tools report `Unsupported`; no hardware success is simulated. The Auto Test runs only after an explicit `Enter` on Summary, is cancellable, and does not change network settings. The optional connectivity test sends one packet to the gateway already configured on `eth0`; Discovery sends ARP requests only when explicitly started. The Switch and Listen observers are passive and need `CAP_NET_RAW`: Debian installation assigns `cap_net_raw=ep` to the app executable via `postinst` (using the `libcap2-bin` dependency), so the app does not need to run as root. Manual installations can grant it with `sudo setcap cap_net_raw+ep /usr/bin/ethernet-tester`; this capability can be unavailable on filesystems that do not support file capabilities. No `CAP_NET_ADMIN` is requested. DNS sends one PTR query to the configured resolver for the `eth0` gateway only; it does not probe public or arbitrary domains, and a successful response without a PTR record is not a DNS failure. A loopback-only DNS stub cannot be queried through `eth0` and is reported as unsupported. The DHCP view is read-only and currently recognizes `systemd-networkd` lease files; it does not perform an active lease exchange or identify rogue servers. The Listen view does not capture packets to disk and may require privileges; no privilege is elevated automatically. Discovery requires `arp-scan` and root or `CAP_NET_RAW` privileges on many systems. A timeout or oversized result is reported as incomplete, never as a complete list. Only responding IPv4 devices on the local broadcast subnet can be found; this is not an inventory of other VLANs or non-responding hosts. The five-second link sample is an indicator only, not cable certification; brief flaps can be missed and driver counters may be unavailable or reset.
The mDNS browse runs for up to 10 seconds after `Enter`, uses IPv4 multicast on `eth0`, and sends DNS-SD queries only; it never contacts discovered services. It is implemented in the app without Avahi or another daemon. IPv6 addresses are displayed when AAAA records are received, but IPv6-only multicast discovery is not supported. An empty result does not mean the network is down: mDNS reveals only services that announce on the local link, not silent devices, other VLANs, or every service present. Resource limits and incomplete results are reported explicitly.

Use the left/right arrows (or `Z`/`C`) to change tabs and `Enter` (or keypad `8`) to run or cancel a test; on Summary this starts the cancellable Auto Test sequence. In the mDNS tab, up/down (or `F`/`X`) selects a service; press `Tab` to open its details and `Esc` or Backspace to return to the list. Up/down scrolls details in that view and results on other tabs. The interface always uses dark mode; tabs and the selected action are clickable.

## Repository Layout

```text
.
├── assets/                 # Runtime assets used by the app
│   ├── applications/       # Desktop/app launcher metadata
│   ├── audio/              # Audio resources
│   ├── fonts/              # TTF fonts loaded through FreeType
│   └── images/             # Image resources
├── screenshot/             # Original simulator screenshots
├── screenshots/            # Store-ready 320×170 application screenshots
├── src/
│   ├── app/                # Application lifecycle, asset loading, screen management
│   ├── config/             # LVGL config headers for desktop and device builds
│   ├── logger/             # Project logging wrapper
│   ├── model/              # Base application data model
│   ├── platform/           # Platform input/hardware integration
│   ├── reactive/           # LVGL observer subjects and binding helpers
│   ├── view/               # Theme, UI constants, screens, and widgets
│   │   ├── screens/        # Page-level LVGL screens
│   │   └── widgets/        # Reusable LVGL widgets such as nav bar and icon button
│   ├── viewmodel/          # BaseViewModel and UI-facing actions/state subjects
│   └── main.cpp            # Program entry point
├── CMakeLists.txt          # Build graph, dependencies, options, targets
├── CMakePresets.json       # Desktop/device configure and build presets
└── README.md
```

### Modules

- `src/app/asset_manager.*`: resolves runtime assets from the optional CMake asset root, source tree `assets/`, and installed `/usr/share/<APP_NAME>/` path.
- `src/app/screen_manager.cpp`: switches LVGL screens when the current page subject changes.
- `src/app/desktop_simulator_frame.*`: owns the desktop SDL window, renderer, high-resolution shell textures, input, and LVGL texture composition.
- `src/config/lv_conf_desktop.h`: desktop LVGL rendering settings.
- `src/config/lv_conf_cm0.h`: embedded Linux/CardputerZero LVGL settings.
- `src/platform/linux_input.*`: Linux evdev keypad support and desktop SDL keyboard routing for tabs and actions.
- `src/reactive/subjects.*`: typed wrappers around LVGL subjects.
- `src/reactive/bindings.*`: helper bindings for labels, theme, widget values, flags, states, and events.
- `src/view/widgets/icon_button.*`: transparent icon button with font/color/theme support.

## Build Options

Common CMake cache options:

| Option | Default | Description |
| --- | --- | --- |
| `USE_DESKTOP` | `ON` | Build SDL desktop simulator when `ON`; build embedded Linux target when `OFF`. |
| `APP_NAME` | `ethernet-tester` | Application name used by installed asset lookup. |
| `APP_ASSETS_ROOT` | empty | Optional runtime asset root. Expected layout includes `fonts/`, `images/`, etc. |
| `APP_CONFIG_FILE` | platform default | Optional legacy config path. Defaults to `config/ethernet-tester.conf` for desktop builds and `/etc/ethernet-tester.conf` for device builds; existing `dark_mode` values are ignored. |
| `APP_KEY_INPUT_DEVICE` | empty | Optional Linux evdev device path, e.g. `/dev/input/event0`. Empty means auto-scan `/dev/input/event*`. |
| `APP_FRAMEBUFFER_DEVICE` | `/dev/fb0` | Linux framebuffer device used by embedded builds when `APP_USE_DRM=OFF`. |
| `APP_USE_DRM` | `OFF` | Use LVGL's Linux DRM/KMS backend instead of fbdev for embedded builds. |
| `APP_DRM_DEVICE` | `/dev/dri/card0` | DRM device path used when `APP_USE_DRM=ON`. |
| `APP_DRM_CONNECTOR_ID` | `-1` | DRM connector id used when `APP_USE_DRM=ON`; `-1` auto-selects. |

Asset lookup order:

1. `APP_ASSETS_ROOT` when provided by CMake
2. source-tree `assets/` for development
3. `/usr/share/<APP_NAME>/` for installed deployments

The application always starts in dark mode. Config files are still read for compatibility, but
their legacy `dark_mode` value is ignored and no theme preference is saved. Previously saved
per-user theme overrides are no longer used.

Installed device builds use `/etc/ethernet-tester.conf` as the legacy config file.

## Desktop Builds

Desktop builds are intended for fast UI development. LVGL renders at the device-native `320x170` resolution, while SDL composites that texture into a hardware-accelerated, high-DPI simulator window with the full-resolution device shell.

Current dependencies info:

| Dependency | Version | Source | Notes |
| --- | --- | --- | --- |
| LVGL | `v9.5.0` | `CMakeLists.txt` FetchContent | Main GUI framework. |
| fmt | `12.1.0` | System package manager or `vcpkg.json` override | Logging and formatted strings. |
| libpng | `1.6.48` | System package manager or `vcpkg.json` override | PNG image decoding support for LVGL. |
| libjpeg-turbo | `3.1.3` | System package manager or `vcpkg.json` override | JPEG image decoding support for LVGL. |
| zlib | `1.3.1` | System package manager or `vcpkg.json` override | Compression dependency used by image libraries. |
| SDL2 | `2.32.54` | System package manager or `vcpkg.json` override | Desktop simulator display and input backend. |
| FreeType | `2.13.3` | System package manager or `vcpkg.json` override | Runtime TTF font rendering. |

> Windows vcpkg installs use the versions above through manifest mode, with the registry baseline pinned in `vcpkg-configuration.json`.

### Linux Desktop

Debian/Ubuntu dependencies:

```shell
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  libpng-dev \
  libjpeg-dev \
  libfmt-dev \
  libsdl2-dev \
  libfreetype-dev \
  iputils-ping \
  arp-scan \
  zlib1g-dev
```

> [!NOTE]
> Minimal CMake version is 3.31.0 (in coordinate with Debian 13 trixie), for Ubuntu user you can install latest cmake via `snap`
> and run following cmake command with `/snap/bin/cmake`.


Configure:

```shell
cmake --preset linux-x86-64
```

Build:

```shell
cmake --build --preset linux-x86-64-dbg
# alternatively, you can run release build
# cmake --build --preset linux-x86-64-rel
```

Run:

```shell
./build/linux-x86-64/Debug/ethernet-tester
# or launch release build
# ./build/linux-x86-64/Release/ethernet-tester
```

### macOS Desktop

Install the Apple command-line tools:

```shell
xcode-select --install
```

Install dependencies with Homebrew:

```shell
brew install \
  cmake \
  ninja \
  libpng \
  jpeg \
  fmt \
  sdl2 \
  freetype \
  dpkg \
  zlib
```

Configure for Apple Silicon:

```shell
cmake --preset darwin-arm64
```

Build:

```shell
cmake --build --preset darwin-arm64-dbg
# alternatively, you can run release build
# cmake --build --preset darwin-arm64-rel
```

Run:

```shell
./build/darwin-arm64/Debug/ethernet-tester
# or launch release build
# ./build/darwin-arm64/Release/ethernet-tester
```

For Intel macOS, use the `darwin-x86-64` configure preset and matching build preset:

```shell
cmake --preset darwin-x86-64
cmake --build --preset darwin-x86-64-dbg
# alternatively, you can run release build
# cmake --build --preset darwin-x86-64-rel
./build/darwin-x86-64/Debug/ethernet-tester
# or launch release build
# ./build/darwin-x86-64/Release/ethernet-tester
```

### Windows Desktop

Install CMake and Ninja:

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

Install a C++ toolchain. Choose one of the following.

MSVC Build Tools:

```powershell
winget install Microsoft.VisualStudio.BuildTools
```

In the Visual Studio installer, enable **Desktop development with C++** and make sure MSVC and a Windows SDK are selected.

MinGW-w64:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```
Alternatively, you can download from [`winlibs`](https://winlibs.com/). 

Install vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Configure environmental variables for current terminal:

```
$env:VCPKG_ROOT="C:\vcpkg"
$env:PATH="$env:VCPKG_ROOT;$env:PATH"
```

> [!TIP]
> Change `VCPKG_ROOT` to your vcpkg installation directory if it is located elsewhere.
>
> These environment variables are **session-scoped**, meaning they only apply to the current terminal session.
> Once the terminal is closed, the variables will be lost and need to be set again in a new session.
> If you want to a persist environment variable
> ```powershell
> setx VCPKG_ROOT "C:\vcpkg"
> ```


Configure and build with MSVC:

> [!IMPORTANT]
> These steps require launching CMake from a **Developer Command Prompt** or **Developer PowerShell** provided by Visual Studio.
>
> This ensures that the MSVC environment (compiler, linker, and Windows SDK paths) is properly initialized.
>
> Without this environment setup, CMake may fail to detect or correctly configure the MSVC toolchain.

```powershell
cmake --preset win32-msvc
cmake --build --preset win32-msvc-dbg
.\build\msvc\Debug\ethernet-tester.exe

# alternatively for release build
# cmake --build --preset win32-msvc-rel
# .\build\msvc\Release\ethernet-tester.exe
```

Configure and build with MinGW-w64:

> [!IMPORTANT]
> These steps require a properly configured MinGW-w64 toolchain in your system PATH.
>
> Please ensure the following compilers are available in the current terminal session:
> - gcc
> - g++
> - ld
> - ar
>
> You can verify the setup by running:
> ```powershell
> gcc --version
> g++ --version
> ```
>
> If these commands are not recognized, you must add MinGW-w64 `bin` directory to your PATH before configuring CMake.

```powershell
cmake --preset win32-mingw64
cmake --build --preset win32-mingw64-dbg
.\build\mingw64\Debug\ethernet-tester.exe

# alternatively for release build
# cmake --build --preset win32-mingw64-rel
#.\build\mingw64\Release\ethernet-tester.exe
```
> [!NOTE]
> `VCPKG` will handle the dependencies during CMake configuration process automatically, 
> this may take several minutes depend on you network connection.

## CardputerZero Cross Build

This preset builds an aarch64 Linux target from a host machine with an `aarch64-linux-gnu-gcc/g++` toolchain.
The BSP at `.cache/sdk_bsp-src` is treated as a sysroot: headers, libraries, startup objects, and pkg-config metadata are resolved from that directory first.
If that directory does not exist on the first configure, the toolchain downloads and extracts `sdk_bsp.tar.gz` automatically before CMake runs compiler checks.

Install a cross compile toolchain:

+ Debian

```shell
sudo apt install crossbuild-essential-arm64
```
+ MacOS

```shell
brew install aarch64-unknown-linux-gnu
```

> [!NOTE]
> On Windows, it's recommended to use WSL for cross build
> or you can download [cross compile tools](https://sysprogs.com/getfile/2542/raspberry64-gcc14.2.0.exe) and configure the `PATH` yourself

Configure:

```shell
cmake --preset cp0-cross
```

Build:

```shell
cmake --build --preset cp0-cross-dbg
# or release build
# cmake --build --preset cp0-cross-rel
```

Deploy the release package to your device after filling in your device user and IP:

```shell
REMOTE_USER=<user> REMOTE_HOST=<device-ip> ./deploy.sh
```

By default, the debian package is copied to '$HOME' folder, normally it's under '/home/\<user\>'.

On your device, install the copied package with `apt` and replace the package file name with the one you copied:

```shell
sudo apt install --no-install-recommends ./ethernet-tester_0.2.1_rev1_arm64.deb
```


## Development Guide

### Navigation Keys

Ethernet Tester hardware keys and keyboard shortcuts:

| Key | Action |
| --- | --- |
| `4` / hold `ESC` | Exit |
| `5` / `Z` / `Left` | Previous tab |
| `Up` / `Down` / `F` / `X` | Select mDNS service; scroll details/results |
| `Tab` | Open selected mDNS service details |
| `Esc` / `Backspace` | Return from mDNS details to the service list |
| `7` / `C` / `Right` | Next tab |
| `Enter` / `8` | Run or cancel the selected test |

Summary's Auto Test runs Link, Configuration, Connectivity, DNS, and Switch in that order, only after an explicit `Enter`; it is cancellable and does not renew DHCP or modify interface settings. The optional connectivity test sends one ICMP echo to the default IPv4 gateway on `eth0`. Discovery sends a single bounded ARP scan of `eth0`'s local subnet only after `Enter`; it may require root or `CAP_NET_RAW`. The mDNS tab performs a bounded 10-second DNS-SD browse only after `Enter`. The DHCP tab reads an existing `systemd-networkd` lease when present, and Listen passively observes selected local frames; neither runs automatically or changes network state.

### Adding a Category

1. Add a category and result behavior in `src/model/ethernet_model.h` and `src/platform/ethernet_service.*`.
2. Keep blocking operations off the LVGL event path and provide cancellation for long-running work.
3. Add the tab label and keyboard-visible description to the Ethernet dashboard.
4. Report unavailable hardware or tools as unsupported rather than simulating success.

### Adding a Widget

1. Derive from `BaseWidgets`.
2. Implement `build()` and create LVGL objects under `parent_`.
3. Use `reactive::bind_*` helpers for text, style, state, or event bindings.
4. Keep LVGL observer lifetimes object-bound whenever possible with `lv_subject_add_observer_obj` or `reactive::observe_obj`.

### Fonts and Assets

Device builds use Noto Sans CJK Medium for the default bold text and Noto Sans CJK Regular for regular-weight text. They are loaded through FreeType from:

```text
/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc
/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
```

When either system font is unavailable, the UI falls back to LVGL's built-in Montserrat fonts. Desktop builds always use the fallback fonts. Runtime icon and other asset fonts remain under:

```text
assets/fonts/
/usr/share/<APP_NAME>/fonts/
```

You can also provide an asset root at configure time:

```shell
cmake --preset darwin-arm64 -DAPP_ASSETS_ROOT=/path/to/assets
```


## Debian Package

Debian packages are produced with CPack and written to `dist/`. The package file name follows:

```text
<package-name>_<Version>_<Revision>_arm64.deb
```

Default example:

```text
dist/ethernet-tester_0.2.1_rev1_arm64.deb
```

Package layout:

| Path | Content |
| --- | --- |
| `/usr/bin/ethernet-tester` | Application executable. |
| `/etc/ethernet-tester.conf` | Legacy application config; any `dark_mode` value is ignored. |
| `/usr/share/ethernet-tester/` | Runtime assets: fonts, images, audio. |
| `/usr/share/APPLaunch/applications/ethernet-tester.desktop` | APPLaunch launcher entry. |
| `/usr/share/APPLaunch/share/images/ethernet-tester*.png` | APPLaunch launcher icons/fallbacks. |
| `/usr/lib/systemd/system/ethernet-tester.service` | Optional systemd service for embedded autostart. |
| `/usr/share/doc/ethernet-tester/` | README and third-party asset license notes. |

The Debian package depends on `libcap2-bin` and its `postinst` grants `cap_net_raw=ep` to `/usr/bin/ethernet-tester` for passive Switch announcements. If `setcap` fails (for example, because the target filesystem does not support file capabilities), package configuration fails rather than silently leaving Switch unavailable. This does not grant `CAP_NET_ADMIN` or make the application run as root.

Build and package:

```shell
cmake --preset cp0-cross
cmake --build --preset cp0-cross-rel
cpack --preset cp0-cross-deb
```

Or run the full configure, release build, and package flow with one workflow preset:

```shell
cmake --workflow --preset cp0-cross-package
```

> [!NOTE]
> Use Debian based Linux host with CPack generator 
> as other OS may not resolve the dependencies correctly
> no full dpkg, dpkg-shlibdeps, objdump and readelf support

# Contributing

If you have better framework designs on Embedded Linux or build system practice, feel free to let us know.

# License

Project released under MIT license. More license information can be found in assets folder. 

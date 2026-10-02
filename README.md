# M5Stack Core2 – OPC UA Server

An OPC UA server running on the M5Stack Core2. Every built-in sensor is in the
address space, and the screen shows the battery plus a live dashboard. A client
such as UaExpert can write a value that the Core2 then shows on its screen.

| | |
|---|---|
| Framework | ESP-IDF **v5.5** (tested with v5.5.5; the VS Code ESP-IDF extension installs it) |
| OPC UA stack | open62541 v1.5.8, already included (nothing to download) |
| Hardware library | M5Unified (downloaded automatically on the first build) |
| Endpoint | `opc.tcp://<ip-shown-on-screen>:4840`, Security **None** (for testing) |

---

## You only need to do 4 things

### 1. Install the tools (one time, about 20 minutes)

1. Install **Visual Studio Code**: <https://code.visualstudio.com>
2. In VS Code press `Ctrl+Shift+X`, search **ESP-IDF**, and install the extension from **Espressif Systems**.
3. Press `Ctrl+Shift+P`, run **ESP-IDF: Open ESP-IDF Installation Manager**, and install **ESP-IDF v5.5.x** (the newest 5.5 release).
   *v6.0 will probably work, but v5.5 is what this project was checked against.*
   Then run **ESP-IDF: Select Current ESP-IDF Version** and pick it. Until you do, the
   extension can't find its tools (see *esptool.py* under "If something goes wrong").
4. Plug the Core2 into the PC with a USB-C **data** cable. It should show up as a COM port in Device Manager.
   If it doesn't, install the USB driver from M5Stack's site (newer Core2 units use the **CH9102** chip, older ones use **CP2104**).

### 2. Put in your Wi-Fi details

Copy **`main/wifi_config.example.h`** to **`main/wifi_config.h`**, then change these two lines in the copy.
(If you skip the copy, the first build makes it for you and asks you to fill it in.)

```c
#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
```

The Core2 only works with **2.4 GHz** Wi-Fi. The time zone is set to US Eastern; other zones are listed in the file.
`main/wifi_config.h` is in `.gitignore`, so your password stays out of git. Don't put it in the example file.

### 3. Build and flash

1. In VS Code: **File → Open Folder…** and choose this `m5core2-opcua` folder.
2. `Ctrl+Shift+P` → **ESP-IDF: Set Espressif Device Target** → **esp32** (if it asks, pick *ESP32 chip (via ESP USB Bridge)* or *Custom board*. Any ESP32 option is fine).
3. Click the **COM port** item in the bottom status bar and choose your Core2's port.
4. Click the **flame icon** (🔥 *Build, Flash and Monitor*) in the bottom bar. If it asks for a flash method, choose **UART**.

The first build takes a few minutes. It downloads M5Unified and compiles the
large OPC UA library once. Later builds are much faster. When it finishes, the
monitor window shows the log and the Core2 restarts.

### 4. Connect a client

The Core2's IP address appears at the top-left of the screen once Wi-Fi is up.

In **UaExpert** (free from Unified Automation):
1. **Server → Add…** → double-click *< Double click to Add Server… >* under **Custom Discovery**.
2. Enter `opc.tcp://<IP-from-screen>:4840` and expand the entry that appears.
3. Choose **None - None**, then **OK** and **Connect**. Log in as **Anonymous**.
4. Browse **Objects → M5Core2**. Drag variables into the Data Access View to watch them live.

To change the big number on the screen, write to **M5Core2 → Display → Value**.
You can also change **Label**, **Unit** and **Brightness**, or right-click
**Outputs → Vibrate** → *Call…* to make the Core2 buzz.
The side lights (M5GO Bottom2 base) are under **Lights**: set **On** to true, and pick
the colour with **Red**, **Green** and **Blue** (0–255 each).

---

## What's on the screen

- **Idle sleep:** after 60 s with no activity, the screen and side lights turn off; the server keeps running.
  Touching the screen or a button wakes it. That first touch only wakes it, so it won't change page or press a button.
  A client that *changes* something (Display, Lights, Vibrate) also wakes it; just reading values doesn't.
  Change the time with **Display → IdleTimeout** (seconds, 0 = never).
- **Battery:** measured with the side lights and microphone off: about 98 mA with the screen on and 59 mA asleep.
  On the Core2's 390 mAh battery that's roughly 6 hours mostly idle. To save power, Wi-Fi
  checks for traffic only every ~0.5 s, so replies take up to about 0.5 s. The 5 V output to the
  base is switched off while the side lights are dark. For always-on use, keep it on USB power.
- **Top bar (all pages):** IP address, number of connected OPC UA clients, battery % with a colour bar (`+` means charging)
- **Touch buttons below the screen** pick the page:
  - **A – Main:** the big *Display Value* that clients write, with its label and unit.
    Below it are on-screen buttons: **Light** (on/off), **Color** (step through colours) and **Buzz** (short vibration)
  - **B – Motion:** accelerometer and gyroscope with live bars, plus tilt (pitch / roll).
    The accelerometer always includes gravity, so a tilted Core2 shows a steady reading.
    The gyroscope only reads while it is turning.
  - **C – System:** battery voltage/current, USB voltage, local time, uptime, Wi-Fi signal, free memory, endpoint URL

## Address space

Namespace URI `urn:m5stack:core2:opcua` (namespace index 2). All NodeIds are strings, e.g. `ns=2;s=Battery.Level`.

| Folder | Variables | Notes |
|---|---|---|
| Battery | Level (%), Voltage (V), Current (mA), IsCharging, UsbVoltage (V) | from the power chip (AXP192 or AXP2101, detected automatically) |
| IMU | AccelX/Y/Z (g), GyroX/Y/Z (deg/s), Temperature (°C), Pitch, Roll (deg) | acceleration includes gravity; pitch/roll are tilt worked out from it; chip temperature runs warmer than the room |
| Microphone | SoundLevel (dBFS), **Enabled** | 0 = loudest, about −90 = silence. Off by default to save battery (reads −96) |
| Touch | ButtonA/B/C, ButtonA/B/CCount, IsTouched, X, Y | |
| Clock | RtcTime (DateTime, UTC) | set from the internet (NTP) at boot, kept by the RTC chip |
| System | Uptime, FreeHeap, WifiRssi, IpAddress, ClientCount, FirmwareVersion | |
| Display | **Value**, **Label**, **Unit**, **Brightness**, Page, **IdleTimeout** (s), Asleep | **bold = writable**. IdleTimeout 0 = never sleep |
| Lights | **On**, **Red**, **Green**, **Blue**, **Brightness** | the 10 side LEDs on the M5GO Bottom2 base; the screen buttons change these too |
| Outputs | **Vibrate(DurationMs)** method | max 5000 ms |

Values update every 250 ms.

> The Core2's speaker is switched off. It shares a bus with the microphone, and the microphone is the sensor.

---

## Project layout

```
main/
  wifi_config.h     <- the only file you need to edit (your copy, not in git)
  wifi_config.example.h  template for it
  main.cpp          sensors, touch buttons, screen drawing
  opcua_server.c    OPC UA address space and server
  wifi_net.c        Wi-Fi connection and clock sync
  device_state.h    data shared between the screen and the server
components/open62541/   OPC UA stack (single-file build, see its README)
sdkconfig.defaults  Core2 settings: 16 MB flash, 8 MB PSRAM, sockets, 802.11r
partitions.csv      6 MB app partition
.vscode/            ESP-IDF extension, IntelliSense and serial port settings
```

> Keep the project out of OneDrive/Dropbox folders (e.g. `C:\dev\m5core2-opcua`).
> Syncing the `build/` folder causes locked-file errors and slow builds.
>
> If you change `sdkconfig.defaults`, delete `sdkconfig` before the next build.
> Otherwise the existing `sdkconfig` keeps the old values.

## If something goes wrong

| Symptom | Fix |
|---|---|
| Screen says *Wi-Fi connecting…* forever | Check SSID/password spelling in `wifi_config.h`, and make sure the network is 2.4 GHz |
| No COM port | Try another USB-C cable (some only charge), or install the CH9102/CP2104 driver |
| Build error mentioning `m5unified` | The first build needs internet to download it. Run the build again. |
| UaExpert can't connect | PC and Core2 must be on the same network. Some routers block ping, so test the port instead: `Test-NetConnection <ip> -Port 4840` in PowerShell. |
| *"Make sure you have the esptool.py installed and set in $PATH"* when choosing a port | The extension has no ESP-IDF selected. Run **ESP-IDF: Select Current ESP-IDF Version**, pick v5.5.x, then reload the window. |
| Flashing says *Could not open COM port, the port is busy* | The serial monitor is still open. Close it (`Ctrl+]` in its terminal) and flash again. |
| Matrikon OPC UA Explorer times out on *Find Servers* | Fixed in `opcua_server.c` (`verifyRequestTimestamp = UA_RULEHANDLING_ACCEPT`). Matrikon sends requests without a timestamp, and open62541 rejects those by default. |
| Wi-Fi keeps dropping (IP disappears from the screen) | The serial monitor shows `disconnected (reason N, rssi …)`. Keep the Core2 away from the laptop and USB 3 ports, which interfere with 2.4 GHz. On mesh routers, use a separate 2.4 GHz IoT network, or turn off client steering / 802.11r. |
| Clock shows *not set* | The Core2 needs internet access for NTP on the first boot. After that the RTC keeps time. |

## License

The project's own code is under the [MIT License](LICENSE).
The bundled OPC UA stack in `components/open62541/` is [open62541](https://open62541.org)
v1.5.8 under the Mozilla Public License 2.0 ([its license](components/open62541/LICENSE)),
with one small local change (see [its README](components/open62541/README.md)).

## Next step: certificates

Security is **None** for now, so anyone on your network can read and write.
The next stage is adding the Basic256Sha256 *Sign & Encrypt* endpoint with a
device certificate. That means regenerating the open62541 library with
encryption (mbedTLS) turned on and adding certificate storage.

# Flight Tracker Tablet

Visual flight tracker for the Waveshare ESP32-S3 7 inch LCD Type B
(`ESP32-S3-LCD-7B` / `ESP32-S3-Touch-LCD-7B`, 1024x600).

The firmware draws a radar-style map centered on your coordinates and refreshes
nearby ADS-B aircraft from `api.adsb.lol` every 15 seconds.

## Configure

The preferred setup path is on-device configuration:

1. Flash the firmware.
2. Join the tablet's setup Wi-Fi: `FlightTrackerTablet`
3. Use password: `flighttablet`
4. Open `http://192.168.4.1`
5. Save Wi-Fi, latitude, longitude, and radius.

The tablet stores those settings in ESP32 NVS and reboots into the tracker.

You can also pre-fill defaults at build time by copying
`include/secrets.example.h` to `include/secrets.h`, then setting:

```cpp
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
#define HOME_LAT 41.8781
#define HOME_LON -87.6298
#define TRACK_RADIUS_NM 15
```

Use latitude/longitude rather than storing your street address in the project.
`include/secrets.h` is ignored by git. Runtime setup values take precedence
after they are saved from the tablet setup page.

## Build

```powershell
pio run
```

## Upload

Connect the board over USB-C. If needed, hold `BOOT`, tap `RESET`, then release
`BOOT` before uploading.

```powershell
pio run --target upload
pio device monitor
```

## Web Flasher

The `webflash/` folder contains an ESP Web Tools installer page and manifest.
Serve it from GitHub Pages or any static web server, then open it in Chrome or
Edge:

```powershell
python -m http.server 8000 --directory webflash
```

Then visit `http://localhost:8000`.

## Notes

- The project uses Waveshare's RGB LCD pin/timing configuration for the 1024x600
  Type B board.
- The PlatformIO board settings, LCD power/reset sequence, one-framebuffer RGB
  driver mode, and PSRAM-backed drawing buffer mirror the working
  `RepublicCommand/display-tablet` firmware.
- If Wi-Fi or coordinates are missing, the first screen starts setup AP mode and
  shows the setup network details.
- ADS-B data can be incomplete or delayed. Treat this as a fun local display,
  not a navigation or safety device.

# ESP32-C6-LCD-1.47 Firmware

Firmware for the ESP32-C6 microcontroller with 1.47" LCD display (172x320 ST7789) using ESP-IDF framework.

## Features

- **WiFi Connectivity**: Auto-connect with saved credentials or configuration portal
- **1.47" LCD Display**: ST7789 driver with 172x320 resolution via esp_lcd
- **SD Card Support**: Read/write files, store configuration
- **Web Interface**: Remote control and configuration via browser
- **REST API**: Programmatic control of display and system

## Hardware

- **MCU**: ESP32-C6
- **Display**: 1.47" LCD (ST7789, 172x320)
- **Storage**: SD card slot (SPI)

### Default Pin Configuration

| Function | GPIO |
|----------|------|
| LCD MOSI | 6 |
| LCD SCLK | 7 |
| LCD CS | 14 |
| LCD DC | 15 |
| LCD RST | 21 |
| LCD BL | 22 |
| SD CS | 5 |
| SD MISO | 2 |

Adjust pins in `include/config.h` to match your specific board.

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- USB cable for flashing

### Build & Upload

```bash
cd esp32-lcd-board

# Build
pio run

# Upload
pio run -t upload

# Monitor serial output
pio device monitor
```

### First Boot

1. On first boot, the device creates a WiFi access point:
   - **SSID**: `ESP32-LCD-Setup`
   - **Password**: `changeme123`

2. Connect to this network and open `192.168.4.1` in your browser

3. Enter your WiFi network credentials and save

4. Device will restart and connect to your network

5. Access the web interface at the device's new IP address

## Web Interface

Once connected, access the device at its IP address:

- **Dashboard** (`/`): System status, WiFi config, and display controls

## REST API

### GET /api/status
Returns system status including WiFi, SD card, and heap info.

```json
{
  "board": "ESP32-C6-LCD-1.47",
  "version": "1.0.0",
  "uptime": 3600,
  "wifi_connected": true,
  "ip": "192.168.1.100",
  "ssid": "MyNetwork",
  "rssi": -45,
  "free_heap": 200000,
  "sd_mounted": true
}
```

### POST /api/display
Control the display.

```json
{"command": "hello_world"}
```

Available commands:
- `hello_world` - Show hello world screen
- `clear` - Clear the display
- `brightness` - Set brightness: `{"command": "brightness", "level": 80}`
- `text` - Display text: `{"command": "text", "text": "Hi", "x": 10, "y": 50, "size": 2}`
- `splash` - Show splash screen

### POST /api/system
System commands.

```json
{"command": "restart"}
```

Available commands:
- `restart` - Restart the device
- `reset_wifi` - Clear WiFi credentials and restart
- `sleep` - Turn off backlight
- `wake` - Turn on backlight

### POST /api/wifi
Save WiFi credentials.

```json
{
  "ssid": "MyNetwork",
  "password": "mypassword"
}
```

## Project Structure

```
esp32-lcd-board/
├── include/
│   ├── config.h         # Pin definitions & constants
│   ├── lcd_driver.h     # LCD display interface
│   ├── sd_card.h        # SD card interface
│   ├── web_server.h     # Web server interface
│   └── wifi_manager.h   # WiFi management
├── src/
│   ├── CMakeLists.txt   # ESP-IDF component config
│   ├── main.c           # Application entry point
│   ├── lcd_driver.c     # ST7789 LCD driver
│   ├── sd_card.c        # SD card operations
│   ├── web_server.c     # HTTP server & API
│   └── wifi_manager.c   # WiFi connection handling
├── CMakeLists.txt       # Top-level CMake config
├── partitions.csv       # Flash partition table
├── sdkconfig.defaults   # ESP-IDF defaults
├── platformio.ini       # PlatformIO configuration
└── README.md
```

## Customization

### Display Pins

Edit `include/config.h`:

```c
#define LCD_PIN_MOSI  6
#define LCD_PIN_SCLK  7
#define LCD_PIN_CS    14
#define LCD_PIN_DC    15
#define LCD_PIN_RST   21
#define LCD_PIN_BL    22
```

### WiFi Configuration

Edit `include/config.h`:

```c
#define WIFI_AP_SSID  "ESP32-LCD-Setup"
#define WIFI_AP_PASS  "changeme123"
```

## Troubleshooting

**Build error "no such file cJSON.h"**: The cJSON library is included with ESP-IDF. Ensure you have a recent ESP-IDF version.

**Display not working**: Check pin connections and verify pin definitions in `config.h` match your board.

**SD card not mounting**: SD card is optional. The firmware will continue without it. If using SD, ensure pins don't conflict with display.

## License

MIT License

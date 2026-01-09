# Departure Board

Real-time public transit departure display for ESP32-C6 with a 1.47" LCD screen. Shows live Sydney Metro and Train departure information using the Transport for NSW API.

![ESP32-C6 LCD Board](https://img.shields.io/badge/Platform-ESP32--C6-blue) ![PlatformIO](https://img.shields.io/badge/Build-PlatformIO-orange) ![License](https://img.shields.io/badge/License-MIT-green)

## Features

- **Real-time departures** - Live Sydney Metro and Train data from TfNSW API
- **Multiple views** - Victoria Cross, Crows Nest, Artarmon stations
- **Web configuration** - WiFi setup and API key entry via browser
- **REST API** - Remote control and status monitoring
- **Auto brightness** - Day/night brightness adjustment
- **Status indicators** - RGB LED and on-screen delay markers

## Hardware

- **MCU**: ESP32-C6 (RISC-V dual-core)
- **Display**: ST7789 1.47" LCD (172x320)
- **Optional**: SD card, RGB LED

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- [TfNSW API key](https://opendata.transport.nsw.gov.au/) (free)

### Build & Flash

```bash
# Build
pio run

# Upload to device
pio run -t upload

# Monitor serial output
pio device monitor
```

### First Boot

1. Connect to WiFi network `ESP32-LCD-Setup` (password: `changeme123`)
2. Open `192.168.4.1` in your browser
3. Enter your WiFi credentials and TfNSW API key
4. Device restarts and connects to your network

## Configuration

Edit `include/config.h` to customize:

| Setting | Default | Description |
|---------|---------|-------------|
| `WIFI_AP_SSID` | `ESP32-LCD-Setup` | Setup network name |
| `WIFI_AP_PASS` | `changeme123` | Setup network password |
| `BRIGHTNESS_DAY` | `80` | Daytime brightness (0-100) |
| `BRIGHTNESS_NIGHT` | `20` | Nighttime brightness (0-100) |

### Pin Configuration

```c
// LCD (ST7789)
LCD_PIN_MOSI  6
LCD_PIN_SCLK  7
LCD_PIN_CS    14
LCD_PIN_DC    15
LCD_PIN_RST   21
LCD_PIN_BL    22

// SD Card (optional)
SD_PIN_CS     4
SD_PIN_MISO   5
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | System status (WiFi, heap, uptime) |
| `/api/display` | POST | Display control (text, brightness) |
| `/api/system` | POST | System commands (restart, reset) |
| `/api/wifi` | POST | Update WiFi credentials |

## Project Structure

```
├── include/
│   ├── config.h          # Pin definitions, constants
│   ├── lcd_driver.h      # Display driver interface
│   ├── tfnsw_client.h    # TfNSW API client
│   ├── web_server.h      # HTTP server
│   └── wifi_manager.h    # WiFi management
├── src/
│   ├── main.c            # Application entry point
│   ├── lcd_driver.c      # ST7789 + LVGL rendering
│   ├── tfnsw_client.c    # API client logic
│   ├── web_server.c      # HTTP endpoints
│   └── wifi_manager.c    # WiFi connectivity
├── platformio.ini        # PlatformIO config
└── partitions.csv        # Flash partition layout
```

## Tech Stack

- **Framework**: ESP-IDF
- **UI**: LVGL 8.3.0
- **Build**: PlatformIO + CMake
- **JSON**: cJSON
- **RTOS**: FreeRTOS

## License

MIT

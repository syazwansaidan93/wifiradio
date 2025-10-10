# ESP32 Audio Streamer + Metadata Server

**A lightweight internet radio streamer** built from two parts:

- `esp.cpp` — ESP32 firmware that connects to a TCP audio source (Orange Pi) and plays PCM audio over I2S to a DAC or amplifier. It also drives a small SSD1306 OLED to show now-playing information.
- `server.go` — Go-based streamer & metadata service. It uses `ffmpeg` to transcode remote radio streams into raw PCM (`s16le`) and serves that PCM over TCP on port `:8888`. It also exposes a simple HTTP metadata API on `:8889` (`/nowplaying`, `/next`, `/current`, `/restart`).

This README explains hardware, configuration, build, deployment, and troubleshooting steps so you can get the system running on an Orange Pi + ESP32 setup.

---

## Table of contents

1. [Features](#features)
2. [Files in this repo](#files-in-this-repo)
3. [Requirements](#requirements)
4. [Hardware wiring](#hardware-wiring)
5. [Configuration](#configuration)
6. [Build & Run](#build--run)
   - [Server (Orange Pi)](#server-orange-pi)
   - [ESP32 firmware](#esp32-firmware)
7. [HTTP metadata API](#http-metadata-api)
8. [Usage](#usage)
9. [Troubleshooting](#troubleshooting)
10. [Contributing](#contributing)
11. [License](#license)

---

## Features

- Stream audio from remote internet radio sources using `ffmpeg` and forward raw PCM to an ESP32.
- On-demand metadata endpoint returning current song (`/nowplaying`).
- Simple station switching via `/next` and status via `/current`.
- Persistent station index stored in `state.txt` so server resumes last station after restart.
- OLED display on ESP32 shows artist + title and a small FFT-based EQ visualizer.
- Deep-sleep / wake control flow in ESP32 firmware.

---

## Files in this repo

- `esp.cpp` — ESP32 Arduino/ESP-IDF sketch (firmware).
- `server.go` — Go program that runs the streamer + metadata API.
- `config.json` — (example) list of stations (create in same folder as `server.go`).
- `state.txt` — runtime file created by server to persist current station index.

---

## Requirements

### Orange Pi / Server

- Linux (Armbian recommended on Orange Pi Zero 3)
- `ffmpeg` installed and available in PATH
- Go (if you want to build from source) or use the prebuilt binary
- Network access to target radio stream URLs

### ESP32

- ESP32 module supported by Arduino core (code uses WiFi, I2S, SSD1306 libraries)
- SSD1306 OLED (128×32)
- I2S-capable DAC or amplifier (wired to I2S pins)

---

## Hardware wiring (as used in `esp.cpp`)

- I2S pins (ESP pin numbers used in code):
  - BCK (bit clock): `GPIO 1`
  - WS (word select / LRCLK): `GPIO 2`
  - DOUT (data out): `GPIO 3`

- Sensor / wake pin:
  - `GPIO 4` used as sensor/wake input (deep-sleep wake source)

- Indicators:
  - `GPIO 8` — state indicator
  - `GPIO 5` — stream indicator

- SSD1306 OLED (I2C):
  - SDA: `GPIO 6`
  - SCL: `GPIO 7`
  - I2C address used: `0x3C`

> Adjust pins in `esp.cpp` to match your board/wiring if necessary.

---

## Configuration

Create a `config.json` file in the same folder as `server.go`. Example:

```json
{
  "stations": [
    {
      "name": "Station A",
      "streamURL": "http://stream.example.com/stream",
      "metadataURL": "http://stream.example.com/metadata.json"
    },
    {
      "name": "Station B",
      "streamURL": "http://another.example/stream",
      "metadataURL": "http://another.example/nowplaying.json"
    }
  ]
}
```

- `streamURL` is the audio stream source used by the server to feed `ffmpeg`.
- `metadataURL` should return JSON matching the small `PlaylistResponse` schema used by the Go server (an array of now playing items). If unavailable, the metadata calls will fail and the `/nowplaying` endpoint will return a 500.

---

## Build & Run

### Server (Orange Pi)

1. Install prerequisites on your Orange Pi:

```bash
sudo apt update
sudo apt install -y ffmpeg golang
```

2. Build the Go server (or run directly):

```bash
# Build binary
go build -o streamer server.go

# Run
./streamer
```

3. The server will open:

- TCP raw PCM audio on port `:8888` for the ESP32 to connect to.
- Metadata HTTP server on `:8889` with endpoints described below.

4. (Optional) Run the server as a systemd service. Example unit (adjust paths):

```ini
[Unit]
Description=ESP32 Streamer Service
After=network.target

[Service]
Type=simple
User=youruser
WorkingDirectory=/home/youruser/streamer
ExecStart=/home/youruser/streamer
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Save as `/etc/systemd/system/streamer.service`, then `sudo systemctl daemon-reload`, `sudo systemctl enable --now streamer`.

> The server stores the current station index to `state.txt` automatically.


### ESP32 firmware

This repository contains `esp.cpp` — the sketch built for the Arduino/ESP32 toolchain. Basic steps:

1. Open `esp.cpp` in Arduino IDE or PlatformIO.
2. Adjust `ssid` and `orangePiIP` (or use DHCP + domain) at the top of the file.
3. Make sure I2S and OLED pins match your wiring.
4. Install libraries used in the sketch (ArduinoJson, Adafruit_SSD1306, Adafruit_GFX, arduinoFFT, etc.).
5. Compile and flash to ESP32.

When the ESP32 boots it will connect to the Orange Pi at `tcp:8888` to receive raw PCM audio and poll `http://<orangePiIP>:8889/nowplaying` periodically for metadata. If `/restart` is called on the metadata server, the ESP will see a short disconnect and can re-connect.

---

## HTTP metadata API

The server exposes the following endpoints on port `:8889`:

- `GET /nowplaying` — Returns current song as JSON: `{ "artist": "..", "title": ".." }`.
- `POST/GET /next` — Switches to the next station and returns JSON with the new station name and index.
- `GET /current` — Returns `{ "station": "Name", "index": n }`.
- `GET /restart` — Saves state and triggers an orderly process restart (sends SIGTERM to self). Returns a JSON acknowledgement.

> Note: `/next` also tries to close the current TCP connection so streaming switches immediately.

---

## Usage

1. Start the server on the Orange Pi.
2. Flash and boot the ESP32; it should connect automatically and begin playing audio.
3. Use `curl` to query metadata or control station switching:

```bash
# Get now playing
curl http://orangepi:8889/nowplaying

# Switch station
curl http://orangepi:8889/next

# Check current
curl http://orangepi:8889/current
```

---

## Troubleshooting

- **ESP cannot connect to Orange Pi TCP port**: verify firewall rules and that the server is listening on `:8888`. Use `ss -tlnp | grep 8888` on the Orange Pi.
- **ffmpeg errors / stream failing**: check `journalctl -u streamer` or run `./streamer` in a terminal to see `ffmpeg` logs.
- **Metadata endpoint returning 500**: verify `metadataURL` in `config.json` is reachable and returns JSON in a `NowPlaying` / `PlaylistResponse` shape.
- **Persistence not working**: ensure `state.txt` is writable by the user running the service.

---

## Contributing

PRs welcome. If you want to improve the project, consider:

- Adding robust metadata parsing for different radio metadata formats.
- Making the ESP firmware configurable via HTTP or captive-portal.
- Adding TLS support or authentication for the metadata endpoints.

---

## License

This project is provided as-is. Add your preferred license (MIT/Apache-2.0) to the repo.

---

If you want, I can:

- Add example `config.json` to the repo.
- Create a `systemd` unit file and example `platformio.ini` or `arduino` instructions.
- Translate the README to Malay.

Tell me which of those you'd like and I'll update the canvas document.


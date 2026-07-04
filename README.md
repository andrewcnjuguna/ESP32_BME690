# ESP32_BME690

Multi-room ESP32-S3 environmental sensor node that streams air-quality data
to a Node.js server over Wi-Fi. Built around the Bosch BME690 with auxiliary
light, sound and battery monitoring.

![PCB](images/pcb_bm690.jpg)

## Hardware

- ESP32-S3-WROOM-1 module
- Bosch **BME690** (I2C) — temperature, humidity, pressure, IAQ, CO2eq, bVOC
- SH1106 0.96" OLED (I2C)
- TEMT6000 ambient light sensor
- Analog MEMS microphone (sound level)
- 1-cell LiPo with 1M/1M voltage divider on the battery rail
- USB-C power / programming
- Status LED

KiCad project lives in [hardware/kicad/](hardware/kicad/).

### Pin map

| Function    | GPIO |
|-------------|------|
| I2C SDA     | 8    |
| I2C SCL     | 9    |
| Status LED  | 2    |
| Light sensor (TEMT6000) | 5 |
| Microphone  | 4    |
| VBAT sense  | 1    |

## Firmware

Sketches live in [firmware/](firmware/).

- `ESP32_BME690_usb/` — main node firmware
- `battery_level/`, `light_sensor_test/`, `sound_sensor/` — bring-up sketches

### Setup

1. Open `firmware/ESP32_BME690_usb/` in the Arduino IDE.
2. Copy `secrets.h.example` → `secrets.h` and fill in your Wi-Fi credentials
   and server URL. `secrets.h` is gitignored.
3. Install required libraries: `Adafruit GFX`, `Adafruit SH110X`,
   `BSEC2` (Bosch), `ArduinoJson`, `ArduinoOTA`.
4. Build & flash. The `locationID` constant in the sketch tags this node's
   data on the server (e.g. `Kitchen`, `Living_Room`).

### Node web page (port 80)

Each node runs its own tiny status page at `http://<node-ip>/`
(auto-refreshes every 5 s). It shows the live BSEC readings, light / sound /
battery, **WiFi signal** (RSSI in dBm, an approximate 0–100 % quality, and a
label Excellent/Good/Fair/Weak/Poor), and the SD-log status. If the SD card
is mounted it also links `http://<node-ip>/datalog.csv` to download the full
log.

The node's IP is printed on the serial console at boot
(`[INFO] Web UI at http://...`).

WiFi is self-healing: if the router is down at boot (or the link drops
later), the node keeps logging to SD and retries the connection every 30 s
in the background; OTA and the web page start automatically as soon as the
first connection succeeds — no reboot needed.

### Data upload

Every 3 s BSEC cycle the node POSTs a JSON snapshot to `SERVER_URL`
(`/sensor-data` on the dashboard server). The payload includes `location`,
all sensor channels, and the WiFi link quality (`rssi` in dBm plus a derived
`wifiPercent`). Nodes running older firmware simply omit the WiFi fields —
the dashboard shows "not reported (update firmware)" for them.

## Server (dashboard)

A small Node/Express app in [server/](server/) collects POSTs from each node
and serves a per-room dashboard.

```bash
cd server
npm install express
node server.js
```

Then point a browser at `http://<host>:3000`.

- `POST /sensor-data` — nodes push their JSON snapshot here.
- `GET /data` — latest snapshot of every room, as JSON.
- `GET /` — dashboard. Room boxes are created dynamically from whatever
  `location` tags have reported, so adding a node needs **no server change**.
  Each box shows the air-quality channels plus battery and WiFi signal
  quality for that node.

The `pi_server` notes file documents the systemd / iptables setup used on
the home Raspberry Pi.

## Data logging & plotting

### SD card log

The node averages its 3 s BSEC samples and appends **one CSV row per minute**
to `/datalog.csv` on the SD card:

```
boot_id,uptime_s,epoch,synced,temp_c,hum_pct,press_hpa,iaq,iaq_acc,
co2_ppm,voc_ppm,lux,sound_db,sound_avg,sound_peak,batt_v,batt_pct
```

- `boot_id` — power-session counter (persisted in EEPROM), groups rows from
  one uninterrupted run.
- `uptime_s` — seconds since boot (monotonic, rollover-safe).
- `epoch` / `synced` — Unix time and whether NTP had synced when the row was
  written. Rows logged *before* the sync have `epoch=0`, but their true time
  is still recoverable (see below).
- `iaq_acc` — BSEC accuracy 0–3; air-quality values below your chosen
  threshold are warm-up noise.

Grab the file over WiFi from `http://<node-ip>/datalog.csv`, or pull the
card.

### Plotting with `analysis/plot_datalog.py`

```bash
pip install pandas matplotlib
python analysis/plot_datalog.py datalog.csv            # plot a local file
python analysis/plot_datalog.py --ip 192.168.0.65      # fetch straight from the node
python analysis/plot_datalog.py datalog.csv -o out.png # save instead of showing
python analysis/plot_datalog.py datalog.csv --last 48  # last 48 h only
python analysis/plot_datalog.py datalog.csv --min-accuracy 3  # strict AQ filter
```

The script produces six stacked, time-aligned panels: temp+humidity,
pressure, IAQ, CO₂eq+VOCeq, light+sound, and battery. Under the hood it:

1. **Reconstructs absolute time.** For each `boot_id` it derives
   `boot_epoch = epoch − uptime_s` from the NTP-synced rows, then stamps
   *every* row of that session as `boot_epoch + uptime_s` — so even rows
   logged before the NTP sync get correct timestamps. Sessions that never
   synced have no time anchor and are dropped with a warning.
2. **Masks warm-up air-quality data.** IAQ/CO₂/VOC are blanked where
   `iaq_acc < --min-accuracy` (default 1) so BSEC's calibration period
   doesn't draw misleading flat lines.
3. **Breaks lines at gaps.** Rows are minutely, so a jump larger than
   `--gap-min` (default 5 min) means the node was off; a NaN break is
   inserted so downtime shows as a blank gap instead of a false straight
   line across it.

## Repo layout

```
firmware/   Arduino sketches (main + bring-up)
server/     Node/Express dashboard
analysis/   plot_datalog.py — CSV log plotting
hardware/   KiCad project + design notes
images/     PCB photos
```

Component datasheets are kept locally only (not redistributable).

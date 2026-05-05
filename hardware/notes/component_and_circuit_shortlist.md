# Component and reference-circuit shortlist

## Project intent
Multi-room ESP32-based environmental sensor nodes that report to a Raspberry Pi server over Wi-Fi. First board should be reliable, easy to bring up, and suitable for later GitHub publication.

## Core design direction
- MCU/module: **ESP32-S3-WROOM-1**
- Main environmental sensor: **BME690**
- Connectivity: **Wi-Fi**
- Recommended messaging: **MQTT**
- Power: **USB-C + 1-cell LiPo support**
- User feedback: **small display + status LED(s)**

---

# 1. Main components

## 1.1 MCU
### Recommended
- **ESP32-S3-WROOM-1**

### Why
- good mainstream ESP32 module for a fresh PCB
- enough headroom for future firmware growth
- strong support ecosystem
- good for Wi-Fi sensor nodes around the house
- better long-term choice than older ESP32-WROOM-32 for a new board

### Circuit block to include
- EN reset circuit
- GPIO0 boot/programming circuit
- required decoupling near module supply pins
- antenna keepout and module placement rules
- USB/UART or native USB support depending on final programming choice

---

## 1.2 Environmental sensor
### Chosen
- **BME690**

### Why
- better low-power behavior than BME688
- better humidity response time
- more robust, especially for high-condensation applications
- supports future AI/gas-scanner exploration

### Recommended interface
- **I2C** for v1

### Circuit block to include
- 3.3 V supply
- local decoupling capacitor(s) close to the sensor
- I2C pull-ups on SDA/SCL
- optional address selection handling if needed
- careful placement away from heat sources and regulators
- venting / exposure considerations in the PCB and enclosure

### Design note
Do not place the BME690 too close to:
- ESP32 module
- charger IC
- regulator
- display backlight / high-current LED sources

---

# 2. Power architecture

## 2.1 USB-C input
### Recommendation
- USB-C used primarily as **5 V power input** and optionally for data/programming

### Circuit block to include
- USB-C receptacle
- **5.1 kΩ CC resistors** on CC1 and CC2 for sink mode if used as a power sink
- ESD protection on USB data lines if data is exposed
- input bulk capacitance
- reverse / surge / transient protection as appropriate

### Note
For v1, keep USB-C simple. Do not overcomplicate with full PD negotiation unless there is a real need.

---

## 2.2 Battery support
### Recommendation
- **1-cell LiPo / Li-ion** support

### Why
- common, compact, easy to source
- good match for room sensor nodes
- easier ecosystem than multi-cell battery designs

### Battery connector
- JST-PH style 2-pin connector is a practical starting point

---

## 2.3 Charger / power-path
### Current opinion
Do **not** automatically copy TP4056 just because many hobby boards use it.

### Better design goal
Choose a charger / power-path solution that behaves well when:
- USB is plugged in
- battery is present
- board is running at the same time

### Recommendation direction
Look for a charger/power-path IC that supports:
- single-cell LiPo charging
- system load sharing / power-path management
- USB input friendliness
- simple implementation for KiCad v1

### If simplicity wins over elegance
A TP4056-style design can still work for a first version, but it is not my preferred final direction.

### Circuit block to include
- charger IC
- charge current programming resistor
- battery connector
- battery protection strategy
- power-path / system rail handling
- charge status outputs if useful for LEDs or firmware input

---

## 2.4 3.3 V rail
### Recommendation
- stable 3.3 V regulator sized for:
  - ESP32 Wi-Fi bursts
  - sensor load
  - display load
  - future small peripherals

### Circuit block to include
- regulator input/output caps per datasheet
- enough current headroom for Wi-Fi bursts
- attention to thermal behavior

---

# 3. Programming and debug

## Recommendation
For v1, strongly consider a **simple and boring programming path**.

### Two possible directions
#### Option A. Native USB on ESP32-S3
Pros:
- fewer parts
- modern
- convenient

Cons:
- requires careful implementation and confidence in bring-up

#### Option B. Dedicated USB-UART bridge
Pros:
- very familiar and forgiving
- easier bring-up for many first boards

Cons:
- extra IC and routing

### My practical recommendation
If you want the first board to be easier to debug, a **USB-UART bridge** is still a very reasonable choice.
If you want a cleaner modern design and are comfortable with ESP32-S3 native USB, that is also valid.

### Required circuit blocks either way
- EN/reset handling
- BOOT/GPIO0 handling
- programming header or exposed test pads
- optional serial debug pads

---

# 4. Display recommendation

## You definitely want a display
I agree. A local display makes the node much more useful for:
- quick room readings
- setup and debugging
- offline confidence during bring-up

## Recommendation for v1
### Small monochrome I2C OLED
- e.g. **0.96 inch or 1.3 inch SSD1306/SH1106 class display**

### Why
- very common
- easy libraries
- low pin count over I2C
- low complexity
- good enough for temp/humidity/air quality/status

## Alternative
### Small low-power memory LCD or e-paper
Good for battery life, but adds complexity and is less convenient for a first board.

## Recommendation on using your current display
If your current display is already well understood and easy to source, reusing it is totally reasonable. For v1, reusing known-good parts is smart.

### My vote
- If current display is already stable and you like it: **reuse it**
- If not: choose a **small I2C OLED** for the first PCB

---

# 5. Extra sensors worth considering

## 5.1 Sound sensor / microphone
### Yes, this makes sense
Potential uses:
- monitor room noise levels
- detect when a room is unusually loud
- baby room monitoring triggers
- general comfort/quietness indicators

### Important distinction
There are two design levels here:

#### Level 1: sound level only
- easier
- enough for “too loud” detection
- usually the right first step

#### Level 2: event classification (crying, patterns, etc.)
- much harder
- may need better mic, DSP, firmware work, and privacy thinking

### Recommendation
For v1, add a **simple digital MEMS microphone or sound-level-capable mic front end** only if you are comfortable with the added complexity.
Otherwise, leave a footprint/header option for v2.

### My opinion
It makes sense, but I would avoid turning v1 into an audio-analysis board unless that is a top priority.

---

## 5.2 Other useful sensors to consider
### Light sensor
Useful for room context and automation.
Low complexity. Good optional add.

### PIR / occupancy / motion
Useful if you want room context, but depends on enclosure and placement.
Probably better as a later revision unless occupancy is core.

### CO2-specific sensor
If accurate CO2 is a major goal, consider a dedicated CO2 sensor later. BME690-derived CO2eq is useful, but not the same as a true NDIR CO2 reading.

---

# 6. Recommended v1 scope
To keep the first PCB sane, I recommend:
- ESP32-S3-WROOM-1
- BME690
- USB-C
- 1-cell LiPo support
- stable charger/power path
- 3.3 V regulator
- small display
- status LED
- optional expansion header

## Things I would defer unless strongly needed now
- advanced audio classification
- too many extra sensors
- fancy battery fuel gauging
- full USB-C complexity beyond what you actually need

---

# 7. Immediate next decisions
1. Pick display strategy
   - reuse current display, or
   - small I2C OLED
2. Decide whether sound sensing is:
   - required for v1, or
   - optional / reserved for v2
3. Choose charger/power-path philosophy
   - simplest workable, or
   - slightly more polished modern solution
4. Decide programming path
   - native USB on ESP32-S3, or
   - USB-UART bridge

---

# 8. My current v1 recommendation
If I were optimizing for a successful first PCB, I would build:
- **ESP32-S3-WROOM-1**
- **BME690 over I2C**
- **small I2C OLED display**
- **USB-C power/programming**
- **1-cell LiPo support**
- **status LED**
- **leave microphone as optional or v2 unless it is a core requirement**

# Unit Heart (SKU: U029) Firmware Build Specification

## Purpose

This file is a board-level firmware implementation reference for the **M5Stack Unit Heart**, a non-invasive blood-oxygen (SpO2) and heart-rate sensor module built around the **Maxim Integrated MAX30100 Pulse Oximeter and Heart-Rate Sensor IC**.

It is intended to be sufficient for implementing a correct driver and board-support integration for this specific module without needing to re-open the product page, schematic, or chip datasheet.

This module is wired to the **M5Stack Core2 for AWS IoT** through the external HY2.0-4P I2C port labeled **"PORT.A"**.

---

## 1. Board Summary

### 1.1 Function

Unit Heart is an optical pulse-oximetry and heart-rate front end. It uses two LEDs (660 nm RED and 880 nm IR) and a photodetector. Firmware configures the MAX30100, then reads optical samples (IR and RED ADC counts) from an internal 16-sample FIFO and runs heart-rate / SpO2 algorithms on the host. The raw counts modulate with the blood pulse waveform; the host extracts beats-per-minute and oxygen saturation.

### 1.2 Core IC

- Device: **MAX30100**
- Function: **Integrated pulse oximeter + heart-rate sensor** (2× LED, photodetector, 16-bit ADC, ambient-light cancellation, digital filter, on-chip temperature sensor, 16-deep FIFO)
- Interface: **I2C, up to 400 kHz**, plus an active-low open-drain `INT` pin
- ADC: continuous-time oversampling sigma-delta, up to **16-bit**
- Output data rate: **50 Hz to 1 kHz** (programmable)

### 1.3 Product-Level Facts

- Module name: **Unit Heart**
- SKU: **U029**
- Connector type: **HY2.0-4P / Grove compatible (single I2C port)**
- I2C address: **0x57 (7-bit)**
- Includes: 1× Unit Heart, 1× HY2.0-4P Grove cable (20 cm)
- Product size: 32.0 × 24.0 × 8.0 mm; weight 4.9 g
- 2× LEGO-compatible mounting holes
- Test method: run firmware, then place a fingertip on the sensor window

---

## 2. Physical / Connector Mapping

### 2.1 Module Pin Map (HY2.0-4P)

Product wire-color pin map for the module's Grove I2C port:

| Signal Group | Black | Red | Yellow | White |
|---|---:|---:|---:|---:|
| PORT.A | GND | 5V | SDA | SCL |

### 2.2 Connection to M5Stack Core2 for AWS IoT

The module plugs into the Core2 external I2C connector labeled **PORT.A**.

| Module wire | Module signal | Core2 PORT.A signal |
|---|---|---|
| Black | GND | GND |
| Red | 5V | 5V |
| Yellow | SDA | I2C SDA (PORT.A) |
| White | SCL | I2C SCL (PORT.A) |

Notes for firmware:

- PORT.A is the Core2 external (Grove) I2C bus.
- The module is powered from the connector **5 V** rail; the MAX30100 internally regulates its own 1.8 V analog supply and 3.3 V LED supply on-board.
- This is a shared I2C bus; the host driver must coexist with any other PORT.A devices.

### 2.3 MAX30100 Pin Reference (chip-level, for context)

| Pin | Name | Function |
|---:|---|---|
| 2 | SCL | I2C clock input |
| 3 | SDA | I2C data, bidirectional open-drain |
| 4 | PGND | LED driver power ground |
| 5 | IR_DRV | IR LED cathode / driver |
| 6 | R_DRV | RED LED cathode / driver |
| 9 | R_LED+ | RED LED anode supply |
| 10 | IR_LED+ | IR LED anode supply |
| 11 | VDD | Analog supply (1.8 V) |
| 12 | GND | Analog ground |
| 13 | INT | Active-low open-drain interrupt |
| 1,7,8,14 | N.C. | No connection |

---

## 3. Board Hardware Topology

From the Unit Heart schematic:

- The MAX30100 sits on the module's internal I2C bus (`CHIP_SDA`, `CHIP_SCL`), level/route to the connector `SDA`/`SCL`.
- I2C lines and the `INT` line have **4.7 kΩ pull-ups**.
- On-board regulation provides the **1.8 V VDD** analog rail and the **3.3 V LED** rail from the connector 5 V input; decoupling capacitors (e.g. 1 µF / 10 µF / 100 nF) are placed near the device.
- The `INT` pin is available on the board but is exposed through the chip's open-drain output; many host integrations poll over I2C instead of wiring `INT` to a host GPIO. Do not assume `INT` is connected to a Core2 GPIO unless verified.

Firmware should not assume hardware `INT` is available on PORT.A. Use **register polling** of the interrupt-status / FIFO pointers as the portable default.

---

## 4. MAX30100 Device Facts Used By Firmware

### 4.1 Supply and Interface Limits

- Analog supply `VDD`: **1.7 V – 2.0 V** (1.8 V typical, regulated on-module)
- LED supply `LED+`: **3.1 V – 5.0 V** (3.3 V typical, regulated on-module)
- I2C clock frequency: **0 – 400 kHz**
- Shutdown current: **0.7 µA typical**
- ADC resolution: up to **16 bits**

### 4.2 Important Behavioral Rules

1. The device powers up with **all registers at 0x00** (mode = 0 = unused). Firmware must explicitly program a mode.
2. **MODE[2:0] must be set** to `0x02` (HR only) or `0x03` (SpO2) to start sampling. `0x00`, `0x01`, and `0x04`–`0x07` are not valid operating modes.
3. The data FIFO is **16 samples deep**; each sample is **4 bytes** (IR word + RED word).
4. Reading `FIFO_DATA` (0x05) **does not auto-increment the register address**; it pops the FIFO. Read 4 bytes to get one sample.
5. All other registers auto-increment the address pointer on burst reads.
6. On entering a mode, firmware should **clear FIFO_WR_PTR, OVF_COUNTER, and FIFO_RD_PTR to 0x00** so the FIFO starts in a known, empty state.
7. Mode changes do **not** clear FIFO pointers or stored data automatically.
8. The `PWR_RDY` interrupt cannot be disabled; it fires on power-up / brownout recovery.
9. The on-chip temperature sensor is a **single-shot** measurement (`TEMP_EN` self-clears). It is optional, used to compensate SpO2; not needed for HR-only mode.

---

## 5. I2C Addressing

### 5.1 Address Encoding

The MAX30100 has a **fixed** 7-bit slave address (no address-strap pins).

| Form | Value |
|---|---:|
| 7-bit address | **0x57** |
| 8-bit write address | **0xAE** |
| 8-bit read address | **0xAF** |

Fixed slave-ID bits B7..B1 = `0b1010111`, with B0 = R/W.

### 5.2 Recommended board default constant

```c
#define UNIT_HEART_I2C_ADDR  0x57   /* 7-bit */
```

---

## 6. Register Map

| Register | Addr | Access | POR | Key fields |
|---|---:|---|---:|---|
| Interrupt Status | 0x00 | R | 0x00 | A_FULL[7], TEMP_RDY[6], HR_RDY[5], SPO2_RDY[4], PWR_RDY[0] |
| Interrupt Enable | 0x01 | R/W | 0x00 | ENB_A_FULL[7], ENB_TEMP_RDY[6], ENB_HR_RDY[5], ENB_SPO2_RDY[4] |
| FIFO Write Pointer | 0x02 | R/W | 0x00 | FIFO_WR_PTR[3:0] |
| Overflow Counter | 0x03 | R/W | 0x00 | OVF_COUNTER[3:0] |
| FIFO Read Pointer | 0x04 | R/W | 0x00 | FIFO_RD_PTR[3:0] |
| FIFO Data | 0x05 | R/W | 0x00 | FIFO_DATA[7:0] (does not auto-increment) |
| Mode Configuration | 0x06 | R/W | 0x00 | SHDN[7], RESET[6], TEMP_EN[3], MODE[2:0] |
| SPO2 Configuration | 0x07 | R/W | 0x00 | SPO2_HI_RES_EN[6], SPO2_SR[4:2], LED_PW[1:0] |
| LED Configuration | 0x09 | R/W | 0x00 | RED_PA[7:4], IR_PA[3:0] |
| Temp Integer | 0x16 | R/W | 0x00 | TINT[7:0] (two's complement °C) |
| Temp Fraction | 0x17 | R/W | 0x00 | TFRAC[3:0] (1/16 °C steps) |
| Revision ID | 0xFE | R | — | REV_ID[7:0] |
| Part ID | 0xFF | R | 0x11 | PART_ID[7:0] = **0x11** |

### 6.1 Interrupt Status (0x00, read-only)

Reading this register, or the register that triggered the interrupt, clears the interrupt and releases the `INT` pin.

| Bit | Name | Meaning |
|---:|---|---|
| 7 | A_FULL | FIFO almost full (one unwritten slot left) |
| 6 | TEMP_RDY | Temperature conversion complete |
| 5 | HR_RDY | New HR sample ready (one IR point) |
| 4 | SPO2_RDY | New SpO2 sample ready (one IR + one RED point) |
| 0 | PWR_RDY | Power-ready after power-up / brownout (cannot be masked) |

### 6.2 Interrupt Enable (0x01)

Same bit positions as 0x00 for A_FULL, TEMP_RDY, HR_RDY, SPO2_RDY. Bits [3:0] must stay 0. When an enable bit is 0, the status bit still sets but `INT` is not pulled low.

### 6.3 Mode Configuration (0x06)

| Bit | Name | Meaning |
|---:|---|---|
| 7 | SHDN | 1 = power-save (registers retained, interrupts cleared) |
| 6 | RESET | 1 = reset all config/data registers to POR; self-clears |
| 3 | TEMP_EN | 1 = start one temperature reading; self-clears |
| 2:0 | MODE | Operating mode (see table) |

#### MODE[2:0]

| MODE | Meaning |
|---|---|
| 000 | Unused |
| 001 | Reserved (do not use) |
| 010 | **HR only** |
| 011 | **SpO2** |
| 100–111 | Unused |

### 6.4 SPO2 Configuration (0x07)

| Bit | Name | Meaning |
|---:|---|---|
| 6 | SPO2_HI_RES_EN | Set 1 for 16-bit resolution with 1.6 ms pulse width |
| 5 | Reserved | Set 0 |
| 4:2 | SPO2_SR | Sample-rate select |
| 1:0 | LED_PW | LED pulse width / ADC integration time |

#### SPO2_SR[2:0] sample rate

| SPO2_SR | Samples/s |
|---|---:|
| 000 | 50 |
| 001 | 100 |
| 010 | 167 |
| 011 | 200 |
| 100 | 400 |
| 101 | 600 |
| 110 | 800 |
| 111 | 1000 |

#### LED_PW[1:0] pulse width / resolution

| LED_PW | Pulse width (µs) | ADC bits |
|---|---:|---:|
| 00 | 200 | 13 |
| 01 | 400 | 14 |
| 10 | 800 | 15 |
| 11 | 1600 | 16 |

Sample rate and pulse width are coupled: a high sample rate caps the usable pulse width. If an invalid combination is written, the device clamps to the highest sample rate allowed for that pulse width. Valid combinations:

| Samples/s | 200 µs | 400 µs | 800 µs | 1600 µs |
|---|:--:|:--:|:--:|:--:|
| 50 | ok | ok | ok | ok |
| 100 | ok | ok | ok | ok |
| 167 | ok | ok | ok | — |
| 200 | ok | ok | ok | — |
| 400 | ok | ok | — | — |
| 600 | ok | (HR only) | — | — |
| 800 | ok | (HR only) | — | — |
| 1000 | ok | (HR only) | — | — |

(For SpO2 mode the 600–1000 sps rows allow only 200 µs; HR-only mode additionally allows 400 µs at those rates.)

### 6.5 LED Configuration (0x09)

| Bits | Field | Controls |
|---|---|---|
| 7:4 | RED_PA | RED LED current |
| 3:0 | IR_PA | IR LED current |

#### LED current (typical, both fields share this scale)

| Code | mA | Code | mA |
|---|---:|---|---:|
| 0000 | 0.0 | 1000 | 27.1 |
| 0001 | 4.4 | 1001 | 30.6 |
| 0010 | 7.6 | 1010 | 33.8 |
| 0011 | 11.0 | 1011 | 37.0 |
| 0100 | 14.2 | 1100 | 40.2 |
| 0101 | 17.4 | 1101 | 43.6 |
| 0110 | 20.8 | 1110 | 46.8 |
| 0111 | 24.0 | 1111 | 50.0 |

> Actual LED current per part varies due to proprietary trim. In HR-only mode the RED LED is inactive; `RED_PA` is ignored.

### 6.6 Temperature Data (0x16 / 0x17)

```
T_measured = TINT + TFRAC
```

- `TINT` (0x16): integer °C, **two's complement** (0x00 = 0 °C, 0x7F = +127, 0x80 = -128, 0xFF = -1).
- `TFRAC` (0x17): bits [3:0], each LSB = **0.0625 °C** (1/16). Always added as a positive fraction even with a negative integer (e.g. -128 °C + 0.5 °C = -127.5 °C).

---

## 7. FIFO Data Structure

- Depth: **16 samples**, 64 bytes total.
- Each sample = **4 bytes**: `IR[15:8]`, `IR[7:0]`, `RED[15:8]`, `RED[7:0]`.
- Data is **left-justified** (MSB always at bit 15 regardless of ADC resolution).
- In **HR-only mode**, the 3rd and 4th bytes (RED) return zeros; the 4-byte structure is unchanged.
- `FIFO_RD_PTR` auto-increments after each full 4-byte sample is read.

### 7.1 Pointer arithmetic

```text
num_available = (FIFO_WR_PTR - FIFO_RD_PTR) & 0x0F   /* 4-bit wrap */
```

`OVF_COUNTER` (0x03) counts dropped samples when the FIFO overflows; it saturates at 0x0F and resets when a sample is popped.

---

## 8. I2C Transactions Required By Firmware

### 8.1 Write one or more registers

```text
START
send 0x57 + write bit          (0xAE byte)
send register address
send data byte(s)              (address auto-increments)
STOP
```

### 8.2 Read one or more registers

```text
START
send 0x57 + write bit
send register address
REPEATED START
send 0x57 + read bit           (0xAF byte)
read data byte(s)              (address auto-increments, except 0x05)
STOP
```

### 8.3 Read N samples from the FIFO

```text
1. read FIFO_WR_PTR (0x02) and FIFO_RD_PTR (0x04)
2. num = (WR_PTR - RD_PTR) & 0x0F
3. set register pointer to FIFO_DATA (0x05)
4. for i in 0..num-1:
       read 4 bytes -> IR_hi, IR_lo, RED_hi, RED_lo
       IR  = (IR_hi  << 8) | IR_lo
       RED = (RED_hi << 8) | RED_lo
5. STOP
```

Because 0x05 does not auto-increment, the same address is re-read for every byte and the FIFO read pointer advances internally.

---

## 9. Recommended Configuration Sequences

### 9.1 Heart-rate only

```text
1. write 0x06 = 0x40                 (RESET); wait for RESET bit to self-clear
2. clear FIFO: write 0x02=0x00, 0x03=0x00, 0x04=0x00
3. write 0x01 = 0xA0                 (enable A_FULL + HR_RDY interrupts) [optional]
4. write 0x07 = (SPO2_HI_RES_EN=0) | SPO2_SR=100sps(001) | LED_PW=400us(01)
                e.g. 0x07 = 0b0_0_001_01 = 0x05
5. write 0x09 = IR_PA only (RED ignored), e.g. IR_PA=0x0C (~40 mA) -> 0x0C
6. write 0x06 = 0x02                 (MODE = HR only)
7. poll INT status / FIFO and read samples (IR only)
```

### 9.2 SpO2 (heart rate + blood oxygen)

```text
1. write 0x06 = 0x40                 (RESET); wait for self-clear
2. clear FIFO: 0x02=0x00, 0x03=0x00, 0x04=0x00
3. write 0x01 = 0xF0                 (enable all maskable interrupts) [optional]
4. write 0x07 = SPO2_HI_RES_EN(1) | SPO2_SR=100sps(001) | LED_PW=1600us(11)
                = 0b0_1_001_11 = 0x47
5. write 0x09 = RED_PA<<4 | IR_PA, e.g. RED=0x0C, IR=0x0C -> 0xCC
6. (optional) write 0x06 = 0x0B      (TEMP_EN | MODE=SpO2) to also start a temp reading
   or write 0x06 = 0x03              (MODE = SpO2 only)
7. poll for SPO2_RDY / A_FULL, read IR+RED sample pairs
```

> Pick `LED_PW` consistent with the chosen sample rate (see §6.4). Tune LED currents for good signal without saturation.

---

## 10. Reading Temperature (optional)

```text
1. write 0x06 with TEMP_EN=1 (bit 3) keeping current MODE bits
2. poll Interrupt Status (0x00) for TEMP_RDY, or wait ~29 ms
3. read 0x16 (TINT) and 0x17 (TFRAC)
4. temp_c = (int8_t)TINT + (TFRAC & 0x0F) * 0.0625
```

---

## 11. Recommended Firmware Model

### 11.1 Driver responsibilities

1. Probe/verify the device by reading **Part ID (0xFF) == 0x11**.
2. Reset and clear FIFO pointers before entering a mode.
3. Configure SPO2 config (sample rate, pulse width, resolution) and LED currents.
4. Select HR-only or SpO2 mode.
5. Read FIFO samples (handling the non-incrementing 0x05 and pointer wrap).
6. Optionally read temperature for SpO2 compensation.
7. Provide shutdown (SHDN) for low power.

### 11.2 Suggested internal state

```c
struct unit_heart_state {
    uint8_t  i2c_addr;     /* 0x57 */
    uint8_t  mode;         /* last MODE: 0x02 HR, 0x03 SpO2 */
    uint8_t  spo2_cfg;     /* cached 0x07 value */
    uint8_t  led_cfg;      /* cached 0x09 value */
};
```

### 11.3 Canonical constants

```c
#define MAX30100_REG_INT_STATUS   0x00
#define MAX30100_REG_INT_ENABLE   0x01
#define MAX30100_REG_FIFO_WR_PTR  0x02
#define MAX30100_REG_OVF_COUNTER  0x03
#define MAX30100_REG_FIFO_RD_PTR  0x04
#define MAX30100_REG_FIFO_DATA    0x05
#define MAX30100_REG_MODE_CONFIG  0x06
#define MAX30100_REG_SPO2_CONFIG  0x07
#define MAX30100_REG_LED_CONFIG   0x09
#define MAX30100_REG_TEMP_INT     0x16
#define MAX30100_REG_TEMP_FRAC    0x17
#define MAX30100_REG_REV_ID       0xFE
#define MAX30100_REG_PART_ID      0xFF

#define MAX30100_PART_ID          0x11

#define MAX30100_MODE_SHDN        0x80
#define MAX30100_MODE_RESET       0x40
#define MAX30100_MODE_TEMP_EN     0x08
#define MAX30100_MODE_HR          0x02
#define MAX30100_MODE_SPO2        0x03

#define MAX30100_SPO2_HI_RES_EN   0x40
#define MAX30100_FIFO_DEPTH       16
```

### 11.4 Minimal C API

```c
int unit_heart_init(struct unit_heart_state *dev, uint8_t i2c_addr);
int unit_heart_set_mode(struct unit_heart_state *dev, uint8_t mode);
int unit_heart_set_spo2_config(struct unit_heart_state *dev, uint8_t sample_rate, uint8_t pulse_width, bool hi_res);
int unit_heart_set_leds(struct unit_heart_state *dev, uint8_t red_pa, uint8_t ir_pa);
int unit_heart_read_fifo(struct unit_heart_state *dev, uint16_t *ir, uint16_t *red, size_t max, size_t *count_out);
int unit_heart_read_temp(struct unit_heart_state *dev, float *temp_c);
int unit_heart_shutdown(struct unit_heart_state *dev, bool enable);
```

---

## 12. Failure Modes and Recovery

### 12.1 Common runtime problems

1. Forgetting to set a valid MODE (device stays idle with MODE=0).
2. Treating `FIFO_DATA` (0x05) as auto-incrementing, corrupting sample alignment.
3. Not clearing FIFO pointers before a new acquisition, so stale data is read.
4. Choosing an illegal sample-rate / pulse-width pair (device silently clamps).
5. Assuming a hardware `INT` GPIO exists on PORT.A; use I2C polling instead.
6. Pointer-wrap math without the `& 0x0F` mask, giving wrong sample counts.
7. RED current set in HR-only mode and expecting RED data (RED bytes are 0).

### 12.2 Recovery recommendations

1. Read Part ID (0xFF) == 0x11 to confirm the device is present and addressed.
2. Issue RESET (0x06 bit 6) and wait for it to self-clear.
3. Re-clear FIFO pointers (0x02/0x03/0x04 = 0x00).
4. Re-apply SPO2 config and LED config, then re-select the mode.
5. If the bus is locked, power-cycle the module supply at the host level.

---

## 13. Safety and Correctness Rules for Code Generation

1. The I2C address is **fixed at 0x57** (8-bit 0xAE write / 0xAF read); there are no address straps.
2. Confirm presence by reading **Part ID 0xFF == 0x11**.
3. Always set a valid **MODE** (`0x02` HR or `0x03` SpO2) — POR leaves it idle.
4. Clear **FIFO_WR_PTR / OVF_COUNTER / FIFO_RD_PTR** to 0 before each acquisition.
5. **FIFO_DATA (0x05) does not auto-increment**; read in 4-byte sample units.
6. Use `(WR_PTR - RD_PTR) & 0x0F` for available-sample count (4-bit wrap).
7. Each sample is `IR[15:8], IR[7:0], RED[15:8], RED[7:0]`, left-justified.
8. Pick `SPO2_SR` and `LED_PW` from the **allowed combinations**; invalid pairs are clamped.
9. Keep I2C clock at or below **400 kHz**.
10. In HR-only mode, RED data is zero; do not rely on it.
11. Prefer **register polling** over hardware `INT` for portability on PORT.A.
12. Temperature is single-shot (`TEMP_EN` self-clears) and optional, used for SpO2 compensation.

---

## 14. Board-Level Quick Reference

### Identity

| Item | Value |
|---|---|
| Module | Unit Heart (U029) |
| Sensor IC | MAX30100 |
| Bus | I2C, ≤ 400 kHz, PORT.A on Core2 |
| 7-bit address | 0x57 |
| Part ID (0xFF) | 0x11 |

### Mode quick map

| MODE byte (0x06[2:0]) | Result |
|---|---|
| 0x02 | Heart-rate only (IR) |
| 0x03 | SpO2 (IR + RED) |

### Sample format (per FIFO read of 4 bytes)

| Byte | Content |
|---:|---|
| 0 | IR[15:8] |
| 1 | IR[7:0] |
| 2 | RED[15:8] (0 in HR-only mode) |
| 3 | RED[7:0] (0 in HR-only mode) |

---

## 15. Final Implementation Intent

A generated firmware driver for Unit Heart should behave as a small, deterministic board-support layer over the MAX30100:

- verify the part (Part ID 0x11) at the fixed address 0x57
- reset and clear FIFO pointers
- configure sample rate, pulse width / resolution, and LED currents
- select HR-only or SpO2 mode
- poll and drain the 16-deep FIFO in 4-byte sample units, honoring the non-incrementing FIFO_DATA register and 4-bit pointer wrap
- optionally read the on-chip temperature for SpO2 compensation

The most common mistakes to avoid are leaving MODE at its idle POR value, mishandling the FIFO_DATA register's non-incrementing behavior, and choosing an illegal sample-rate / pulse-width combination. The raw IR/RED counts are inputs to host-side heart-rate and SpO2 algorithms; the device itself only delivers filtered optical samples.

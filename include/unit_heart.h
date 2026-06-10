/*!
 * @brief Library for the Unit Heart (U029) MAX30100 pulse-oximeter and
 * heart-rate sensor by M5Stack on the Core2 for AWS IoT Kit.
 *
 * @copyright Copyright (c) 2026 by Rashed Talukder[https://rashedtalukder.com]
 *
 * @license SPDX-License-Identifier: Apache 2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @Links [Unit Heart](https://docs.m5stack.com/en/unit/heart)
 *
 * @version  V0.0.1
 * @date  2026-06-10
 */

#ifndef _UNIT_HEART_H_
#define _UNIT_HEART_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

  /** @brief 7-bit I2C address of the Unit Heart (MAX30100). Fixed, no straps. */
#define UNIT_HEART_I2C_ADDR        0x57

  /** @brief Expected Part ID value read from register 0xFF. */
#define UNIT_HEART_PART_ID         0x11

  /** @brief Depth of the MAX30100 sample FIFO (16 samples, 4 bytes each). */
#define UNIT_HEART_FIFO_DEPTH      16

  /* ---- Register map (datasheet section 6) -------------------------------- */
#define UNIT_HEART_REG_INT_STATUS  0x00 /*!< Interrupt status (read clears) */
#define UNIT_HEART_REG_INT_ENABLE  0x01 /*!< Interrupt enable mask */
#define UNIT_HEART_REG_FIFO_WR_PTR 0x02 /*!< FIFO write pointer [3:0] */
#define UNIT_HEART_REG_OVF_COUNTER 0x03 /*!< FIFO overflow counter [3:0] */
#define UNIT_HEART_REG_FIFO_RD_PTR 0x04 /*!< FIFO read pointer [3:0] */
#define UNIT_HEART_REG_FIFO_DATA   0x05 /*!< FIFO data (does NOT auto-increment) */
#define UNIT_HEART_REG_MODE_CONFIG 0x06 /*!< Mode configuration */
#define UNIT_HEART_REG_SPO2_CONFIG 0x07 /*!< SpO2 configuration */
#define UNIT_HEART_REG_LED_CONFIG  0x09 /*!< LED current configuration */
#define UNIT_HEART_REG_TEMP_INT    0x16 /*!< Temperature integer (two's complement) */
#define UNIT_HEART_REG_TEMP_FRAC   0x17 /*!< Temperature fraction (1/16 deg C) */
#define UNIT_HEART_REG_REV_ID      0xFE /*!< Revision ID */
#define UNIT_HEART_REG_PART_ID     0xFF /*!< Part ID (== 0x11) */

  /* ---- Interrupt status / enable bits (registers 0x00 / 0x01) ------------ */
#define UNIT_HEART_INT_A_FULL      ( 1 << 7 ) /*!< FIFO almost full */
#define UNIT_HEART_INT_TEMP_RDY    ( 1 << 6 ) /*!< Temperature ready */
#define UNIT_HEART_INT_HR_RDY      ( 1 << 5 ) /*!< Heart-rate sample ready */
#define UNIT_HEART_INT_SPO2_RDY    ( 1 << 4 ) /*!< SpO2 sample ready */
#define UNIT_HEART_INT_PWR_RDY     ( 1 << 0 ) /*!< Power ready (cannot be masked) */

  /* ---- Mode configuration bits (register 0x06) --------------------------- */
#define UNIT_HEART_MODE_SHDN       ( 1 << 7 ) /*!< Shutdown / power-save */
#define UNIT_HEART_MODE_RESET      ( 1 << 6 ) /*!< Reset (self-clearing) */
#define UNIT_HEART_MODE_TEMP_EN    ( 1 << 3 ) /*!< Start temp reading (self-clearing) */

  /* ---- SpO2 configuration bits (register 0x07) --------------------------- */
#define UNIT_HEART_SPO2_HI_RES_EN  ( 1 << 6 ) /*!< 16-bit resolution enable */

  /**
   * @brief Operating modes for the MAX30100 (MODE[2:0] of register 0x06).
   *
   * Power-on reset leaves the device idle (MODE = 0). A valid mode MUST be
   * programmed before the device produces samples.
   */
  typedef enum
  {
    UNIT_HEART_MODE_HEART_RATE = 0x02, /*!< Heart-rate only (IR channel) */
    UNIT_HEART_MODE_SPO2       = 0x03, /*!< SpO2 (IR + RED channels) */
  } unit_heart_mode_t;

  /**
   * @brief Sample-rate selections (SPO2_SR[2:0] of register 0x07).
   *
   * Sample rate and pulse width are coupled; an illegal combination is
   * silently clamped by the device (datasheet section 6.4).
   */
  typedef enum
  {
    UNIT_HEART_SR_50HZ   = 0x00, /*!< 50 samples/s */
    UNIT_HEART_SR_100HZ  = 0x01, /*!< 100 samples/s */
    UNIT_HEART_SR_167HZ  = 0x02, /*!< 167 samples/s */
    UNIT_HEART_SR_200HZ  = 0x03, /*!< 200 samples/s */
    UNIT_HEART_SR_400HZ  = 0x04, /*!< 400 samples/s */
    UNIT_HEART_SR_600HZ  = 0x05, /*!< 600 samples/s */
    UNIT_HEART_SR_800HZ  = 0x06, /*!< 800 samples/s */
    UNIT_HEART_SR_1000HZ = 0x07, /*!< 1000 samples/s */
  } unit_heart_sample_rate_t;

  /**
   * @brief LED pulse-width / ADC-resolution selections (LED_PW[1:0] of 0x07).
   */
  typedef enum
  {
    UNIT_HEART_PW_200US  = 0x00, /*!< 200 us pulse width, 13-bit ADC */
    UNIT_HEART_PW_400US  = 0x01, /*!< 400 us pulse width, 14-bit ADC */
    UNIT_HEART_PW_800US  = 0x02, /*!< 800 us pulse width, 15-bit ADC */
    UNIT_HEART_PW_1600US = 0x03, /*!< 1600 us pulse width, 16-bit ADC */
  } unit_heart_pulse_width_t;

  /**
   * @brief LED current selections (RED_PA / IR_PA of register 0x09).
   *
   * Values are the nominal milliamp currents; actual current varies per part
   * due to proprietary trim (datasheet section 6.5).
   */
  typedef enum
  {
    UNIT_HEART_LED_0_0MA  = 0x00, /*!< 0.0 mA (LED off) */
    UNIT_HEART_LED_4_4MA  = 0x01, /*!< 4.4 mA */
    UNIT_HEART_LED_7_6MA  = 0x02, /*!< 7.6 mA */
    UNIT_HEART_LED_11_0MA = 0x03, /*!< 11.0 mA */
    UNIT_HEART_LED_14_2MA = 0x04, /*!< 14.2 mA */
    UNIT_HEART_LED_17_4MA = 0x05, /*!< 17.4 mA */
    UNIT_HEART_LED_20_8MA = 0x06, /*!< 20.8 mA */
    UNIT_HEART_LED_24_0MA = 0x07, /*!< 24.0 mA */
    UNIT_HEART_LED_27_1MA = 0x08, /*!< 27.1 mA */
    UNIT_HEART_LED_30_6MA = 0x09, /*!< 30.6 mA */
    UNIT_HEART_LED_33_8MA = 0x0A, /*!< 33.8 mA */
    UNIT_HEART_LED_37_0MA = 0x0B, /*!< 37.0 mA */
    UNIT_HEART_LED_40_2MA = 0x0C, /*!< 40.2 mA */
    UNIT_HEART_LED_43_6MA = 0x0D, /*!< 43.6 mA */
    UNIT_HEART_LED_46_8MA = 0x0E, /*!< 46.8 mA */
    UNIT_HEART_LED_50_0MA = 0x0F, /*!< 50.0 mA */
  } unit_heart_led_current_t;

  /* ---- Sensible defaults -------------------------------------------------
   * These let an application bring the sensor up with a single call and get
   * clean, unsaturated readings without having to understand the MAX30100
   * sample-rate / pulse-width coupling or LED-current trade-offs. They are
   * applied automatically by @ref unit_heart_init and @ref unit_heart_start,
   * and can be overridden afterwards with the granular setters. */

  /** @brief Default sample rate: 100 samples/s (a good HR/SpO2 rate). */
#define UNIT_HEART_DEFAULT_SAMPLE_RATE UNIT_HEART_SR_100HZ
  /** @brief Default LED pulse width: 1600 us (full 16-bit ADC resolution). */
#define UNIT_HEART_DEFAULT_PULSE_WIDTH UNIT_HEART_PW_1600US
  /** @brief Default high-resolution mode: enabled. */
#define UNIT_HEART_DEFAULT_HI_RES      true
  /** @brief Default RED LED current: 11 mA (avoids ADC saturation on contact). */
#define UNIT_HEART_DEFAULT_RED_CURRENT UNIT_HEART_LED_11_0MA
  /** @brief Default IR LED current: 24 mA (strong IR signal for HR detection). */
#define UNIT_HEART_DEFAULT_IR_CURRENT  UNIT_HEART_LED_24_0MA
  /** @brief Default operating mode used by @ref unit_heart_start when unspecified. */
#define UNIT_HEART_DEFAULT_MODE        UNIT_HEART_MODE_SPO2

  /**
   * @brief A single optical sample read from the MAX30100 FIFO.
   *
   * Both fields are left-justified 16-bit ADC counts. In heart-rate-only mode
   * the @ref red channel is always zero.
   */
  typedef struct
  {
    uint16_t ir;  /*!< Infrared (880 nm) ADC counts */
    uint16_t red; /*!< Red (660 nm) ADC counts; 0 in HR-only mode */
  } unit_heart_sample_t;

  /**
   * @brief Initialize the Unit Heart driver and the MAX30100 device.
   *
   * Registers the device on the Core2 PORT.A (external) I2C bus, verifies the
   * part by reading Part ID (0xFF) == 0x11, issues a soft reset, clears the
   * FIFO pointers, and applies the sensible default configuration (see the
   * UNIT_HEART_DEFAULT_* macros: 100 sps, 1600 us pulse width, high-resolution,
   * 11 mA RED / 24 mA IR). The device is left idle (no mode selected) so it
   * draws minimal current until you start sampling.
   *
   * After this call you can simply enter a mode with @ref unit_heart_set_mode
   * (or use @ref unit_heart_start to do both in one step) and then read samples
   * with @ref unit_heart_read_fifo — no manual register tuning required. The
   * granular setters remain available if you need to override the defaults.
   *
   * Safe to call once; subsequent calls without an intervening
   * @ref unit_heart_deinit return ESP_OK.
   *
   * @return
   *  - ESP_OK                : Success
   *  - ESP_FAIL              : Failed to create the internal mutex
   *  - ESP_ERR_NOT_FOUND     : Part ID mismatch (device not present)
   *  - others                : Propagated I2C / device-add errors
   */
  esp_err_t unit_heart_init( void );

  /**
   * @brief Start sampling in one call using the default configuration.
   *
   * Convenience entry point that re-applies the default SpO2 configuration and
   * LED currents (UNIT_HEART_DEFAULT_* macros), clears the FIFO, and enters the
   * requested mode. After this returns, samples can be read directly with
   * @ref unit_heart_read_fifo. Use @ref UNIT_HEART_DEFAULT_MODE for the typical
   * SpO2 + heart-rate acquisition.
   *
   * @param[in] mode One of @ref unit_heart_mode_t (e.g. UNIT_HEART_MODE_SPO2).
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : Invalid mode
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_start( unit_heart_mode_t mode );

  /**
   * @brief Deinitialize the driver and release all resources.
   *
   * Puts the MAX30100 into shutdown, removes the I2C device registration, and
   * deletes the internal mutex.
   *
   * @return ESP_OK on success.
   */
  esp_err_t unit_heart_deinit( void );

  /**
   * @brief Issue a soft reset of the MAX30100.
   *
   * Sets the RESET bit and waits for it to self-clear, returning all
   * configuration and data registers to their power-on state.
   *
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - ESP_ERR_TIMEOUT       : RESET did not self-clear, or mutex timeout
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_reset( void );

  /**
   * @brief Read the Part ID register (0xFF).
   *
   * @param[out] part_id Receives the part identifier (expected 0x11). Must not
   *                     be NULL.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : part_id is NULL
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_get_part_id( uint8_t *part_id );

  /**
   * @brief Read the Revision ID register (0xFE).
   *
   * @param[out] rev_id Receives the revision identifier. Must not be NULL.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : rev_id is NULL
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_get_revision_id( uint8_t *rev_id );

  /**
   * @brief Clear the FIFO write, overflow, and read pointers to zero.
   *
   * Should be called before starting a new acquisition so the FIFO begins in a
   * known, empty state (datasheet section 4.2, rule 6).
   *
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_clear_fifo( void );

  /**
   * @brief Select the operating mode (heart-rate only or SpO2).
   *
   * Power-on reset leaves the device idle; this must be called to start
   * sampling. The FIFO pointers are cleared as part of entering the mode.
   *
   * @param[in] mode One of @ref unit_heart_mode_t.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : Invalid mode
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_set_mode( unit_heart_mode_t mode );

  /**
   * @brief Configure the SpO2 sample rate, LED pulse width, and resolution.
   *
   * Writes the SpO2 configuration register (0x07). Choose a sample-rate /
   * pulse-width pair from the allowed combinations in the datasheet; illegal
   * pairs are silently clamped by the device.
   *
   * @param[in] sample_rate One of @ref unit_heart_sample_rate_t.
   * @param[in] pulse_width One of @ref unit_heart_pulse_width_t.
   * @param[in] hi_res      true to enable 16-bit high-resolution mode.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : Invalid sample rate or pulse width
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_set_spo2_config( unit_heart_sample_rate_t sample_rate,
                                        unit_heart_pulse_width_t pulse_width,
                                        bool hi_res );

  /**
   * @brief Configure the RED and IR LED drive currents (register 0x09).
   *
   * In heart-rate-only mode the RED LED is inactive and @p red_pa is ignored
   * by the device.
   *
   * @param[in] red_pa RED LED current, one of @ref unit_heart_led_current_t.
   * @param[in] ir_pa  IR LED current, one of @ref unit_heart_led_current_t.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : Current code out of range
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_set_leds( unit_heart_led_current_t red_pa,
                                 unit_heart_led_current_t ir_pa );

  /**
   * @brief Read the interrupt-status register (0x00).
   *
   * Reading this register clears the latched interrupt flags and releases the
   * INT line.
   *
   * @param[out] status Receives the interrupt-status bitfield. Must not be NULL.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : status is NULL
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_read_interrupt_status( uint8_t *status );

  /**
   * @brief Set the interrupt-enable mask (register 0x01).
   *
   * @param[in] mask Bitwise OR of UNIT_HEART_INT_A_FULL, UNIT_HEART_INT_TEMP_RDY,
   *                 UNIT_HEART_INT_HR_RDY, UNIT_HEART_INT_SPO2_RDY. The low four
   *                 bits are reserved and must remain zero.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_set_interrupt_enable( uint8_t mask );

  /**
   * @brief Return the number of unread samples currently in the FIFO.
   *
   * Computed as (FIFO_WR_PTR - FIFO_RD_PTR) & 0x0F.
   *
   * @param[out] count Receives the available sample count (0-16). Must not be
   *                   NULL.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : count is NULL
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_available( uint8_t *count );

  /**
   * @brief Drain up to @p max samples from the FIFO.
   *
   * Reads the available-sample count, then pops 4-byte samples from the
   * non-incrementing FIFO_DATA register (0x05). Each sample is decoded into a
   * left-justified IR and RED 16-bit value.
   *
   * @param[out] samples   Caller-provided array receiving the samples. Must not
   *                       be NULL when @p max > 0.
   * @param[in]  max       Capacity of the @p samples array.
   * @param[out] count_out Receives the number of samples actually read. Must not
   *                       be NULL.
   * @return
   *  - ESP_OK                : Success (including the zero-samples case)
   *  - ESP_ERR_INVALID_ARG   : NULL pointer with non-zero capacity
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_read_fifo( unit_heart_sample_t *samples, size_t max,
                                  size_t *count_out );

  /**
   * @brief Perform a single-shot on-chip temperature measurement.
   *
   * Sets TEMP_EN, waits for the TEMP_RDY status, then reads and combines the
   * integer and fractional temperature registers. Used to compensate SpO2
   * readings; optional for heart-rate-only mode.
   *
   * @param[out] temp_c Receives the temperature in degrees Celsius. Must not be
   *                    NULL.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_ARG   : temp_c is NULL
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - ESP_ERR_TIMEOUT       : Conversion did not complete, or mutex timeout
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_read_temperature( float *temp_c );

  /**
   * @brief Enter or leave power-save shutdown (SHDN bit of register 0x06).
   *
   * Configuration registers are retained across shutdown; pending interrupts
   * are cleared.
   *
   * @param[in] enable true to shut down, false to resume normal operation.
   * @return
   *  - ESP_OK                : Success
   *  - ESP_ERR_INVALID_STATE : Driver not initialized
   *  - others                : Propagated I2C errors
   */
  esp_err_t unit_heart_shutdown( bool enable );

#ifdef __cplusplus
}
#endif
#endif

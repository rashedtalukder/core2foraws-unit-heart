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

#include "unit_heart.h"
#include "core2foraws.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifdef CONFIG_UNIT_HEART_USE_PAHUB
#include "unit_pahub.h"
#endif

static const char *_TAG = "UNIT_HEART";
static SemaphoreHandle_t _heart_mutex = NULL;
static i2c_master_dev_handle_t _heart_dev = NULL;

// How long to wait for the internal mutex before giving up.
#define UNIT_HEART_MUTEX_TIMEOUT_MS 1000

// MAX30100 supports I2C up to 400 kHz (datasheet section 4.1).
#define UNIT_HEART_I2C_SPEED_HZ     400000

// Bounded poll budget for self-clearing bits (RESET, TEMP_EN). A temperature
// conversion completes in ~29 ms. The delay must be at least one FreeRTOS tick
// (10 ms at the default 100 Hz) or pdMS_TO_TICKS() rounds down to 0 and the
// loop spins without ever waiting for the conversion. 30 * 10 ms = 300 ms.
#define UNIT_HEART_POLL_RETRIES     30
#define UNIT_HEART_POLL_DELAY_MS    10

// Number of bytes per FIFO sample: IR[15:8], IR[7:0], RED[15:8], RED[7:0].
#define UNIT_HEART_BYTES_PER_SAMPLE 4

// 4-bit mask used for FIFO pointer-wrap arithmetic.
#define UNIT_HEART_FIFO_PTR_MASK    0x0F

// Reads a single device register. Caller MUST hold _heart_mutex.
static esp_err_t _heart_read_reg_locked( uint8_t reg, uint8_t *data,
                                         uint16_t length )
{
#ifdef CONFIG_UNIT_HEART_USE_PAHUB
  return unit_pahub_i2c_read( CONFIG_UNIT_HEART_PAHUB_CHANNEL, _heart_dev, reg,
                              data, length );
#else
  return core2foraws_expports_i2c_read( _heart_dev, reg, data, length );
#endif
}

// Writes a single device register. Caller MUST hold _heart_mutex.
static esp_err_t _heart_write_reg_locked( uint8_t reg, uint8_t value )
{
#ifdef CONFIG_UNIT_HEART_USE_PAHUB
  return unit_pahub_i2c_write( CONFIG_UNIT_HEART_PAHUB_CHANNEL, _heart_dev, reg,
                               &value, 1 );
#else
  return core2foraws_expports_i2c_write( _heart_dev, reg, &value, 1 );
#endif
}

// Clears the FIFO write/overflow/read pointers. Caller MUST hold _heart_mutex.
static esp_err_t _heart_clear_fifo_locked( void )
{
  esp_err_t err = _heart_write_reg_locked( UNIT_HEART_REG_FIFO_WR_PTR, 0x00 );
  if( err != ESP_OK )
  {
    return err;
  }
  err = _heart_write_reg_locked( UNIT_HEART_REG_OVF_COUNTER, 0x00 );
  if( err != ESP_OK )
  {
    return err;
  }
  return _heart_write_reg_locked( UNIT_HEART_REG_FIFO_RD_PTR, 0x00 );
}

// Issues a soft reset and waits for the RESET bit to self-clear. Caller MUST
// hold _heart_mutex.
static esp_err_t _heart_reset_locked( void )
{
  esp_err_t err = _heart_write_reg_locked( UNIT_HEART_REG_MODE_CONFIG,
                                           UNIT_HEART_MODE_RESET );
  if( err != ESP_OK )
  {
    return err;
  }

  for( int attempt = 0; attempt < UNIT_HEART_POLL_RETRIES; attempt++ )
  {
    uint8_t mode = 0;
    err = _heart_read_reg_locked( UNIT_HEART_REG_MODE_CONFIG, &mode, 1 );
    if( err != ESP_OK )
    {
      return err;
    }
    if( ( mode & UNIT_HEART_MODE_RESET ) == 0 )
    {
      return ESP_OK;
    }
    vTaskDelay( pdMS_TO_TICKS( UNIT_HEART_POLL_DELAY_MS ) );
  }

  ESP_LOGE( _TAG, "RESET bit did not self-clear" );
  return ESP_ERR_TIMEOUT;
}

// Writes the SpO2-config and LED-current registers with the library defaults so
// the sensor produces clean, unsaturated samples without app-level tuning.
// Caller MUST hold _heart_mutex.
static esp_err_t _heart_apply_defaults_locked( void )
{
  uint8_t spo2_cfg =
      (uint8_t)( ( UNIT_HEART_DEFAULT_SAMPLE_RATE & 0x07 ) << 2 )
      | (uint8_t)( UNIT_HEART_DEFAULT_PULSE_WIDTH & 0x03 );
  if( UNIT_HEART_DEFAULT_HI_RES )
  {
    spo2_cfg |= UNIT_HEART_SPO2_HI_RES_EN;
  }

  esp_err_t err = _heart_write_reg_locked( UNIT_HEART_REG_SPO2_CONFIG, spo2_cfg );
  if( err != ESP_OK )
  {
    return err;
  }

  uint8_t led_cfg = (uint8_t)( ( UNIT_HEART_DEFAULT_RED_CURRENT & 0x0F ) << 4 )
                    | (uint8_t)( UNIT_HEART_DEFAULT_IR_CURRENT & 0x0F );
  return _heart_write_reg_locked( UNIT_HEART_REG_LED_CONFIG, led_cfg );
}

esp_err_t unit_heart_init( void )
{
  if( _heart_mutex != NULL )
  {
    return ESP_OK;
  }

  _heart_mutex = xSemaphoreCreateMutex();
  if( _heart_mutex == NULL )
  {
    ESP_LOGE( _TAG, "Failed to create Unit Heart mutex" );
    return ESP_FAIL;
  }

  // The Unit Heart lives on the Core2 external (PORT.A) I2C bus, which is not
  // brought up by core2foraws_init(). Ensure it is initialized before adding
  // the device. Safe to call repeatedly.
  esp_err_t err = core2foraws_expports_i2c_begin();
  if( err != ESP_OK )
  {
    ESP_LOGE( _TAG, "Failed to initialize PORT.A I2C bus: %s",
              esp_err_to_name( err ) );
    vSemaphoreDelete( _heart_mutex );
    _heart_mutex = NULL;
    return err;
  }

#ifdef CONFIG_UNIT_HEART_USE_PAHUB
  // The Unit Heart hangs off a PA Hub channel. Bring up the hub and select the
  // configured channel before talking to the sensor.
  err = unit_pahub_init();
  if( err != ESP_OK )
  {
    ESP_LOGE( _TAG, "Failed to initialize PA Hub: %s", esp_err_to_name( err ) );
    vSemaphoreDelete( _heart_mutex );
    _heart_mutex = NULL;
    return err;
  }

  err = unit_pahub_channel_set( CONFIG_UNIT_HEART_PAHUB_CHANNEL );
  if( err != ESP_OK )
  {
    ESP_LOGE( _TAG, "Failed to select PA Hub channel %d: %s",
              CONFIG_UNIT_HEART_PAHUB_CHANNEL, esp_err_to_name( err ) );
    vSemaphoreDelete( _heart_mutex );
    _heart_mutex = NULL;
    return err;
  }
#endif

  err = core2foraws_expports_i2c_device_add(
      UNIT_HEART_I2C_ADDR, UNIT_HEART_I2C_SPEED_HZ, &_heart_dev );
  if( err != ESP_OK )
  {
    ESP_LOGE( _TAG, "Failed to add Unit Heart I2C device: %s",
              esp_err_to_name( err ) );
    vSemaphoreDelete( _heart_mutex );
    _heart_mutex = NULL;
    return err;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    err = ESP_ERR_TIMEOUT;
    goto fail;
  }

  // Confirm the device is present by reading Part ID == 0x11 (datasheet
  // section 13, rule 2).
  uint8_t part_id = 0;
  err = _heart_read_reg_locked( UNIT_HEART_REG_PART_ID, &part_id, 1 );
  if( err != ESP_OK )
  {
    ESP_LOGE( _TAG, "Failed to read Part ID: %s", esp_err_to_name( err ) );
    xSemaphoreGive( _heart_mutex );
    goto fail;
  }
  if( part_id != UNIT_HEART_PART_ID )
  {
    ESP_LOGE( _TAG, "Unexpected Part ID 0x%02X (expected 0x%02X)", part_id,
              UNIT_HEART_PART_ID );
    xSemaphoreGive( _heart_mutex );
    err = ESP_ERR_NOT_FOUND;
    goto fail;
  }

  // Bring the device to a known idle state: reset, then clear FIFO pointers.
  err = _heart_reset_locked();
  if( err == ESP_OK )
  {
    err = _heart_clear_fifo_locked();
  }
  // Apply sensible defaults so the application can sample without manual tuning.
  if( err == ESP_OK )
  {
    err = _heart_apply_defaults_locked();
  }

  xSemaphoreGive( _heart_mutex );

  if( err != ESP_OK )
  {
    ESP_LOGE( _TAG, "Failed to reset Unit Heart: %s", esp_err_to_name( err ) );
    goto fail;
  }

  ESP_LOGI( _TAG, "Unit Heart initialized successfully" );
  return ESP_OK;

fail:
  if( _heart_dev != NULL )
  {
    core2foraws_i2c_device_remove( _heart_dev );
    _heart_dev = NULL;
  }
  vSemaphoreDelete( _heart_mutex );
  _heart_mutex = NULL;
  return err;
}

esp_err_t unit_heart_start( unit_heart_mode_t mode )
{
  if( mode != UNIT_HEART_MODE_HEART_RATE && mode != UNIT_HEART_MODE_SPO2 )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    ESP_LOGE( _TAG, "Unit Heart not initialized. Call unit_heart_init() first." );
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  // Re-apply defaults (in case they were overridden), clear the FIFO, then
  // enter the requested mode so sampling begins immediately.
  esp_err_t err = _heart_apply_defaults_locked();
  if( err == ESP_OK )
  {
    err = _heart_clear_fifo_locked();
  }
  if( err == ESP_OK )
  {
    err = _heart_write_reg_locked( UNIT_HEART_REG_MODE_CONFIG, (uint8_t)mode );
  }

  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_deinit( void )
{
  if( _heart_mutex == NULL )
  {
    return ESP_OK;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) == pdTRUE )
  {
    // Put the device into low-power shutdown before tearing down.
    _heart_write_reg_locked( UNIT_HEART_REG_MODE_CONFIG, UNIT_HEART_MODE_SHDN );

    if( _heart_dev != NULL )
    {
      core2foraws_i2c_device_remove( _heart_dev );
      _heart_dev = NULL;
    }
    xSemaphoreGive( _heart_mutex );
  }

  vSemaphoreDelete( _heart_mutex );
  _heart_mutex = NULL;
  ESP_LOGI( _TAG, "Unit Heart deinitialized" );
  return ESP_OK;
}

esp_err_t unit_heart_reset( void )
{
  if( _heart_mutex == NULL )
  {
    ESP_LOGE( _TAG, "Unit Heart not initialized. Call unit_heart_init() first." );
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_reset_locked();
  if( err == ESP_OK )
  {
    err = _heart_clear_fifo_locked();
  }

  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_get_part_id( uint8_t *part_id )
{
  if( part_id == NULL )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_read_reg_locked( UNIT_HEART_REG_PART_ID, part_id, 1 );
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_get_revision_id( uint8_t *rev_id )
{
  if( rev_id == NULL )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_read_reg_locked( UNIT_HEART_REG_REV_ID, rev_id, 1 );
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_clear_fifo( void )
{
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_clear_fifo_locked();
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_set_mode( unit_heart_mode_t mode )
{
  if( mode != UNIT_HEART_MODE_HEART_RATE && mode != UNIT_HEART_MODE_SPO2 )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  // Entering a mode does not clear the FIFO automatically (datasheet section
  // 4.2, rule 7); clear it first so sampling starts from an empty FIFO.
  esp_err_t err = _heart_clear_fifo_locked();
  if( err == ESP_OK )
  {
    err = _heart_write_reg_locked( UNIT_HEART_REG_MODE_CONFIG, (uint8_t)mode );
  }

  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_set_spo2_config( unit_heart_sample_rate_t sample_rate,
                                      unit_heart_pulse_width_t pulse_width,
                                      bool hi_res )
{
  if( sample_rate > UNIT_HEART_SR_1000HZ || pulse_width > UNIT_HEART_PW_1600US )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t cfg = (uint8_t)( ( sample_rate & 0x07 ) << 2 )
                | (uint8_t)( pulse_width & 0x03 );
  if( hi_res )
  {
    cfg |= UNIT_HEART_SPO2_HI_RES_EN;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_write_reg_locked( UNIT_HEART_REG_SPO2_CONFIG, cfg );
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_set_leds( unit_heart_led_current_t red_pa,
                               unit_heart_led_current_t ir_pa )
{
  if( red_pa > UNIT_HEART_LED_50_0MA || ir_pa > UNIT_HEART_LED_50_0MA )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t cfg = (uint8_t)( ( red_pa & 0x0F ) << 4 ) | (uint8_t)( ir_pa & 0x0F );

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_write_reg_locked( UNIT_HEART_REG_LED_CONFIG, cfg );
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_read_interrupt_status( uint8_t *status )
{
  if( status == NULL )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_read_reg_locked( UNIT_HEART_REG_INT_STATUS, status, 1 );
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_set_interrupt_enable( uint8_t mask )
{
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  // The low four bits are reserved and must stay zero (datasheet section 6.2).
  mask &= 0xF0;

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_write_reg_locked( UNIT_HEART_REG_INT_ENABLE, mask );
  xSemaphoreGive( _heart_mutex );
  return err;
}

// Computes the number of unread FIFO samples. Caller MUST hold _heart_mutex.
static esp_err_t _heart_available_locked( uint8_t *count )
{
  uint8_t wr_ptr = 0;
  uint8_t rd_ptr = 0;
  esp_err_t err = _heart_read_reg_locked( UNIT_HEART_REG_FIFO_WR_PTR, &wr_ptr, 1 );
  if( err != ESP_OK )
  {
    return err;
  }
  err = _heart_read_reg_locked( UNIT_HEART_REG_FIFO_RD_PTR, &rd_ptr, 1 );
  if( err != ESP_OK )
  {
    return err;
  }

  *count = (uint8_t)( ( wr_ptr - rd_ptr ) & UNIT_HEART_FIFO_PTR_MASK );
  return ESP_OK;
}

esp_err_t unit_heart_available( uint8_t *count )
{
  if( count == NULL )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = _heart_available_locked( count );
  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_read_fifo( unit_heart_sample_t *samples, size_t max,
                                size_t *count_out )
{
  if( count_out == NULL || ( samples == NULL && max > 0 ) )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  *count_out = 0;
  if( max == 0 )
  {
    return ESP_OK;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  uint8_t available = 0;
  esp_err_t err = _heart_available_locked( &available );
  if( err != ESP_OK )
  {
    xSemaphoreGive( _heart_mutex );
    return err;
  }

  size_t to_read = available;
  if( to_read > max )
  {
    to_read = max;
  }

  // FIFO_DATA (0x05) does not auto-increment; reading 4 bytes pops one sample
  // and advances the read pointer internally (datasheet section 4.2, rule 4).
  for( size_t i = 0; i < to_read; i++ )
  {
    uint8_t raw[UNIT_HEART_BYTES_PER_SAMPLE];
    err = _heart_read_reg_locked( UNIT_HEART_REG_FIFO_DATA, raw,
                                  UNIT_HEART_BYTES_PER_SAMPLE );
    if( err != ESP_OK )
    {
      break;
    }
    samples[i].ir  = (uint16_t)( ( raw[0] << 8 ) | raw[1] );
    samples[i].red = (uint16_t)( ( raw[2] << 8 ) | raw[3] );
    ( *count_out )++;
  }

  xSemaphoreGive( _heart_mutex );
  return err;
}

esp_err_t unit_heart_read_temperature( float *temp_c )
{
  if( temp_c == NULL )
  {
    return ESP_ERR_INVALID_ARG;
  }
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  // Set TEMP_EN while preserving the current MODE bits (datasheet section 10).
  uint8_t mode = 0;
  esp_err_t err = _heart_read_reg_locked( UNIT_HEART_REG_MODE_CONFIG, &mode, 1 );
  if( err != ESP_OK )
  {
    xSemaphoreGive( _heart_mutex );
    return err;
  }

  err = _heart_write_reg_locked( UNIT_HEART_REG_MODE_CONFIG,
                                 mode | UNIT_HEART_MODE_TEMP_EN );
  if( err != ESP_OK )
  {
    xSemaphoreGive( _heart_mutex );
    return err;
  }

  // Wait for the single-shot conversion to land. The TEMP_RDY status bit is not
  // reliably observable on all MAX30100 parts, so allow a fixed settle time
  // (datasheet section 10: "poll Interrupt Status for TEMP_RDY, or wait
  // ~29 ms") and then confirm TEMP_EN has self-cleared before reading the data
  // registers. The poll loop runs until either condition is met.
  bool ready = false;
  for( int attempt = 0; attempt < UNIT_HEART_POLL_RETRIES; attempt++ )
  {
    vTaskDelay( pdMS_TO_TICKS( UNIT_HEART_POLL_DELAY_MS ) );

    uint8_t int_status = 0;
    err = _heart_read_reg_locked( UNIT_HEART_REG_INT_STATUS, &int_status, 1 );
    if( err != ESP_OK )
    {
      xSemaphoreGive( _heart_mutex );
      return err;
    }

    uint8_t mode_now = 0;
    err = _heart_read_reg_locked( UNIT_HEART_REG_MODE_CONFIG, &mode_now, 1 );
    if( err != ESP_OK )
    {
      xSemaphoreGive( _heart_mutex );
      return err;
    }

    // Require both a minimum settle time (one conversion period) AND that the
    // self-clearing TEMP_EN bit has dropped, so the data registers are valid.
    if( ( int_status & UNIT_HEART_INT_TEMP_RDY )
        || ( attempt >= 3 && ( mode_now & UNIT_HEART_MODE_TEMP_EN ) == 0 ) )
    {
      ready = true;
      break;
    }
  }

  if( !ready )
  {
    ESP_LOGE( _TAG, "Temperature conversion did not complete" );
    xSemaphoreGive( _heart_mutex );
    return ESP_ERR_TIMEOUT;
  }

  uint8_t tint = 0;
  uint8_t tfrac = 0;
  err = _heart_read_reg_locked( UNIT_HEART_REG_TEMP_INT, &tint, 1 );
  if( err == ESP_OK )
  {
    err = _heart_read_reg_locked( UNIT_HEART_REG_TEMP_FRAC, &tfrac, 1 );
  }

  xSemaphoreGive( _heart_mutex );

  if( err != ESP_OK )
  {
    return err;
  }

  // TINT is two's complement integer degrees; TFRAC[3:0] adds 0.0625 deg/LSB
  // as a positive fraction (datasheet section 6.6).
  *temp_c = (float)( (int8_t)tint ) + (float)( tfrac & 0x0F ) * 0.0625f;
  return ESP_OK;
}

esp_err_t unit_heart_shutdown( bool enable )
{
  if( _heart_mutex == NULL )
  {
    return ESP_ERR_INVALID_STATE;
  }

  if( xSemaphoreTake( _heart_mutex,
                      pdMS_TO_TICKS( UNIT_HEART_MUTEX_TIMEOUT_MS ) ) != pdTRUE )
  {
    return ESP_ERR_TIMEOUT;
  }

  // Preserve the current MODE bits and only toggle SHDN.
  uint8_t mode = 0;
  esp_err_t err = _heart_read_reg_locked( UNIT_HEART_REG_MODE_CONFIG, &mode, 1 );
  if( err == ESP_OK )
  {
    if( enable )
    {
      mode |= UNIT_HEART_MODE_SHDN;
    }
    else
    {
      mode &= (uint8_t)~UNIT_HEART_MODE_SHDN;
    }
    err = _heart_write_reg_locked( UNIT_HEART_REG_MODE_CONFIG, mode );
  }

  xSemaphoreGive( _heart_mutex );
  return err;
}

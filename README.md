# M5Stack Unit Heart (U029) ESP-IDF Component

This is a component library for the M5Stack Unit Heart (SKU: U029) on the Core2 for AWS IoT Kit. It uses the abstractions built into the [BSP for the Core2 for AWS](https://github.com/m5stack/Core2-for-AWS-IoT-Kit/tree/BSP-dev).

The Unit Heart is a non-invasive blood-oxygen (SpO2) and heart-rate sensor built around the Maxim Integrated **MAX30100**. It connects to the Core2 external HY2.0-4P I2C port labeled **PORT.A** at the fixed 7-bit I2C address `0x57`. The device exposes a 16-deep, 4-byte-per-sample optical FIFO (IR + RED ADC counts) that the host reads to run heart-rate / SpO2 algorithms.

## Features

- One-call bring-up with sensible defaults — no register tuning required
- Device presence verification via Part ID (`0xFF` == `0x11`)
- Soft reset and FIFO-pointer clearing
- Heart-rate-only and SpO2 mode selection
- Configurable sample rate, LED pulse width / ADC resolution, and high-resolution mode
- Independent RED and IR LED drive-current selection
- Thread-safe FIFO draining that honors the non-incrementing `FIFO_DATA` register and 4-bit pointer wrap
- Interrupt status read and interrupt enable mask configuration
- Single-shot on-chip temperature read (for SpO2 compensation)
- Low-power shutdown control

All public functions are thread-safe; an internal mutex serializes access to the shared PORT.A I2C bus. `unit_heart_init()` also brings up the PORT.A I2C bus for you, so you do not need to initialize it separately.

## Quick start

`unit_heart_init()` verifies the sensor, resets it, and applies tuned defaults
(100 sps, 1600 µs pulse width, high-resolution, 11 mA RED / 24 mA IR). Just
start a mode and read — no manual configuration needed.

```c
#include "unit_heart.h"

void app_main( void )
{
    core2foraws_init();

    unit_heart_init();                           // probes, resets, applies defaults
    unit_heart_start( UNIT_HEART_DEFAULT_MODE ); // begin SpO2 sampling

    while ( true )
    {
        unit_heart_sample_t samples[UNIT_HEART_FIFO_DEPTH];
        size_t count = 0;

        unit_heart_read_fifo( samples, UNIT_HEART_FIFO_DEPTH, &count );
        for ( size_t i = 0; i < count; i++ )
        {
            printf( "IR=%u RED=%u\n", samples[i].ir, samples[i].red );
        }

        vTaskDelay( pdMS_TO_TICKS( 100 ) );
    }
}
```

The raw IR / RED counts are inputs to host-side heart-rate and SpO2 algorithms; the MAX30100 only delivers filtered optical samples.

## Heart-rate-only mode

Pass `UNIT_HEART_MODE_HEART_RATE` to use only the IR LED (lower power). The
`red` field of each sample is `0` in this mode.

```c
unit_heart_init();
unit_heart_start( UNIT_HEART_MODE_HEART_RATE );

unit_heart_sample_t samples[UNIT_HEART_FIFO_DEPTH];
size_t count = 0;
unit_heart_read_fifo( samples, UNIT_HEART_FIFO_DEPTH, &count );
// samples[i].ir holds the pulse waveform; samples[i].red == 0
```

## Reading die temperature

The on-chip temperature sensor (used for SpO2 compensation) is a single-shot
measurement. Read it while the device is in an active mode.

```c
float temp_c = 0.0f;
if ( unit_heart_read_temperature( &temp_c ) == ESP_OK )
{
    printf( "Die temperature: %.2f C\n", temp_c );
}
```

## Low power

```c
unit_heart_shutdown( true );   // enter power-save (config retained)
// ...
unit_heart_shutdown( false );  // resume sampling
```

## Overriding the defaults (advanced)

The defaults suit most fingertip measurements. If you need to tune the sensor,
call the granular setters after `unit_heart_init()` and use `unit_heart_set_mode()`
instead of `unit_heart_start()` so your settings are not overwritten.

```c
unit_heart_init();

// Custom acquisition: 200 sps, 800 us pulse width, no hi-res.
unit_heart_set_spo2_config( UNIT_HEART_SR_200HZ, UNIT_HEART_PW_800US, false );
unit_heart_set_leds( UNIT_HEART_LED_7_6MA, UNIT_HEART_LED_27_1MA );
unit_heart_set_mode( UNIT_HEART_MODE_SPO2 );   // start without re-applying defaults
```

The default values are exposed as `UNIT_HEART_DEFAULT_*` macros in
[`unit_heart.h`](include/unit_heart.h) if you want to reference or partially
override them.

## Using a PA Hub (I2C multiplexer)

If the Unit Heart is connected through an M5Stack PA Hub (TCA9548A) instead of
directly to PORT.A, enable PA Hub routing in `menuconfig` under
**Unit Heart Configuration**:

- **Use PA Hub for I2C communication** (`CONFIG_UNIT_HEART_USE_PAHUB`)
- **PA Hub channel number** (`CONFIG_UNIT_HEART_PAHUB_CHANNEL`, 0-5) — the hub
  channel the sensor is plugged in to

When enabled, `unit_heart_init()` automatically brings up the PA Hub and selects
the configured channel; every I2C transfer is then routed through it. No code
changes are required — the same API (`unit_heart_init`, `unit_heart_start`,
`unit_heart_read_fifo`, …) works in both direct and PA Hub modes.

This option requires the [`core2foraws-unit-pahub`](../core2foraws-unit-pahub)
component to be present in the project.


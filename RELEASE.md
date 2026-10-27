# Changelog for Code Example for the on-chip feature motion detection of XENSIV™ BGT60CUTRxx Radar Sensor

## Overview

This code example demonstrates Infineon's radar capabilities by implementing the on-chip feature for motion detection on the CSK hardware platform

### 1.1.7

#### Add
- `RD_MAX_RANGE_M`, `RD_MAX_VELOCITY_MPS`, `RD_RX_ANTENNAS_MASK` and their `ULP_` counterparts in `presence_detection_localization.h` and its Japan variant, so the instrumental range, the unambiguous velocity and the active receive antennas are available to the application without recomputing them

#### Change
- Radar configurations regenerated with `ifx_smartar_hal` v2.4.7, which corrects the Doppler velocity grid and the burst repetition times of the range-Doppler and ultra-low-power sequences

#### Remove
- The motion detection radar configurations, which this example does not use. Only `presence_detection_localization` and its Japan variant are shipped

#### Fixed
- ADC calibration no longer leaves `SEQ3_DONE` latched in `IR_STAT`; the sensor driver clears it through `IR_CLEAR` once calibration has completed

### 1.1.4

This code example is available in the staging repository for customer use.

### 1.1.0

#### Add
- Renamed from `motion-detection-power-on-off-csk` to `clutter-localization-and-clustering-power-on-off-csk`
- Clutter localization and DBSCAN clustering added to the power-cycled processing pipeline
- JSON protocol handler for power on/off application (`protocol_handler` processes `get_info` and power commands from host)
- `USE_CASE_NAME` macro used instead of hardcoded application name string

#### Change
- Updated radar configuration files for clutter and cluster power-on/off use case
- Improved power cycling configuration: power-on time set to 20 ms, switching period to 250 ms
- Reset delay reduced from 5 ms to 1 ms

### 1.0.0

This is the initial release of the code example to demonstrate Infineon's radar capabilities by implementing the on-chip feature for motion detection on the CSK hardware platform

Further it enables power optimization features for the BGT60CUTRxx radar sensor to reduce power consumption during operation.

#### Add
- Initializes the board, peripherals, and debug UART.
- Configures GPIOs and the SPI interface for radar sensor communication.
- Initializes the radar sensor and sets up the interrupt handler for motion detection events.
- Sets up the motion detection configuration.
- Streams motion detection events over UART for monitoring.
- Config files for power optimization
- Enabling power down mode for BGT60CUTRxx radar sensor


#### Change

#### Fixed

#### Improved

## Supported software and tools

This version of the XENSIV™ BGT60CUTRxx 60GHz radar sensor code example was validated for compatibility with the following software and tools:

| Software and tools                        | Version |
| :---                                      | :----:  |
| ModusToolbox™ software environment        | 3.4.0   |
| GCC compiler                              | 10.3.1  |

Minimum required ModusToolbox&trade; software environment: v3.4


© Infineon Technologies AG, 2026.

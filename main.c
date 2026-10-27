/*
 * main.c
 *
 * Description: Demonstrates Motion Detection with Clutter Localization and DBSCAN clustering.
 *
 * This code example demonstrates how to use the motion detection algorithm in a power-cycled application, where the
 * radar sensor is turned on and off periodically.
 * The radar sensor is powered on for 500ms, during which motion detection is performed continuously.
 * After 500ms, the sensor is powered off for 1500ms to save power, during which no motion detection is performed.
 * This cycle repeats indefinitely.

 * Motion Detection detects moving targets in the radar's field of view.
 * Clutter Localization identifies static clutter and excludes it from motion detection.
 * DBSCAN clustering groups detected motion targets into clusters based on their spatial proximity.
 *
 * Related Document: See README.md
 *
 ********************************************************************************
 * Copyright (c) (2016-2022), Cypress Semiconductor Corporation (an Infineon company) or
 * an affiliate of Cypress Semiconductor Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
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
 *******************************************************************************/

#include "cyhal.h"
#include "cy_retarget_io.h"
#include <stdbool.h>
#include <inttypes.h>
#include "common.h"
#include "protocol.h"
#include "radar_motion_detection.h"
#include "radar_clutter_localization.h"
#include "radar_clustering.h"
#include "resource_map.h"
#include "radar_config/presence_detection_localization.h"
#include "xensiv_bgt60cutrxx.h"
#include "xensiv_bgt60cutrxx_mtb.h"
#include "xensiv_bgt60cutrxx_regs.h"
#include "version.h"

/*******************************************************************************
 * Macros
 ********************************************************************************/

/** baud rate for the UART communication
 * The baudrate must be high enough to transmit the data before the next interrupt occurs.
 */
#define UART_BAUDRATE 2000000

/** Default SPI speed for the XENSIV BGT60CUTRXX sensor, which is used during startup */
#define SPI_FREQUENCY               (18750000UL) // 18.75MHz

/** Duration of POWER_ON state within one cycle */
// 25-50 ms for typical cases for medium power and 5-10 ms for low power cases
#define POWER_ON_TIME_MS            (9000U)

/** Full POWER_ON + POWER_OFF cycle period */
// 250 ms
#define POWER_PERIOD_MS             (10000U)

/** Duration of POWER_OFF state within one cycle */
#define POWER_OFF_TIME_MS           (POWER_PERIOD_MS - POWER_ON_TIME_MS)

/** Main loop tick used for software timing */
#define TIMER_POWER_ON_OFF_TICK_MS         (10U)

/** Hardware timer frequency used for state machine tick generation */
#define TIMER_POWER_ON_OFF_FREQUENCY_HZ    (10000U)

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static void xensiv_bgt60cutrxx_irq_handler(void* callback_arg, cyhal_gpio_event_t event);
static int32_t xensiv_bgt60cutrxx_board_init(void);
static int32_t initialize_board_leds(void);
static void turn_radar_power(bool power_on);
static int32_t timer_power_on_off_init(void);
static void timer_power_on_off_irq_handler(void* callback_arg, cyhal_timer_event_t event);

/*******************************************************************************
* Global Variables
*******************************************************************************/

/** Radar sensor object */
static xensiv_bgt60cutrxx_mtb_t sensor;

/** Motion Detection object */
static motion_detection_t motion_detection;

/** Clutter Localization object */
#if MD_RD_ENABLED
static clutter_localization_t clutter_localization;
#endif

/** Clustering object */
#if MD_ULP_ENABLED
static clustering_t clustering;
#endif

/** Buffer to store the full Range Doppler Map */
#ifdef RADAR_MOTION_DETECTION_USE_RD_BUFFER
static uint32_t rd_data_buffer[BGT60CUTR13_RD_DATABUFFER_MAX_SIZE];
#else
static uint32_t rd_data_buffer[0];
#endif

/** Helper variable used by IRQ handler to indicate data events to main loop */
static volatile bool radar_event_irq = false;

/** Hardware timer object and tick counter for state machine timing */
static cyhal_timer_t timer_power_on_off_obj;
static volatile uint32_t timer_power_on_off_ticks = 0;

/** Platform interface configuration */
static xensiv_bgt60cutrxx_mtb_iface_config_t mcu_iface_config =
{
    .selpin       = PIN_XENSIV_BGT60CUTRXX_SPI_CSN,
    .rstpin       = PIN_XENSIV_BGT60CUTRXX_RSTN,
    .irqpin       = PIN_XENSIV_BGT60CUTRXX_IRQ,
    .irq_priority = CYHAL_ISR_PRIORITY_DEFAULT,
    .spi_mosi     = CYBSP_SPI_MOSI,
    .spi_miso     = CYBSP_SPI_MISO,
    .spi_clk      = CYBSP_SPI_CLK,
    .spi_speed_hz = SPI_FREQUENCY,
};

/*******************************************************************************
* Function Definitions
*******************************************************************************/

/**
 * @brief Interrupt handler for the XENSIV BGT60CUTRXX radar sensor.
 *
 * This function is called when a radar interrupt occurs. It sets the radar_event_irq
 * flag to signal the main loop that radar data is ready for processing.
 *
 * @param[in] callback_arg User-defined callback argument (unused).
 * @param[in] event GPIO event that triggered the interrupt.
 */
static void xensiv_bgt60cutrxx_irq_handler(void* callback_arg, cyhal_gpio_event_t event)
{
    (void)callback_arg;
    (void)event;

    // flag to indicate radar event
    radar_event_irq = true;
}


/** Helper function to initialize board specific settings */
static int32_t xensiv_bgt60cutrxx_board_init(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    /* Enable LDO */
    result |= cyhal_gpio_init(PIN_XENSIV_BGT60CUTRXX_LDO_EN, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, 1);

    /* Set specific pins to disable BGT60CUTRXX bootloader */
    result |= cyhal_gpio_init(PIN_XENSIV_BGT60CUTRXX_DIO2, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, 1);
    result |= cyhal_gpio_init(PIN_XENSIV_BGT60CUTRXX_DIO3, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, 1);

    /* Set level shifter direction */
    result |= cyhal_gpio_init(SHIELD_XENSIV_DIR4_EN, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, 1);

    if (result != CY_RSLT_SUCCESS)
    {
        return -1;
    }

    return XENSIV_BGT60CUTRXX_STATUS_OK;
}


/** Helper function to initialize board LEDs */
static int32_t initialize_board_leds(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;

    result |= cyhal_gpio_init(CYBSP_USER_LED, CYHAL_GPIO_DIR_OUTPUT,
                              CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);
    result |= cyhal_gpio_init(SHIELD_XENSIV_RADAR_LED_RED, CYHAL_GPIO_DIR_OUTPUT,
                              CYHAL_GPIO_DRIVE_STRONG, SHIELD_XENSIV_RADAR_LED_RED_OFF);
    result |= cyhal_gpio_init(SHIELD_XENSIV_RADAR_LED_GREEN, CYHAL_GPIO_DIR_OUTPUT,
                              CYHAL_GPIO_DRIVE_STRONG, SHIELD_XENSIV_RADAR_LED_GREEN_OFF);

    if (result != CY_RSLT_SUCCESS)
    {
        return -1;
    }

    return XENSIV_BGT60CUTRXX_STATUS_OK;
}


/** Controls radar sensor power supply and clears software event flag when powering off */
static void turn_radar_power(bool power_on)
{
    cyhal_gpio_write(PIN_XENSIV_BGT60CUTRXX_LDO_EN, power_on ? 1 : 0);
    if (!power_on)
    {
        radar_event_irq = false;
    }
}


/** Hardware timer callback used as time base for the power state machine */
static void timer_power_on_off_irq_handler(void* callback_arg, cyhal_timer_event_t event)
{
    (void)callback_arg;
    (void)event;
    timer_power_on_off_ticks++;
}


/** Initializes and starts the hardware timer used by power state machine */
static int32_t timer_power_on_off_init(void)
{
    cy_rslt_t result;
    // Calculate timer period in ticks based on defined tick duration and frequency, -1 because timer counts from 0
    const uint32_t timer_period_ticks = ((TIMER_POWER_ON_OFF_TICK_MS * TIMER_POWER_ON_OFF_FREQUENCY_HZ) / 1000U)-1U;

    const cyhal_timer_cfg_t timer_cfg =
    {
        .compare_value = 0,
        .period        = timer_period_ticks,
        .direction     = CYHAL_TIMER_DIR_UP,
        .is_compare    = false,
        .is_continuous = true,
        .value         = 0
    };

    result = cyhal_timer_init(&timer_power_on_off_obj, NC, NULL);
    if (result != CY_RSLT_SUCCESS)
    {
        return -1;
    }

    result = cyhal_timer_configure(&timer_power_on_off_obj, &timer_cfg);
    if (result != CY_RSLT_SUCCESS)
    {
        return -1;
    }

    result = cyhal_timer_set_frequency(&timer_power_on_off_obj, TIMER_POWER_ON_OFF_FREQUENCY_HZ);
    if (result != CY_RSLT_SUCCESS)
    {
        return -1;
    }

    cyhal_timer_register_callback(&timer_power_on_off_obj, &timer_power_on_off_irq_handler, NULL);
    cyhal_timer_enable_event(&timer_power_on_off_obj,
                             CYHAL_TIMER_IRQ_TERMINAL_COUNT,
                             CYHAL_ISR_PRIORITY_DEFAULT,
                             true);

    return XENSIV_BGT60CUTRXX_STATUS_OK;
}


/**
 * @brief Main application entry point for radar motion detection code example.
 *
 * This function performs the following operations:
 * - Initializes the device, board peripherals, and UART for debug output
 * - Configures GPIO pins and SPI interface for the XENSIV BGT60CUTRXX radar sensor
 * - Sets up the radar sensor with interrupt-driven data acquisition
 * - Initializes motion detection with range-doppler and ultra-low-power sequences
 * - Configures the clutter localization module to identify and mask static objects
 * - Configures the DBSCAN clustering algorithm for target grouping
 * - Enters an infinite loop processing radar interrupts, detecting motion, identifying clutter,
 *   clustering targets, and printing detection results, clutter zones, and clusters
 *
 * @return int This function never returns under normal operation due to infinite loop.
 */
int main(void)
{
    typedef enum
    {
        POWER_ON = 0,
        POWER_OFF
    } power_state_t;
    power_state_t power_state = POWER_ON;
    uint32_t power_state_elapsed_ms = 0;


    #if defined (CY_DEVICE_SECURE)
    cyhal_wdt_t wdt_obj;

    /* Clear watchdog timer so that it doesn't trigger a reset */
    result = cyhal_wdt_init(&wdt_obj, cyhal_wdt_get_max_timeout_ms());
    CY_ASSERT(CY_RSLT_SUCCESS == result);
    cyhal_wdt_free(&wdt_obj);
    #endif /* #if defined (CY_DEVICE_SECURE) */

    // Initialize MCU and board peripherals
    cy_rslt_t cy_result;
    cy_result = cybsp_init();
    cy_result |= initialize_board_leds();
    if (cy_result != CY_RSLT_SUCCESS)
    {
        // MCU and board init failed. Stop program execution
        CY_ASSERT(0);
        return 1;
    }

    // Enable global interrupts
    __enable_irq();

    // Initialize UART port used for logging
    cy_result = cy_retarget_io_init_fc(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX, NC, NC, UART_BAUDRATE);
    if (cy_result != CY_RSLT_SUCCESS)
    {
        // retarget-io init failed. Stop program execution
        CY_ASSERT(0);
        return 1;
    }

    // wait until UART is ready
    cyhal_system_delay_ms(5000);

    protocol_print_info("******************");
    protocol_print_info("RADAR: Clutter Localization and Clustering Example (Power On/Off)");
    protocol_print_info("******************");
    protocol_print_info("For more projects, visit our code examples repositories:");
    protocol_print_info("https://github.com/Infineon/Code-Examples-for-ModusToolbox-Software");

    // init timer for turning on and off the power of the sensor
    cy_result = timer_power_on_off_init();
    if (XENSIV_BGT60CUTRXX_STATUS_OK != cy_result)
    {
        protocol_print_error("Error initializing state timer");
        CY_ASSERT(0);
        return 1;
    }

    int32_t result;
    result = xensiv_bgt60cutrxx_board_init();
    result |= xensiv_bgt60cutrxx_platform_open(&sensor.iface, &mcu_iface_config);
    if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
    {
        protocol_print_error("Error initializing platform interfaces");
        CY_ASSERT(0);
        return 1;
    }

    // Radar sensor initialization
    xensiv_bgt60cutrxx_power_mode_t power_mode = DEEP_SLEEP_MODE;
    bool deep_sleep_after_calibration = false;
    result = xensiv_bgt60cutrxx_platform_init(&sensor, &mcu_iface_config, register_list, ARRAY_SIZE(
                                                  register_list), power_mode, deep_sleep_after_calibration);
    if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
    {
        protocol_print_error("Error initializing radar sensor");
        CY_ASSERT(0);
        return 1;
    }
    protocol_print_info("Radar sensor is initialized");

    result = xensiv_bgt60cutrxx_platform_irq_init(&sensor, &mcu_iface_config, &xensiv_bgt60cutrxx_irq_handler, NULL);
    if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
    {
        protocol_print_error("Error initializing radar IRQ handler");
        CY_ASSERT(0);
        return 1;
    }
    protocol_print_info("Radar IRQ handler for motion detection events is enabled");

    protocol_set_app_info(USE_CASE_NAME, VERSION, VERSION_BUILD);
    protocol_set_sensor_info("BGT60CUTR13B", VERSION, false,
                             sensor.dev.clk_freq_xtal == BGT60CUTRXX_CLK_80MHZ ? 80.0f : 40.0f);

    #if MD_RD_ENABLED
    // Radar Motion Detection initialization
    const float rd_velocity_grid[] = RD_VELOCITY_GRID_MPS;
    const sequence_settings_t rd_sequence_settings =
    {
        .enabled            = true,
        .velocity_grid_mps  = rd_velocity_grid,
        .range_resolution_m = RD_RANGE_RESOLUTION_M
    };
    #else
    const sequence_settings_t rd_sequence_settings = {};
    #endif

    #if MD_ULP_ENABLED
    const float ulp_velocity_grid[] = ULP_VELOCITY_GRID_MPS;
    const sequence_settings_t ulp_sequence_settings =
    {
        .enabled            = true,
        .velocity_grid_mps  = ulp_velocity_grid,
        .range_resolution_m = ULP_RANGE_RESOLUTION_M
    };
    #else
    const sequence_settings_t ulp_sequence_settings = {};
    #endif

    result = radar_motion_detection_init(&motion_detection,
                                         &sensor.dev,
                                         rd_data_buffer,
                                         &rd_sequence_settings,
                                         &ulp_sequence_settings
                                         );
    if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
    {
        protocol_print_error("Radar Motion Detection initialization failed");
        CY_ASSERT(0);
        return 1;
    }
    protocol_print_info("Radar Motion Detection is initialized");

    // Clutter Localization initialization
    #if MD_RD_ENABLED
    static clutter_localization_config_t clutter_localization_config =
    {
        // expectation: if any range zone masking required by default, this is expected to configure below. Priority is
        // given to the values set below not from register list
        /*.default_exclusion_zones =
           {
            .MD_ZONE_EXCL0 = 0x00000000,
            .MD_ZONE_EXCL1 = 0x00000000,
            .MD_ZONE_EXCL2 = 0x00000000,
            .MD_ZONE_EXCL3 = 0x00000000,
            .MD_ZONE_CTRL  = 0x00000001
           },*/
        .doppler_gating_speed_mps             = 0.25,
        .increment_count                      = 2,
        .decrement_count                      = 1,
        .saturation_limit                     = 100,
        .settings_threshold                   = 10,
        .priority_h_plane                     = false, // based on default_exclusion_zones configured above this
                                                       // attribute is overwritten in the clutter_localization_init()
        .num_neighboring_zones                = 1,
        .period_ticks                         = 150,
        .max_duration_ticks                   = 100,
        .auto_masking_active                  = true,
        .deactivation_counter_threshold_ticks = 500,
        .range_max                            = RD_RANGE_RESOLUTION_M * RD_SAMPLES_PER_CHIRP / 2.0f
    };

    result = clutter_localization_init(&motion_detection, &clutter_localization, &clutter_localization_config);
    if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
    {
        protocol_print_error("Clutter localization init failed");
        CY_ASSERT(0);
        return 1;
    }
    protocol_print_info("Radar Clutter Localization is initialized");
    protocol_set_zone_config(get_zone_config(RD_RANGE_RESOLUTION_M, RD_SAMPLES_PER_CHIRP, 16));
    protocol_set_clutter_info(&clutter_localization);
    #endif

    // Clustering initialization
    #if MD_ULP_ENABLED
    clustering_settings_t clustering_config =
    {
        .max_rows                        = 25,
        .min_range_m                     = 0.5f,
        .static_check_frame_count        = 5,
        .cluster_clear_counter_threshold = 100,
        .dbscan_eps_m                    = 1.0f,
        .dbscan_min_samples              = 5
    };

    result = clustering_init(&clustering, &clustering_config);
    if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
    {
        protocol_print_error("Clustering init failed");
        CY_ASSERT(0);
        return 1;
    }
    protocol_print_info("Radar Clustering is initialized");
    #endif


    radar_event_irq = false;
    uint32_t irq_status = 0;
    xensiv_bgt60cutrxx_get_irq_status(motion_detection.radar_sensor, &irq_status);
    xensiv_bgt60cutrxx_set_irq_clear(motion_detection.radar_sensor, BGT60CUTR13B_IR_CLEAR_FOR_STARTUP_MSK);

    result = cyhal_timer_start(&timer_power_on_off_obj);
    if (result != CY_RSLT_SUCCESS)
    {
        protocol_print_error("Error starting state timer");
        CY_ASSERT(0);
        return 1;
    }

    while (true)
    {
        static uint32_t counter = 0;

        protocol_handler(counter);

        if (power_state == POWER_ON)
        {
            if (radar_event_irq)
            {
                //clear internal interrupt flag
                radar_event_irq = false;

                // Process radar motion detection
                result = radar_motion_detection_process(&motion_detection);
                if (result != XENSIV_BGT60CUTRXX_STATUS_OK)
                {
                    protocol_print_error("Motion Detection process error: %" PRId32, result);
                    CY_ASSERT(0);
                    return 1;
                }

                // Process clutter localization (RD sequence only)
                #if MD_RD_ENABLED
                result = clutter_localization_process(&motion_detection, &clutter_localization);
                if (result != XENSIV_BGT60CUTRXX_STATUS_OK)
                {
                    protocol_print_error("Clutter localization process error: %" PRId32, result);
                    CY_ASSERT(0);
                    return 1;
                }
                #endif

                // Process clustering (ULP sequence only)
                #if MD_ULP_ENABLED
                result = clustering_process(&motion_detection, &clustering);
                if (result < 0)
                {
                    protocol_print_error("Clustering process error: %" PRId32, result);
                    CY_ASSERT(0);
                    return 1;
                }
                #endif

                // Print the detections
                #if MD_ULP_ENABLED
                if (motion_detection.seq2_done ||
                    (motion_detection.seq1_done && clustering.ulp_seq_done_flag))
                {
                    print_detections(&motion_detection, counter, true);
                }
                #else
                print_detections(&motion_detection, counter, true);
                #endif

                // Print the clutter zones, if they have been updated
                #if MD_RD_ENABLED
                if (!motion_detection.seq1_done)
                {
                    print_clutter_zones(&clutter_localization, counter);
                }
                #endif

                // Print clustering results
                #if MD_ULP_ENABLED
                if (motion_detection.seq1_done && clustering.ulp_seq_done_flag)
                {
                    print_clustering_results(&clustering, counter, true);
                }
                #endif

                counter++;
            }

            if (power_state_elapsed_ms >= POWER_ON_TIME_MS)
            {
                // turn off the power of the radar sensor
                turn_radar_power(false);
                power_state = POWER_OFF;
                power_state_elapsed_ms = 0;
            }
        }
        else if (power_state == POWER_OFF)
        {
            if (power_state_elapsed_ms >= POWER_OFF_TIME_MS)
            {
                // turn on the power of the radar sensor
                turn_radar_power(true);
                // Radar sensor initialization
                result = xensiv_bgt60cutrxx_platform_init(&sensor, &mcu_iface_config, register_list, ARRAY_SIZE(
                                                              register_list), power_mode, deep_sleep_after_calibration);
                if (XENSIV_BGT60CUTRXX_STATUS_OK != result)
                {
                    protocol_print_error("Error re-initializing radar sensor");
                    CY_ASSERT(0);
                    return 1;
                }
                // Re-apply learned exclusion zones to sensor registers after power-on
                #if MD_RD_ENABLED
                clutter_localization_write_exclusion_zones(&motion_detection, &clutter_localization);
                #endif
                power_state = POWER_ON;
                power_state_elapsed_ms = 0;
            }
        }
        else
        {
            CY_ASSERT(0);
            power_state = POWER_ON;
            power_state_elapsed_ms = 0;
        }

        /*
         * The timer ISR may run multiple times while one main-loop iteration is executing.
         * Drain all pending ticks so elapsed time stays accurate and no timer events are lost.
         */
        while (timer_power_on_off_ticks > 0U)
        {
            timer_power_on_off_ticks--;
            power_state_elapsed_ms += TIMER_POWER_ON_OFF_TICK_MS;
        }
    }
}


/* [] END OF FILE */

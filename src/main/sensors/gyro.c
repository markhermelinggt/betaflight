/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "platform.h"

#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "config/feature.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"
#include "pg/gyrodev.h"

#include "drivers/accgyro/accgyro.h"
#include "drivers/accgyro/accgyro_fake.h"
#include "drivers/accgyro/accgyro_mpu.h"
#include "drivers/accgyro/accgyro_mpu3050.h"
#include "drivers/accgyro/accgyro_mpu6050.h"
#include "drivers/accgyro/accgyro_mpu6500.h"
#include "drivers/accgyro/accgyro_spi_bmi160.h"
#include "drivers/accgyro/accgyro_spi_icm20649.h"
#include "drivers/accgyro/accgyro_spi_icm20689.h"
#include "drivers/accgyro/accgyro_spi_mpu6000.h"
#include "drivers/accgyro/accgyro_spi_mpu6500.h"
#include "drivers/accgyro/accgyro_spi_mpu9250.h"

#ifdef USE_GYRO_L3GD20
#include "drivers/accgyro/accgyro_spi_l3gd20.h"
#endif

#ifdef USE_GYRO_L3G4200D
#include "drivers/accgyro_legacy/accgyro_l3g4200d.h"
#endif

#include "drivers/accgyro/gyro_sync.h"
#include "drivers/bus_spi.h"
#include "drivers/io.h"

#include "config/config.h"
#include "fc/runtime_config.h"

#ifdef USE_GYRO_DATA_ANALYSE
#include "flight/gyroanalyse.h"
#endif
#include "flight/rpm_filter.h"

#include "io/beeper.h"
#include "io/statusindicator.h"

#include "scheduler/scheduler.h"

#include "sensors/boardalignment.h"
#include "sensors/gyro.h"
#include "sensors/sensors.h"

#if ((FLASH_SIZE > 128) && (defined(USE_GYRO_SPI_ICM20601) || defined(USE_GYRO_SPI_ICM20689) || defined(USE_GYRO_SPI_MPU6500)))
#define USE_GYRO_SLEW_LIMITER
#endif

FAST_RAM_ZERO_INIT gyro_t gyro;
static FAST_RAM_ZERO_INIT uint8_t gyroDebugMode;

static FAST_RAM_ZERO_INIT uint8_t gyroToUse;
static FAST_RAM_ZERO_INIT bool overflowDetected;
#ifdef USE_GYRO_OVERFLOW_CHECK
static FAST_RAM_ZERO_INIT timeUs_t overflowTimeUs;
#endif

#ifdef USE_GYRO_OVERFLOW_CHECK
static FAST_RAM_ZERO_INIT uint8_t overflowAxisMask;
#endif

#ifdef USE_YAW_SPIN_RECOVERY
static FAST_RAM_ZERO_INIT bool yawSpinDetected;
static FAST_RAM_ZERO_INIT timeUs_t yawSpinTimeUs;
#endif

static FAST_RAM_ZERO_INIT float accumulatedMeasurements[XYZ_AXIS_COUNT];
static FAST_RAM_ZERO_INIT float gyroPrevious[XYZ_AXIS_COUNT];
static FAST_RAM_ZERO_INIT int accumulatedMeasurementCount;

static FAST_RAM_ZERO_INIT int16_t gyroSensorTemperature;

static bool gyroHasOverflowProtection = true;

static FAST_RAM_ZERO_INIT bool useDualGyroDebugging;
static FAST_RAM_ZERO_INIT flight_dynamics_index_t gyroDebugAxis;

typedef struct gyroCalibration_s {
    float sum[XYZ_AXIS_COUNT];
    stdev_t var[XYZ_AXIS_COUNT];
    int32_t cyclesRemaining;
} gyroCalibration_t;

static bool firstArmingCalibrationWasStarted = false;

typedef struct gyroSensor_s {
    gyroDev_t gyroDev;
    gyroCalibration_t calibration;
} gyroSensor_t;

STATIC_UNIT_TESTED FAST_RAM_ZERO_INIT gyroSensor_t gyroSensor1;
#ifdef USE_MULTI_GYRO
STATIC_UNIT_TESTED FAST_RAM_ZERO_INIT gyroSensor_t gyroSensor2;
#endif

static gyroDetectionFlags_t gyroDetectionFlags = NO_GYROS_DETECTED;

#ifdef UNIT_TEST
STATIC_UNIT_TESTED gyroSensor_t * const gyroSensorPtr = &gyroSensor1;
STATIC_UNIT_TESTED gyroDev_t * const gyroDevPtr = &gyroSensor1.gyroDev;
#endif

static void gyroInitSensorFilters(gyroSensor_t *gyroSensor);
static void gyroInitLowpassFilterLpf(int slot, int type, uint16_t lpfHz);

#define DEBUG_GYRO_CALIBRATION 3

#ifdef STM32F10X
#define GYRO_SYNC_DENOM_DEFAULT 8
#elif defined(USE_GYRO_SPI_MPU6000) || defined(USE_GYRO_SPI_MPU6500) || defined(USE_GYRO_SPI_ICM20601) || defined(USE_GYRO_SPI_ICM20649) \
   || defined(USE_GYRO_SPI_ICM20689)
#define GYRO_SYNC_DENOM_DEFAULT 1
#else
#define GYRO_SYNC_DENOM_DEFAULT 3
#endif

#define GYRO_OVERFLOW_TRIGGER_THRESHOLD 31980  // 97.5% full scale (1950dps for 2000dps gyro)
#define GYRO_OVERFLOW_RESET_THRESHOLD 30340    // 92.5% full scale (1850dps for 2000dps gyro)

PG_REGISTER_WITH_RESET_FN(gyroConfig_t, gyroConfig, PG_GYRO_CONFIG, 7);

#ifndef GYRO_CONFIG_USE_GYRO_DEFAULT
#define GYRO_CONFIG_USE_GYRO_DEFAULT GYRO_CONFIG_USE_GYRO_1
#endif

#ifdef USE_GYRO_DATA_ANALYSE
#define DYNAMIC_NOTCH_DEFAULT_CENTER_HZ 350
#define DYNAMIC_NOTCH_DEFAULT_CUTOFF_HZ 300
#endif

void pgResetFn_gyroConfig(gyroConfig_t *gyroConfig)
{ 
    gyroConfig->gyroCalibrationDuration = 125;        // 1.25 seconds
    gyroConfig->gyroMovementCalibrationThreshold = 48;
    gyroConfig->gyro_sync_denom = GYRO_SYNC_DENOM_DEFAULT;
    gyroConfig->gyro_hardware_lpf = GYRO_HARDWARE_LPF_NORMAL;
    gyroConfig->gyro_lowpass_type = FILTER_PT1;
    gyroConfig->gyro_lowpass_hz = 200;  // NOTE: dynamic lpf is enabled by default so this setting is actually
                                        // overridden and the static lowpass 1 is disabled. We can't set this
                                        // value to 0 otherwise Configurator versions 10.4 and earlier will also
                                        // reset the lowpass filter type to PT1 overriding the desired BIQUAD setting.
    gyroConfig->gyro_lowpass2_type = FILTER_PT1;
    gyroConfig->gyro_lowpass2_hz = 250;
    gyroConfig->gyro_high_fsr = false;
    gyroConfig->gyro_to_use = GYRO_CONFIG_USE_GYRO_DEFAULT;
    gyroConfig->gyro_soft_notch_hz_1 = 0;
    gyroConfig->gyro_soft_notch_cutoff_1 = 0;
    gyroConfig->gyro_soft_notch_hz_2 = 0;
    gyroConfig->gyro_soft_notch_cutoff_2 = 0;
    gyroConfig->checkOverflow = GYRO_OVERFLOW_CHECK_ALL_AXES;
    gyroConfig->gyro_offset_yaw = 0;
    gyroConfig->yaw_spin_recovery = true;
    gyroConfig->yaw_spin_threshold = 1950;
    gyroConfig->dyn_lpf_gyro_min_hz = 200;
    gyroConfig->dyn_lpf_gyro_max_hz = 500;
    gyroConfig->dyn_notch_range = DYN_NOTCH_RANGE_MEDIUM;
    gyroConfig->dyn_notch_width_percent = 8;
    gyroConfig->dyn_notch_q = 120;
    gyroConfig->dyn_notch_min_hz = 150;
    gyroConfig->gyro_filter_debug_axis = FD_ROLL;
}

#ifdef USE_MULTI_GYRO
#define ACTIVE_GYRO ((gyroToUse == GYRO_CONFIG_USE_GYRO_2) ? &gyroSensor2 : &gyroSensor1)
#else
#define ACTIVE_GYRO (&gyroSensor1)
#endif

const busDevice_t *gyroSensorBus(void)
{
    return &ACTIVE_GYRO->gyroDev.bus;
}

#ifdef USE_GYRO_REGISTER_DUMP
const busDevice_t *gyroSensorBusByDevice(uint8_t whichSensor)
{
#ifdef USE_MULTI_GYRO
    if (whichSensor == GYRO_CONFIG_USE_GYRO_2) {
        return &gyroSensor2.gyroDev.bus;
    }
#else
    UNUSED(whichSensor);
#endif
    return &gyroSensor1.gyroDev.bus;
}
#endif // USE_GYRO_REGISTER_DUMP

const mpuDetectionResult_t *gyroMpuDetectionResult(void)
{
    return &ACTIVE_GYRO->gyroDev.mpuDetectionResult;
}

STATIC_UNIT_TESTED gyroHardware_e gyroDetect(gyroDev_t *dev)
{
    gyroHardware_e gyroHardware = GYRO_DEFAULT;

    switch (gyroHardware) {
    case GYRO_DEFAULT:
        FALLTHROUGH;

#ifdef USE_GYRO_MPU6050
    case GYRO_MPU6050:
        if (mpu6050GyroDetect(dev)) {
            gyroHardware = GYRO_MPU6050;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_L3G4200D
    case GYRO_L3G4200D:
        if (l3g4200dDetect(dev)) {
            gyroHardware = GYRO_L3G4200D;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_MPU3050
    case GYRO_MPU3050:
        if (mpu3050Detect(dev)) {
            gyroHardware = GYRO_MPU3050;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_L3GD20
    case GYRO_L3GD20:
        if (l3gd20GyroDetect(dev)) {
            gyroHardware = GYRO_L3GD20;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_SPI_MPU6000
    case GYRO_MPU6000:
        if (mpu6000SpiGyroDetect(dev)) {
            gyroHardware = GYRO_MPU6000;
            break;
        }
        FALLTHROUGH;
#endif

#if defined(USE_GYRO_MPU6500) || defined(USE_GYRO_SPI_MPU6500)
    case GYRO_MPU6500:
    case GYRO_ICM20601:
    case GYRO_ICM20602:
    case GYRO_ICM20608G:
#ifdef USE_GYRO_SPI_MPU6500
        if (mpu6500GyroDetect(dev) || mpu6500SpiGyroDetect(dev)) {
#else
        if (mpu6500GyroDetect(dev)) {
#endif
            switch (dev->mpuDetectionResult.sensor) {
            case MPU_9250_SPI:
                gyroHardware = GYRO_MPU9250;
                break;
            case ICM_20601_SPI:
                gyroHardware = GYRO_ICM20601;
                break;
            case ICM_20602_SPI:
                gyroHardware = GYRO_ICM20602;
                break;
            case ICM_20608_SPI:
                gyroHardware = GYRO_ICM20608G;
                break;
            default:
                gyroHardware = GYRO_MPU6500;
            }
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_SPI_MPU9250
    case GYRO_MPU9250:
        if (mpu9250SpiGyroDetect(dev)) {
            gyroHardware = GYRO_MPU9250;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_SPI_ICM20649
    case GYRO_ICM20649:
        if (icm20649SpiGyroDetect(dev)) {
            gyroHardware = GYRO_ICM20649;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_GYRO_SPI_ICM20689
    case GYRO_ICM20689:
        if (icm20689SpiGyroDetect(dev)) {
            gyroHardware = GYRO_ICM20689;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_ACCGYRO_BMI160
    case GYRO_BMI160:
        if (bmi160SpiGyroDetect(dev)) {
            gyroHardware = GYRO_BMI160;
            break;
        }
        FALLTHROUGH;
#endif

#ifdef USE_FAKE_GYRO
    case GYRO_FAKE:
        if (fakeGyroDetect(dev)) {
            gyroHardware = GYRO_FAKE;
            break;
        }
        FALLTHROUGH;
#endif

    default:
        gyroHardware = GYRO_NONE;
    }

    if (gyroHardware != GYRO_NONE) {
        sensorsSet(SENSOR_GYRO);
    }


    return gyroHardware;
}

static void gyroPreInitSensor(const gyroDeviceConfig_t *config)
{
#if defined(USE_GYRO_MPU6050) || defined(USE_GYRO_MPU3050) || defined(USE_GYRO_MPU6500) || defined(USE_GYRO_SPI_MPU6500) || defined(USE_GYRO_SPI_MPU6000) \
 || defined(USE_ACC_MPU6050) || defined(USE_GYRO_SPI_MPU9250) || defined(USE_GYRO_SPI_ICM20601) || defined(USE_GYRO_SPI_ICM20649) || defined(USE_GYRO_SPI_ICM20689)
    mpuPreInit(config);
#else
    UNUSED(config);
#endif
}

static bool gyroDetectSensor(gyroSensor_t *gyroSensor, const gyroDeviceConfig_t *config)
{
#if defined(USE_GYRO_MPU6050) || defined(USE_GYRO_MPU3050) || defined(USE_GYRO_MPU6500) || defined(USE_GYRO_SPI_MPU6500) || defined(USE_GYRO_SPI_MPU6000) \
 || defined(USE_ACC_MPU6050) || defined(USE_GYRO_SPI_MPU9250) || defined(USE_GYRO_SPI_ICM20601) || defined(USE_GYRO_SPI_ICM20649) || defined(USE_GYRO_SPI_ICM20689) || defined(USE_GYRO_L3GD20)

    bool gyroFound = mpuDetect(&gyroSensor->gyroDev, config);

#if !defined(USE_FAKE_GYRO) // Allow resorting to fake accgyro if defined
    if (!gyroFound) {
        return false;
    }
#else
    UNUSED(gyroFound);
#endif
#else
    UNUSED(config);
#endif

    const gyroHardware_e gyroHardware = gyroDetect(&gyroSensor->gyroDev);
    gyroSensor->gyroDev.gyroHardware = gyroHardware;

    return gyroHardware != GYRO_NONE;
}

static void gyroInitSensor(gyroSensor_t *gyroSensor, const gyroDeviceConfig_t *config)
{
    gyroSensor->gyroDev.gyro_high_fsr = gyroConfig()->gyro_high_fsr;
    gyroSensor->gyroDev.gyroAlign = config->alignment;
    buildRotationMatrixFromAlignment(&config->customAlignment, &gyroSensor->gyroDev.rotationMatrix);
    gyroSensor->gyroDev.mpuIntExtiTag = config->extiTag;

    // Must set gyro targetLooptime before gyroDev.init and initialisation of filters
    gyro.targetLooptime = gyroSetSampleRate(&gyroSensor->gyroDev, gyroConfig()->gyro_hardware_lpf, gyroConfig()->gyro_sync_denom);
    gyroSensor->gyroDev.hardware_lpf = gyroConfig()->gyro_hardware_lpf;
    gyroSensor->gyroDev.initFn(&gyroSensor->gyroDev);

    // As new gyros are supported, be sure to add them below based on whether they are subject to the overflow/inversion bug
    // Any gyro not explicitly defined will default to not having built-in overflow protection as a safe alternative.
    switch (gyroSensor->gyroDev.gyroHardware) {
    case GYRO_NONE:    // Won't ever actually get here, but included to account for all gyro types
    case GYRO_DEFAULT:
    case GYRO_FAKE:
    case GYRO_MPU6050:
    case GYRO_L3G4200D:
    case GYRO_MPU3050:
    case GYRO_L3GD20:
    case GYRO_BMI160:
    case GYRO_MPU6000:
    case GYRO_MPU6500:
    case GYRO_MPU9250:
        gyroSensor->gyroDev.gyroHasOverflowProtection = true;
        break;

    case GYRO_ICM20601:
    case GYRO_ICM20602:
    case GYRO_ICM20608G:
    case GYRO_ICM20649:  // we don't actually know if this is affected, but as there are currently no flight controllers using it we err on the side of caution
    case GYRO_ICM20689:
        gyroSensor->gyroDev.gyroHasOverflowProtection = false;
        break;

    default:
        gyroSensor->gyroDev.gyroHasOverflowProtection = false;  // default catch for newly added gyros until proven to be unaffected
        break;
    }

    gyroInitSensorFilters(gyroSensor);

}

void gyroPreInit(void)
{
    gyroPreInitSensor(gyroDeviceConfig(0));
#ifdef USE_MULTI_GYRO
    gyroPreInitSensor(gyroDeviceConfig(1));
#endif
}

bool gyroInit(void)
{
#ifdef USE_GYRO_OVERFLOW_CHECK
    if (gyroConfig()->checkOverflow == GYRO_OVERFLOW_CHECK_YAW) {
        overflowAxisMask = GYRO_OVERFLOW_Z;
    } else if (gyroConfig()->checkOverflow == GYRO_OVERFLOW_CHECK_ALL_AXES) {
        overflowAxisMask = GYRO_OVERFLOW_X | GYRO_OVERFLOW_Y | GYRO_OVERFLOW_Z;
    } else {
        overflowAxisMask = 0;
    }
#endif

    gyroDebugMode = DEBUG_NONE;
    useDualGyroDebugging = false;

    switch (debugMode) {
    case DEBUG_FFT:
    case DEBUG_FFT_FREQ:
    case DEBUG_GYRO_RAW:
    case DEBUG_GYRO_SCALED:
    case DEBUG_GYRO_FILTERED:
    case DEBUG_DYN_LPF:
        gyroDebugMode = debugMode;
        break;
    case DEBUG_DUAL_GYRO_DIFF:
    case DEBUG_DUAL_GYRO_RAW:
    case DEBUG_DUAL_GYRO_SCALED:
        useDualGyroDebugging = true;
        break;
    }
    firstArmingCalibrationWasStarted = false;

    gyroDetectionFlags = NO_GYROS_DETECTED;

    gyroToUse = gyroConfig()->gyro_to_use;
    gyroDebugAxis = gyroConfig()->gyro_filter_debug_axis;

    if (gyroDetectSensor(&gyroSensor1, gyroDeviceConfig(0))) {
        gyroDetectionFlags |= DETECTED_GYRO_1;
    }

#if defined(USE_MULTI_GYRO)
    if (gyroDetectSensor(&gyroSensor2, gyroDeviceConfig(1))) {
        gyroDetectionFlags |= DETECTED_GYRO_2;
    }
#endif

    if (gyroDetectionFlags == NO_GYROS_DETECTED) {
        return false;
    }

#if defined(USE_MULTI_GYRO)
    if ((gyroToUse == GYRO_CONFIG_USE_GYRO_BOTH && !((gyroDetectionFlags & DETECTED_BOTH_GYROS) == DETECTED_BOTH_GYROS))
        || (gyroToUse == GYRO_CONFIG_USE_GYRO_1 && !(gyroDetectionFlags & DETECTED_GYRO_1))
        || (gyroToUse == GYRO_CONFIG_USE_GYRO_2 && !(gyroDetectionFlags & DETECTED_GYRO_2))) {
        if (gyroDetectionFlags & DETECTED_GYRO_1) {
            gyroToUse = GYRO_CONFIG_USE_GYRO_1;
        } else {
            gyroToUse = GYRO_CONFIG_USE_GYRO_2;
        }

        gyroConfigMutable()->gyro_to_use = gyroToUse;
    }

    // Only allow using both gyros simultaneously if they are the same hardware type.
    if (((gyroDetectionFlags & DETECTED_BOTH_GYROS) == DETECTED_BOTH_GYROS) && gyroSensor1.gyroDev.gyroHardware == gyroSensor2.gyroDev.gyroHardware) {
        gyroDetectionFlags |= DETECTED_DUAL_GYROS;
    } else if (gyroToUse == GYRO_CONFIG_USE_GYRO_BOTH) {
        // If the user selected "BOTH" and they are not the same type, then reset to using only the first gyro.
        gyroToUse = GYRO_CONFIG_USE_GYRO_1;
        gyroConfigMutable()->gyro_to_use = gyroToUse;
    }

    if (gyroToUse == GYRO_CONFIG_USE_GYRO_2 || gyroToUse == GYRO_CONFIG_USE_GYRO_BOTH) {
        gyroInitSensor(&gyroSensor2, gyroDeviceConfig(1));
        gyroHasOverflowProtection =  gyroHasOverflowProtection && gyroSensor2.gyroDev.gyroHasOverflowProtection;
        detectedSensors[SENSOR_INDEX_GYRO] = gyroSensor2.gyroDev.gyroHardware;
    }
#endif

    if (gyroToUse == GYRO_CONFIG_USE_GYRO_1 || gyroToUse == GYRO_CONFIG_USE_GYRO_BOTH) {
        gyroInitSensor(&gyroSensor1, gyroDeviceConfig(0));
        gyroHasOverflowProtection =  gyroHasOverflowProtection && gyroSensor1.gyroDev.gyroHasOverflowProtection;
        detectedSensors[SENSOR_INDEX_GYRO] = gyroSensor1.gyroDev.gyroHardware;
    }

    // Copy the sensor's scale to the high-level gyro object. If running in "BOTH" mode
    // then logic above requires both sensors to be the same so we'll use sensor1's scale.
    // This will need to be revised if we ever allow different sensor types to be used simultaneously.
    // Likewise determine the appropriate raw data for use in DEBUG_GYRO_RAW
    gyro.scale = gyroSensor1.gyroDev.scale;
    gyro.rawSensorDev = &gyroSensor1.gyroDev;
#if defined(USE_MULTI_GYRO)
    if (gyroToUse == GYRO_CONFIG_USE_GYRO_2) {
        gyro.scale = gyroSensor2.gyroDev.scale;
        gyro.rawSensorDev = &gyroSensor2.gyroDev;
    }
#endif

    gyroInitFilters();
    return true;
}

gyroDetectionFlags_t getGyroDetectionFlags(void)
{
    return gyroDetectionFlags;
}

#ifdef USE_DYN_LPF
static FAST_RAM uint8_t dynLpfFilter = DYN_LPF_NONE;
static FAST_RAM_ZERO_INIT uint16_t dynLpfMin;
static FAST_RAM_ZERO_INIT uint16_t dynLpfMax;

static void dynLpfFilterInit()
{
    if (gyroConfig()->dyn_lpf_gyro_min_hz > 0) {
        switch (gyroConfig()->gyro_lowpass_type) {
        case FILTER_PT1:
            dynLpfFilter = DYN_LPF_PT1;
            break;
        case FILTER_BIQUAD:
            dynLpfFilter = DYN_LPF_BIQUAD;
            break;
        default:
            dynLpfFilter = DYN_LPF_NONE;
            break;
        } 
    } else {
        dynLpfFilter = DYN_LPF_NONE;
    }
    dynLpfMin = gyroConfig()->dyn_lpf_gyro_min_hz;
    dynLpfMax = gyroConfig()->dyn_lpf_gyro_max_hz;
}
#endif

void gyroInitLowpassFilterLpf(int slot, int type, uint16_t lpfHz)
{
    filterApplyFnPtr *lowpassFilterApplyFn;
    gyroLowpassFilter_t *lowpassFilter = NULL;

    switch (slot) {
    case FILTER_LOWPASS:
        lowpassFilterApplyFn = &gyro.lowpassFilterApplyFn;
        lowpassFilter = gyro.lowpassFilter;
        break;

    case FILTER_LOWPASS2:
        lowpassFilterApplyFn = &gyro.lowpass2FilterApplyFn;
        lowpassFilter = gyro.lowpass2Filter;
        break;

    default:
        return;
    }

    // Establish some common constants
    const uint32_t gyroFrequencyNyquist = 1000000 / 2 / gyro.targetLooptime;
    const float gyroDt = gyro.targetLooptime * 1e-6f;

    // Gain could be calculated a little later as it is specific to the pt1/bqrcf2/fkf branches
    const float gain = pt1FilterGain(lpfHz, gyroDt);

    // Dereference the pointer to null before checking valid cutoff and filter
    // type. It will be overridden for positive cases.
    *lowpassFilterApplyFn = nullFilterApply;

    // If lowpass cutoff has been specified and is less than the Nyquist frequency
    if (lpfHz && lpfHz <= gyroFrequencyNyquist) {
        switch (type) {
        case FILTER_PT1:
            *lowpassFilterApplyFn = (filterApplyFnPtr) pt1FilterApply;
            for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
                pt1FilterInit(&lowpassFilter[axis].pt1FilterState, gain);
            }
            break;
        case FILTER_BIQUAD:
#ifdef USE_DYN_LPF
            *lowpassFilterApplyFn = (filterApplyFnPtr) biquadFilterApplyDF1;
#else
            *lowpassFilterApplyFn = (filterApplyFnPtr) biquadFilterApply;
#endif
            for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
                biquadFilterInitLPF(&lowpassFilter[axis].biquadFilterState, lpfHz, gyro.targetLooptime);
            }
            break;
        }
    }
}

static uint16_t calculateNyquistAdjustedNotchHz(uint16_t notchHz, uint16_t notchCutoffHz)
{
    const uint32_t gyroFrequencyNyquist = 1000000 / 2 / gyro.targetLooptime;
    if (notchHz > gyroFrequencyNyquist) {
        if (notchCutoffHz < gyroFrequencyNyquist) {
            notchHz = gyroFrequencyNyquist;
        } else {
            notchHz = 0;
        }
    }

    return notchHz;
}

#if defined(USE_GYRO_SLEW_LIMITER)
void gyroInitSlewLimiter(gyroSensor_t *gyroSensor) {

    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        gyroSensor->gyroDev.gyroADCRawPrevious[axis] = 0;
    }
}
#endif

static void gyroInitFilterNotch1(uint16_t notchHz, uint16_t notchCutoffHz)
{
    gyro.notchFilter1ApplyFn = nullFilterApply;

    notchHz = calculateNyquistAdjustedNotchHz(notchHz, notchCutoffHz);

    if (notchHz != 0 && notchCutoffHz != 0) {
        gyro.notchFilter1ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        const float notchQ = filterGetNotchQ(notchHz, notchCutoffHz);
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            biquadFilterInit(&gyro.notchFilter1[axis], notchHz, gyro.targetLooptime, notchQ, FILTER_NOTCH);
        }
    }
}

static void gyroInitFilterNotch2(uint16_t notchHz, uint16_t notchCutoffHz)
{
    gyro.notchFilter2ApplyFn = nullFilterApply;

    notchHz = calculateNyquistAdjustedNotchHz(notchHz, notchCutoffHz);

    if (notchHz != 0 && notchCutoffHz != 0) {
        gyro.notchFilter2ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        const float notchQ = filterGetNotchQ(notchHz, notchCutoffHz);
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            biquadFilterInit(&gyro.notchFilter2[axis], notchHz, gyro.targetLooptime, notchQ, FILTER_NOTCH);
        }
    }
}

#ifdef USE_GYRO_DATA_ANALYSE
static bool isDynamicFilterActive(void)
{
    return featureIsEnabled(FEATURE_DYNAMIC_FILTER);
}

static void gyroInitFilterDynamicNotch()
{
    gyro.notchFilterDynApplyFn = nullFilterApply;
    gyro.notchFilterDynApplyFn2 = nullFilterApply;

    if (isDynamicFilterActive()) {
        gyro.notchFilterDynApplyFn = (filterApplyFnPtr)biquadFilterApplyDF1; // must be this function, not DF2
        if(gyroConfig()->dyn_notch_width_percent != 0) {
            gyro.notchFilterDynApplyFn2 = (filterApplyFnPtr)biquadFilterApplyDF1; // must be this function, not DF2
        }
        const float notchQ = filterGetNotchQ(DYNAMIC_NOTCH_DEFAULT_CENTER_HZ, DYNAMIC_NOTCH_DEFAULT_CUTOFF_HZ); // any defaults OK here
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            biquadFilterInit(&gyro.notchFilterDyn[axis], DYNAMIC_NOTCH_DEFAULT_CENTER_HZ, gyro.targetLooptime, notchQ, FILTER_NOTCH);
            biquadFilterInit(&gyro.notchFilterDyn2[axis], DYNAMIC_NOTCH_DEFAULT_CENTER_HZ, gyro.targetLooptime, notchQ, FILTER_NOTCH);
        }
    }
}
#endif

static void gyroInitSensorFilters(gyroSensor_t *gyroSensor)
{
#if defined(USE_GYRO_SLEW_LIMITER)
    gyroInitSlewLimiter(gyroSensor);
#else
    UNUSED(gyroSensor);
#endif
}

void gyroInitFilters(void)
{
    uint16_t gyro_lowpass_hz = gyroConfig()->gyro_lowpass_hz;

#ifdef USE_DYN_LPF
    if (gyroConfig()->dyn_lpf_gyro_min_hz > 0) {
        gyro_lowpass_hz = gyroConfig()->dyn_lpf_gyro_min_hz;
    }
#endif

    gyroInitLowpassFilterLpf(
      FILTER_LOWPASS,
      gyroConfig()->gyro_lowpass_type,
      gyro_lowpass_hz
    );

    gyroInitLowpassFilterLpf(
      FILTER_LOWPASS2,
      gyroConfig()->gyro_lowpass2_type,
      gyroConfig()->gyro_lowpass2_hz
    );

    gyroInitFilterNotch1(gyroConfig()->gyro_soft_notch_hz_1, gyroConfig()->gyro_soft_notch_cutoff_1);
    gyroInitFilterNotch2(gyroConfig()->gyro_soft_notch_hz_2, gyroConfig()->gyro_soft_notch_cutoff_2);
#ifdef USE_GYRO_DATA_ANALYSE
    gyroInitFilterDynamicNotch();
#endif
#ifdef USE_DYN_LPF
    dynLpfFilterInit();
#endif
#ifdef USE_GYRO_DATA_ANALYSE
    gyroDataAnalyseStateInit(&gyro.gyroAnalyseState, gyro.targetLooptime);
#endif
}

FAST_CODE bool isGyroSensorCalibrationComplete(const gyroSensor_t *gyroSensor)
{
    return gyroSensor->calibration.cyclesRemaining == 0;
}

FAST_CODE bool gyroIsCalibrationComplete(void)
{
    switch (gyroToUse) {
        default:
        case GYRO_CONFIG_USE_GYRO_1: {
            return isGyroSensorCalibrationComplete(&gyroSensor1);
        }
#ifdef USE_MULTI_GYRO
        case GYRO_CONFIG_USE_GYRO_2: {
            return isGyroSensorCalibrationComplete(&gyroSensor2);
        }
        case GYRO_CONFIG_USE_GYRO_BOTH: {
            return isGyroSensorCalibrationComplete(&gyroSensor1) && isGyroSensorCalibrationComplete(&gyroSensor2);
        }
#endif
    }
}

static bool isOnFinalGyroCalibrationCycle(const gyroCalibration_t *gyroCalibration)
{
    return gyroCalibration->cyclesRemaining == 1;
}

static int32_t gyroCalculateCalibratingCycles(void)
{
    return (gyroConfig()->gyroCalibrationDuration * 10000) / gyro.targetLooptime;
}

static bool isOnFirstGyroCalibrationCycle(const gyroCalibration_t *gyroCalibration)
{
    return gyroCalibration->cyclesRemaining == gyroCalculateCalibratingCycles();
}

static void gyroSetCalibrationCycles(gyroSensor_t *gyroSensor)
{
#if defined(USE_FAKE_GYRO) && !defined(UNIT_TEST)
    if (gyroSensor->gyroDev.gyroHardware == GYRO_FAKE) {
        gyroSensor->calibration.cyclesRemaining = 0;
        return;
    }
#endif
    gyroSensor->calibration.cyclesRemaining = gyroCalculateCalibratingCycles();
}

void gyroStartCalibration(bool isFirstArmingCalibration)
{
    if (isFirstArmingCalibration && firstArmingCalibrationWasStarted) {
        return;
    }

    gyroSetCalibrationCycles(&gyroSensor1);
#ifdef USE_MULTI_GYRO
    gyroSetCalibrationCycles(&gyroSensor2);
#endif

    if (isFirstArmingCalibration) {
        firstArmingCalibrationWasStarted = true;
    }
}

bool isFirstArmingGyroCalibrationRunning(void)
{
    return firstArmingCalibrationWasStarted && !gyroIsCalibrationComplete();
}

STATIC_UNIT_TESTED void performGyroCalibration(gyroSensor_t *gyroSensor, uint8_t gyroMovementCalibrationThreshold)
{
    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        // Reset g[axis] at start of calibration
        if (isOnFirstGyroCalibrationCycle(&gyroSensor->calibration)) {
            gyroSensor->calibration.sum[axis] = 0.0f;
            devClear(&gyroSensor->calibration.var[axis]);
            // gyroZero is set to zero until calibration complete
            gyroSensor->gyroDev.gyroZero[axis] = 0.0f;
        }

        // Sum up CALIBRATING_GYRO_TIME_US readings
        gyroSensor->calibration.sum[axis] += gyroSensor->gyroDev.gyroADCRaw[axis];
        devPush(&gyroSensor->calibration.var[axis], gyroSensor->gyroDev.gyroADCRaw[axis]);

        if (isOnFinalGyroCalibrationCycle(&gyroSensor->calibration)) {
            const float stddev = devStandardDeviation(&gyroSensor->calibration.var[axis]);
            // DEBUG_GYRO_CALIBRATION records the standard deviation of roll
            // into the spare field - debug[3], in DEBUG_GYRO_RAW
            if (axis == X) {
                DEBUG_SET(DEBUG_GYRO_RAW, DEBUG_GYRO_CALIBRATION, lrintf(stddev));
            }

            // check deviation and startover in case the model was moved
            if (gyroMovementCalibrationThreshold && stddev > gyroMovementCalibrationThreshold) {
                gyroSetCalibrationCycles(gyroSensor);
                return;
            }

            // please take care with exotic boardalignment !!
            gyroSensor->gyroDev.gyroZero[axis] = gyroSensor->calibration.sum[axis] / gyroCalculateCalibratingCycles();
            if (axis == Z) {
              gyroSensor->gyroDev.gyroZero[axis] -= ((float)gyroConfig()->gyro_offset_yaw / 100);
            }
        }
    }

    if (isOnFinalGyroCalibrationCycle(&gyroSensor->calibration)) {
        schedulerResetTaskStatistics(TASK_SELF); // so calibration cycles do not pollute tasks statistics
        if (!firstArmingCalibrationWasStarted || (getArmingDisableFlags() & ~ARMING_DISABLED_CALIBRATING) == 0) {
            beeper(BEEPER_GYRO_CALIBRATED);
        }
    }

    --gyroSensor->calibration.cyclesRemaining;
}

#if defined(USE_GYRO_SLEW_LIMITER)
FAST_CODE int32_t gyroSlewLimiter(gyroSensor_t *gyroSensor, int axis)
{
    int32_t ret = (int32_t)gyroSensor->gyroDev.gyroADCRaw[axis];
    if (gyroConfig()->checkOverflow || gyroHasOverflowProtection) {
        // don't use the slew limiter if overflow checking is on or gyro is not subject to overflow bug
        return ret;
    }
    if (abs(ret - gyroSensor->gyroDev.gyroADCRawPrevious[axis]) > (1<<14)) {
        // there has been a large change in value, so assume overflow has occurred and return the previous value
        ret = gyroSensor->gyroDev.gyroADCRawPrevious[axis];
    } else {
        gyroSensor->gyroDev.gyroADCRawPrevious[axis] = ret;
    }
    return ret;
}
#endif

#ifdef USE_GYRO_OVERFLOW_CHECK
static FAST_CODE_NOINLINE void handleOverflow(timeUs_t currentTimeUs)
{
    // This will need to be revised if we ever allow different sensor types to be 
    // used simultaneously. In that case the scale might be different between sensors.
    // It's complicated by the fact that we're using filtered gyro data here which is
    // after both sensors are scaled and averaged.
    const float gyroOverflowResetRate = GYRO_OVERFLOW_RESET_THRESHOLD * gyro.scale;

    if ((fabsf(gyro.gyroADCf[X]) < gyroOverflowResetRate)
          && (fabsf(gyro.gyroADCf[Y]) < gyroOverflowResetRate)
          && (fabsf(gyro.gyroADCf[Z]) < gyroOverflowResetRate)) {
        // if we have 50ms of consecutive OK gyro vales, then assume yaw readings are OK again and reset overflowDetected
        // reset requires good OK values on all axes
        if (cmpTimeUs(currentTimeUs, overflowTimeUs) > 50000) {
            overflowDetected = false;
        }
    } else {
        // not a consecutive OK value, so reset the overflow time
        overflowTimeUs = currentTimeUs;
    }
}

static FAST_CODE void checkForOverflow(timeUs_t currentTimeUs)
{
    // check for overflow to handle Yaw Spin To The Moon (YSTTM)
    // ICM gyros are specified to +/- 2000 deg/sec, in a crash they can go out of spec.
    // This can cause an overflow and sign reversal in the output.
    // Overflow and sign reversal seems to result in a gyro value of +1996 or -1996.
    if (overflowDetected) {
        handleOverflow(currentTimeUs);
    } else {
#ifndef SIMULATOR_BUILD
        // check for overflow in the axes set in overflowAxisMask
        gyroOverflow_e overflowCheck = GYRO_OVERFLOW_NONE;

        // This will need to be revised if we ever allow different sensor types to be 
        // used simultaneously. In that case the scale might be different between sensors.
        // It's complicated by the fact that we're using filtered gyro data here which is
        // after both sensors are scaled and averaged.
        const float gyroOverflowTriggerRate = GYRO_OVERFLOW_TRIGGER_THRESHOLD * gyro.scale;

        if (fabsf(gyro.gyroADCf[X]) > gyroOverflowTriggerRate) {
            overflowCheck |= GYRO_OVERFLOW_X;
        }
        if (fabsf(gyro.gyroADCf[Y]) > gyroOverflowTriggerRate) {
            overflowCheck |= GYRO_OVERFLOW_Y;
        }
        if (fabsf(gyro.gyroADCf[Z]) > gyroOverflowTriggerRate) {
            overflowCheck |= GYRO_OVERFLOW_Z;
        }
        if (overflowCheck & overflowAxisMask) {
            overflowDetected = true;
            overflowTimeUs = currentTimeUs;
#ifdef USE_YAW_SPIN_RECOVERY
            yawSpinDetected = false;
#endif // USE_YAW_SPIN_RECOVERY
        }
#endif // SIMULATOR_BUILD
    }
}
#endif // USE_GYRO_OVERFLOW_CHECK

#ifdef USE_YAW_SPIN_RECOVERY
static FAST_CODE_NOINLINE void handleYawSpin(timeUs_t currentTimeUs)
{
    const float yawSpinResetRate = gyroConfig()->yaw_spin_threshold - 100.0f;
    if (fabsf(gyro.gyroADCf[Z]) < yawSpinResetRate) {
        // testing whether 20ms of consecutive OK gyro yaw values is enough
        if (cmpTimeUs(currentTimeUs, yawSpinTimeUs) > 20000) {
            yawSpinDetected = false;
        }
    } else {
        // reset the yaw spin time
        yawSpinTimeUs = currentTimeUs;
    }
}

static FAST_CODE void checkForYawSpin(timeUs_t currentTimeUs)
{
    // if not in overflow mode, handle yaw spins above threshold
#ifdef USE_GYRO_OVERFLOW_CHECK
    if (overflowDetected) {
        yawSpinDetected = false;
        return;
    }
#endif // USE_GYRO_OVERFLOW_CHECK

    if (yawSpinDetected) {
        handleYawSpin(currentTimeUs);
    } else {
#ifndef SIMULATOR_BUILD
        // check for spin on yaw axis only
         if (abs(gyro.gyroADCf[Z]) > gyroConfig()->yaw_spin_threshold) {
            yawSpinDetected = true;
            yawSpinTimeUs = currentTimeUs;
        }
#endif // SIMULATOR_BUILD
    }
}
#endif // USE_YAW_SPIN_RECOVERY

static FAST_CODE FAST_CODE_NOINLINE void gyroUpdateSensor(gyroSensor_t *gyroSensor)
{
    if (!gyroSensor->gyroDev.readFn(&gyroSensor->gyroDev)) {
        return;
    }
    gyroSensor->gyroDev.dataReady = false;

    if (isGyroSensorCalibrationComplete(gyroSensor)) {
        // move 16-bit gyro data into 32-bit variables to avoid overflows in calculations

#if defined(USE_GYRO_SLEW_LIMITER)
        gyroSensor->gyroDev.gyroADC[X] = gyroSlewLimiter(gyroSensor, X) - gyroSensor->gyroDev.gyroZero[X];
        gyroSensor->gyroDev.gyroADC[Y] = gyroSlewLimiter(gyroSensor, Y) - gyroSensor->gyroDev.gyroZero[Y];
        gyroSensor->gyroDev.gyroADC[Z] = gyroSlewLimiter(gyroSensor, Z) - gyroSensor->gyroDev.gyroZero[Z];
#else
        gyroSensor->gyroDev.gyroADC[X] = gyroSensor->gyroDev.gyroADCRaw[X] - gyroSensor->gyroDev.gyroZero[X];
        gyroSensor->gyroDev.gyroADC[Y] = gyroSensor->gyroDev.gyroADCRaw[Y] - gyroSensor->gyroDev.gyroZero[Y];
        gyroSensor->gyroDev.gyroADC[Z] = gyroSensor->gyroDev.gyroADCRaw[Z] - gyroSensor->gyroDev.gyroZero[Z];
#endif

        if (gyroSensor->gyroDev.gyroAlign == ALIGN_CUSTOM) {
            alignSensorViaMatrix(gyroSensor->gyroDev.gyroADC, &gyroSensor->gyroDev.rotationMatrix);
        } else {
            alignSensorViaRotation(gyroSensor->gyroDev.gyroADC, gyroSensor->gyroDev.gyroAlign);
        }
    } else {
        performGyroCalibration(gyroSensor, gyroConfig()->gyroMovementCalibrationThreshold);
    }
}

#define GYRO_FILTER_FUNCTION_NAME filterGyro
#define GYRO_FILTER_DEBUG_SET(mode, index, value) { UNUSED(mode); UNUSED(index); UNUSED(value); }
#include "gyro_filter_impl.c"
#undef GYRO_FILTER_FUNCTION_NAME
#undef GYRO_FILTER_DEBUG_SET

#define GYRO_FILTER_FUNCTION_NAME filterGyroDebug
#define GYRO_FILTER_DEBUG_SET DEBUG_SET
#include "gyro_filter_impl.c"
#undef GYRO_FILTER_FUNCTION_NAME
#undef GYRO_FILTER_DEBUG_SET

FAST_CODE void gyroUpdate(timeUs_t currentTimeUs)
{

    switch (gyroToUse) {
    case GYRO_CONFIG_USE_GYRO_1:
        gyroUpdateSensor(&gyroSensor1);
        if (isGyroSensorCalibrationComplete(&gyroSensor1)) {
            gyro.gyroADC[X] = gyroSensor1.gyroDev.gyroADC[X] * gyroSensor1.gyroDev.scale;
            gyro.gyroADC[Y] = gyroSensor1.gyroDev.gyroADC[Y] * gyroSensor1.gyroDev.scale;
            gyro.gyroADC[Z] = gyroSensor1.gyroDev.gyroADC[Z] * gyroSensor1.gyroDev.scale;
        }
        break;
#ifdef USE_MULTI_GYRO
    case GYRO_CONFIG_USE_GYRO_2:
        gyroUpdateSensor(&gyroSensor2);
        if (isGyroSensorCalibrationComplete(&gyroSensor2)) {
            gyro.gyroADC[X] = gyroSensor2.gyroDev.gyroADC[X] * gyroSensor2.gyroDev.scale;
            gyro.gyroADC[Y] = gyroSensor2.gyroDev.gyroADC[Y] * gyroSensor2.gyroDev.scale;
            gyro.gyroADC[Z] = gyroSensor2.gyroDev.gyroADC[Z] * gyroSensor2.gyroDev.scale;
        }
        break;
    case GYRO_CONFIG_USE_GYRO_BOTH:
        gyroUpdateSensor(&gyroSensor1);
        gyroUpdateSensor(&gyroSensor2);
        if (isGyroSensorCalibrationComplete(&gyroSensor1) && isGyroSensorCalibrationComplete(&gyroSensor2)) {
            gyro.gyroADC[X] = ((gyroSensor1.gyroDev.gyroADC[X] * gyroSensor1.gyroDev.scale) + (gyroSensor2.gyroDev.gyroADC[X] * gyroSensor2.gyroDev.scale)) / 2.0f;
            gyro.gyroADC[Y] = ((gyroSensor1.gyroDev.gyroADC[Y] * gyroSensor1.gyroDev.scale) + (gyroSensor2.gyroDev.gyroADC[Y] * gyroSensor2.gyroDev.scale)) / 2.0f;
            gyro.gyroADC[Z] = ((gyroSensor1.gyroDev.gyroADC[Z] * gyroSensor1.gyroDev.scale) + (gyroSensor2.gyroDev.gyroADC[Z] * gyroSensor2.gyroDev.scale)) / 2.0f;
        }
        break;
#endif
    }

    if (gyroDebugMode == DEBUG_NONE) {
        filterGyro();
    } else {
        filterGyroDebug();
    }

#ifdef USE_GYRO_DATA_ANALYSE
    if (isDynamicFilterActive()) {
        gyroDataAnalyse(&gyro.gyroAnalyseState, gyro.notchFilterDyn, gyro.notchFilterDyn2);
    }
#endif

    if (useDualGyroDebugging) {
        switch (gyroToUse) {
        case GYRO_CONFIG_USE_GYRO_1:
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 0, gyroSensor1.gyroDev.gyroADCRaw[X]);
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 1, gyroSensor1.gyroDev.gyroADCRaw[Y]);
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 0, lrintf(gyroSensor1.gyroDev.gyroADC[X] * gyroSensor1.gyroDev.scale));
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 1, lrintf(gyroSensor1.gyroDev.gyroADC[Y] * gyroSensor1.gyroDev.scale));
            break;

#ifdef USE_MULTI_GYRO
        case GYRO_CONFIG_USE_GYRO_2:
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 2, gyroSensor2.gyroDev.gyroADCRaw[X]);
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 3, gyroSensor2.gyroDev.gyroADCRaw[Y]);
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 2, lrintf(gyroSensor2.gyroDev.gyroADC[X] * gyroSensor2.gyroDev.scale));
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 3, lrintf(gyroSensor2.gyroDev.gyroADC[Y] * gyroSensor2.gyroDev.scale));
            break;

    case GYRO_CONFIG_USE_GYRO_BOTH:
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 0, gyroSensor1.gyroDev.gyroADCRaw[X]);
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 1, gyroSensor1.gyroDev.gyroADCRaw[Y]);
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 2, gyroSensor2.gyroDev.gyroADCRaw[X]);
            DEBUG_SET(DEBUG_DUAL_GYRO_RAW, 3, gyroSensor2.gyroDev.gyroADCRaw[Y]);
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 0, lrintf(gyroSensor1.gyroDev.gyroADC[X] * gyroSensor1.gyroDev.scale));
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 1, lrintf(gyroSensor1.gyroDev.gyroADC[Y] * gyroSensor1.gyroDev.scale));
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 2, lrintf(gyroSensor2.gyroDev.gyroADC[X] * gyroSensor2.gyroDev.scale));
            DEBUG_SET(DEBUG_DUAL_GYRO_SCALED, 3, lrintf(gyroSensor2.gyroDev.gyroADC[Y] * gyroSensor2.gyroDev.scale));
            DEBUG_SET(DEBUG_DUAL_GYRO_DIFF, 0, lrintf((gyroSensor1.gyroDev.gyroADC[X] * gyroSensor1.gyroDev.scale) - (gyroSensor2.gyroDev.gyroADC[X] * gyroSensor2.gyroDev.scale)));
            DEBUG_SET(DEBUG_DUAL_GYRO_DIFF, 1, lrintf((gyroSensor1.gyroDev.gyroADC[Y] * gyroSensor1.gyroDev.scale) - (gyroSensor2.gyroDev.gyroADC[Y] * gyroSensor2.gyroDev.scale)));
            DEBUG_SET(DEBUG_DUAL_GYRO_DIFF, 2, lrintf((gyroSensor1.gyroDev.gyroADC[Z] * gyroSensor1.gyroDev.scale) - (gyroSensor2.gyroDev.gyroADC[Z] * gyroSensor2.gyroDev.scale)));
            break;
#endif
        }
    }

#ifdef USE_GYRO_OVERFLOW_CHECK
    if (gyroConfig()->checkOverflow && !gyroHasOverflowProtection) {
        checkForOverflow(currentTimeUs);
    }
#endif

#ifdef USE_YAW_SPIN_RECOVERY
    if (gyroConfig()->yaw_spin_recovery) {
        checkForYawSpin(currentTimeUs);
    }
#endif

    if (!overflowDetected) {
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            // integrate using trapezium rule to avoid bias
            accumulatedMeasurements[axis] += 0.5f * (gyroPrevious[axis] + gyro.gyroADCf[axis]) * gyro.targetLooptime;
            gyroPrevious[axis] = gyro.gyroADCf[axis];
        }
        accumulatedMeasurementCount++;
    }

#if !defined(USE_GYRO_OVERFLOW_CHECK) && !defined(USE_YAW_SPIN_RECOVERY)
    UNUSED(currentTimeUs);
#endif
}

bool gyroGetAccumulationAverage(float *accumulationAverage)
{
    if (accumulatedMeasurementCount) {
        // If we have gyro data accumulated, calculate average rate that will yield the same rotation
        const timeUs_t accumulatedMeasurementTimeUs = accumulatedMeasurementCount * gyro.targetLooptime;
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            accumulationAverage[axis] = accumulatedMeasurements[axis] / accumulatedMeasurementTimeUs;
            accumulatedMeasurements[axis] = 0.0f;
        }
        accumulatedMeasurementCount = 0;
        return true;
    } else {
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            accumulationAverage[axis] = 0.0f;
        }
        return false;
    }
}

int16_t gyroReadSensorTemperature(gyroSensor_t gyroSensor)
{
    if (gyroSensor.gyroDev.temperatureFn) {
        gyroSensor.gyroDev.temperatureFn(&gyroSensor.gyroDev, &gyroSensor.gyroDev.temperature);
    }
    return gyroSensor.gyroDev.temperature;
}

void gyroReadTemperature(void)
{
    switch (gyroToUse) {
    case GYRO_CONFIG_USE_GYRO_1:
        gyroSensorTemperature = gyroReadSensorTemperature(gyroSensor1);
        break;

#ifdef USE_MULTI_GYRO
    case GYRO_CONFIG_USE_GYRO_2:
        gyroSensorTemperature = gyroReadSensorTemperature(gyroSensor2);
        break;

    case GYRO_CONFIG_USE_GYRO_BOTH:
        gyroSensorTemperature = MAX(gyroReadSensorTemperature(gyroSensor1), gyroReadSensorTemperature(gyroSensor2));
        break;
#endif // USE_MULTI_GYRO
    }
}

int16_t gyroGetTemperature(void)
{
    return gyroSensorTemperature;
}

int16_t gyroRateDps(int axis)
{
    return lrintf(gyro.gyroADCf[axis] / ACTIVE_GYRO->gyroDev.scale);
}

bool gyroOverflowDetected(void)
{
#ifdef USE_GYRO_OVERFLOW_CHECK
    return overflowDetected;
#else
    return false;
#endif // USE_GYRO_OVERFLOW_CHECK
}

#ifdef USE_YAW_SPIN_RECOVERY
bool gyroYawSpinDetected(void)
{
    return yawSpinDetected;
}
#endif // USE_YAW_SPIN_RECOVERY

uint16_t gyroAbsRateDps(int axis)
{
    return fabsf(gyro.gyroADCf[axis]);
}

#ifdef USE_GYRO_REGISTER_DUMP
uint8_t gyroReadRegister(uint8_t whichSensor, uint8_t reg)
{
    return mpuGyroReadRegister(gyroSensorBusByDevice(whichSensor), reg);
}
#endif // USE_GYRO_REGISTER_DUMP

#ifdef USE_DYN_LPF

float dynThrottle(float throttle) {
    return throttle * (1 - (throttle * throttle) / 3.0f) * 1.5f;
}

void dynLpfGyroUpdate(float throttle)
{
    if (dynLpfFilter != DYN_LPF_NONE) {
        const unsigned int cutoffFreq = fmax(dynThrottle(throttle) * dynLpfMax, dynLpfMin);

        if (dynLpfFilter == DYN_LPF_PT1) {
            DEBUG_SET(DEBUG_DYN_LPF, 2, cutoffFreq);
            const float gyroDt = gyro.targetLooptime * 1e-6f;
            for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
                pt1FilterUpdateCutoff(&gyro.lowpassFilter[axis].pt1FilterState, pt1FilterGain(cutoffFreq, gyroDt));
            }
        } else if (dynLpfFilter == DYN_LPF_BIQUAD) {
            DEBUG_SET(DEBUG_DYN_LPF, 2, cutoffFreq);
            for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
                biquadFilterUpdateLPF(&gyro.lowpassFilter[axis].biquadFilterState, cutoffFreq, gyro.targetLooptime);
            }
        }
    }
}
#endif

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

//
// Firmware related menu contents and support functions
//

#include <stdbool.h>

#include "platform.h"

#ifdef USE_CMS

#include "build/version.h"

#include "cms/cms.h"
#include "cms/cms_types.h"

#include "common/printf.h"

#include "config/config.h"

#include "drivers/system.h"

#include "fc/runtime_config.h"

#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/gyro.h"

#include "cms_menu_firmware.h"


// Calibration

#define CALIBRATION_STATUS_MAX_LENGTH 6

#define CALIBRATION_STATUS_OFF " --- "
#define CALIBRATION_STATUS_NOK " NOK "
#define CALIBRATION_STATUS_WAIT "WAIT "
#define CALIBRATION_STATUS_OK "  OK "

static char gyroCalibrationStatus[CALIBRATION_STATUS_MAX_LENGTH];
#if defined(USE_ACC)
static char accCalibrationStatus[CALIBRATION_STATUS_MAX_LENGTH];
#endif
#if defined(USE_BARO)
static char baroCalibrationStatus[CALIBRATION_STATUS_MAX_LENGTH];
#endif

static long cmsx_CalibrationOnDisplayUpdate(const OSD_Entry *selected)
{
    UNUSED(selected);

    tfp_sprintf(gyroCalibrationStatus, sensors(SENSOR_GYRO) ? gyroIsCalibrationComplete() ? CALIBRATION_STATUS_OK : CALIBRATION_STATUS_WAIT: CALIBRATION_STATUS_OFF);
#if defined(USE_ACC)
    tfp_sprintf(accCalibrationStatus, sensors(SENSOR_ACC) ? accIsCalibrationComplete() ? accHasBeenCalibrated() ? CALIBRATION_STATUS_OK : CALIBRATION_STATUS_NOK : CALIBRATION_STATUS_WAIT: CALIBRATION_STATUS_OFF);
#endif
#if defined(USE_BARO)
    tfp_sprintf(baroCalibrationStatus, sensors(SENSOR_BARO) ? baroIsCalibrationComplete() ? CALIBRATION_STATUS_OK : CALIBRATION_STATUS_WAIT: CALIBRATION_STATUS_OFF);
#endif

    return 0;
}

static long cmsCalibrateGyro(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (sensors(SENSOR_GYRO)) {
        gyroStartCalibration(false);
    }

    return 0;
}

#if defined(USE_ACC)
static long cmsCalibrateAcc(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (sensors(SENSOR_ACC)) {
        accStartCalibration();
    }

    return 0;
}
#endif

#if defined(USE_BARO)
static long cmsCalibrateBaro(displayPort_t *pDisp, const void *self)
{
    UNUSED(pDisp);
    UNUSED(self);

    if (sensors(SENSOR_BARO)) {
        baroStartCalibration();
    }

    return 0;
}
#endif

static const OSD_Entry menuCalibrationEntries[] = {
    { "--- CALIBRATE ---", OME_Label, NULL, NULL, 0 },
    { "GYRO", OME_Funcall, cmsCalibrateGyro, gyroCalibrationStatus, DYNAMIC },
#if defined(USE_ACC)
    { "ACC",  OME_Funcall, cmsCalibrateAcc, accCalibrationStatus, DYNAMIC },
#endif
#if defined(USE_BARO)
    { "BARO", OME_Funcall, cmsCalibrateBaro, baroCalibrationStatus, DYNAMIC },
#endif
    { "BACK", OME_Back, NULL, NULL, 0 },
    { NULL, OME_END, NULL, NULL, 0 }
};

static CMS_Menu cmsx_menuCalibration = {
#ifdef CMS_MENU_DEBUG
    .GUARD_text = "MENUCALIBRATION",
    .GUARD_type = OME_MENU,
#endif
    .onEnter = NULL,
    .onExit = NULL,
    .checkRedirect = NULL,
    .onDisplayUpdate = cmsx_CalibrationOnDisplayUpdate,
    .entries = menuCalibrationEntries
};

// Info

static char infoGitRev[GIT_SHORT_REVISION_LENGTH + 1];
static char infoTargetName[] = __TARGET__;

static long cmsx_FirmwareInit(void)
{
    unsigned i;
    for (i = 0 ; i < GIT_SHORT_REVISION_LENGTH ; i++) {
        if (shortGitRevision[i] >= 'a' && shortGitRevision[i] <= 'f') {
            infoGitRev[i] = shortGitRevision[i] - 'a' + 'A';
        } else {
            infoGitRev[i] = shortGitRevision[i];
        }
    }

    infoGitRev[i] = 0x0; // Terminate string

    return 0;
}

static const OSD_Entry menuFirmwareEntries[] = {
    { "--- INFO ---", OME_Label, NULL, NULL, 0 },
    { "FWID", OME_String, NULL, FC_FIRMWARE_IDENTIFIER, 0 },
    { "FWVER", OME_String, NULL, FC_VERSION_STRING, 0 },
    { "GITREV", OME_String, NULL, infoGitRev, 0 },
    { "TARGET", OME_String, NULL, infoTargetName, 0 },
    { "--- SETUP ---", OME_Label, NULL, NULL, 0 },
    { "CALIBRATE",     OME_Submenu, cmsMenuChange, &cmsx_menuCalibration, 0},
    { "BACK", OME_Back, NULL, NULL, 0 },
    { NULL, OME_END, NULL, NULL, 0 }
};

CMS_Menu cmsx_menuFirmware = {
#ifdef CMS_MENU_DEBUG
    .GUARD_text = "MENUFIRMWARE",
    .GUARD_type = OME_MENU,
#endif
    .onEnter = cmsx_FirmwareInit,
    .onExit = NULL,
    .checkRedirect = NULL,
    .onDisplayUpdate = NULL,
    .entries = menuFirmwareEntries
};
#endif

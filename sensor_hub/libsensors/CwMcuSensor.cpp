/*
 * Copyright (C) 2008-2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

#define LOG_TAG "CwMcuSensor"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <utils/BitSet.h>

#include "CwMcuSensor.h"


#define REL_Significant_Motion REL_WHEEL
#define LIGHTSENSOR_LEVEL 10
#define DEBUG_DATA 0
#define COMPASS_CALIBRATION_DATA_SIZE 26
#define G_SENSOR_CALIBRATION_DATA_SIZE 3
#define NS_PER_MS 1000000LL
#define SYNC_ACK_MAGIC  0x66
#define EXHAUSTED_MAGIC 0x77

/*****************************************************************************/
#define IIO_MAX_BUFF_SIZE 1024
#define IIO_MAX_DATA_SIZE 24
#define IIO_MAX_NAME_LENGTH 30
#define INT32_CHAR_LEN 12

static const char iio_dir[] = "/sys/bus/iio/devices/";

static int chomp(char *buf, size_t len) {
    if (buf == NULL)
        return -1;

    while (len > 0 && isspace(buf[len-1])) {
        buf[len - 1] = '\0';
        len--;
    }

    return 0;
}

int CwMcuSensor::sysfs_set_input_attr(const char *attr, char *value, size_t len) {
    char fname[PATH_MAX];
    int fd;
    int rc;

    snprintf(fname, sizeof(fname), "%s/%s", mDevPath, attr);
    fname[sizeof(fname) - 1] = '\0';

    fd = open(fname, O_WRONLY);
    if (fd < 0) {
        ALOGE("%s: fname = %s, fd = %d, failed: %s\n", __func__, fname, fd, strerror(errno));
        return -EACCES;
    }

    rc = write(fd, value, (size_t)len);
    if (rc < 0) {
        ALOGE("%s: write failed: fd = %d, rc = %d, strerr = %s\n", __func__, fd, rc, strerror(errno));
        close(fd);
        return -EIO;
    }

    close(fd);

    return 0;
}

int CwMcuSensor::sysfs_set_input_attr_by_int(const char *attr, int value) {
    char buf[INT32_CHAR_LEN];

    size_t n = snprintf(buf, sizeof(buf), "%d", value);
    if (n > sizeof(buf)) {
        return -1;
    }

    return sysfs_set_input_attr(attr, buf, n);
}

static inline int find_type_by_name(const char *name, const char *type) {
    const struct dirent *ent;
    int number, numstrlen;

    DIR *dp;
    char thisname[IIO_MAX_NAME_LENGTH];
    char *filename;
    size_t size;
    size_t typeLen = strlen(type);
    size_t nameLen = strlen(name);

    if (nameLen >= sizeof(thisname) - 1) {
        return -ERANGE;
    }

    dp = opendir(iio_dir);
    if (dp == NULL) {
        return -ENODEV;
    }

    while (ent = readdir(dp), ent != NULL) {
        if (strcmp(ent->d_name, ".") != 0 &&
                strcmp(ent->d_name, "..") != 0 &&
                strlen(ent->d_name) > typeLen &&
                strncmp(ent->d_name, type, typeLen) == 0) {
            numstrlen = sscanf(ent->d_name + typeLen,
                               "%d", &number);

            /* verify the next character is not a colon */
            if (ent->d_name[strlen(type) + numstrlen] != ':') {
                size = sizeof(iio_dir) - 1 + typeLen + numstrlen + 6;
                filename = (char *)malloc(size);

                if (filename == NULL)
                    return -ENOMEM;

                snprintf(filename, size,
                         "%s%s%d/name",
                         iio_dir, type, number);

                int fd = open(filename, O_RDONLY);
                free(filename);
                if (fd < 0) {
                    continue;
                }
                size = read(fd, thisname, sizeof(thisname) - 1);
                close(fd);
                if (size < nameLen) {
                    continue;
                }
                thisname[size] = '\0';
                if (strncmp(name, thisname, nameLen)) {
                    continue;
                }
                // check for termination or whitespace
                if (!thisname[nameLen] || isspace(thisname[nameLen])) {
                    return number;
                }
            }
        }
    }
    return -ENODEV;
}

int fill_block_debug = 0;

pthread_mutex_t sys_fs_mutex = PTHREAD_MUTEX_INITIALIZER;

CwMcuSensor::CwMcuSensor()
    : SensorBase(NULL, "CwMcuSensor")
    , mEnabled(0)
    , mInputReader(IIO_MAX_BUFF_SIZE)
    , mFlushSensorEnabled(-1)
    , l_timestamp(0)
    , g_timestamp(0)
    , init_trigger_done(false) {

    int rc;

    mPendingEvents[CW_ACCELERATION].version = sizeof(sensors_event_t);
    mPendingEvents[CW_ACCELERATION].sensor = ID_A;
    mPendingEvents[CW_ACCELERATION].type = SENSOR_TYPE_ACCELEROMETER;

    mPendingEvents[CW_MAGNETIC].version = sizeof(sensors_event_t);
    mPendingEvents[CW_MAGNETIC].sensor = ID_M;
    mPendingEvents[CW_MAGNETIC].type = SENSOR_TYPE_MAGNETIC_FIELD;

    mPendingEvents[CW_GYRO].version = sizeof(sensors_event_t);
    mPendingEvents[CW_GYRO].sensor = ID_GY;
    mPendingEvents[CW_GYRO].type = SENSOR_TYPE_GYROSCOPE;

    mPendingEvents[CW_LIGHT].version = sizeof(sensors_event_t);
    mPendingEvents[CW_LIGHT].sensor = ID_L;
    mPendingEvents[CW_LIGHT].type = SENSOR_TYPE_LIGHT;
    memset(mPendingEvents[CW_LIGHT].data, 0, sizeof(mPendingEvents[CW_LIGHT].data));

    mPendingEvents[CW_PRESSURE].version = sizeof(sensors_event_t);
    mPendingEvents[CW_PRESSURE].sensor = ID_PS;
    mPendingEvents[CW_PRESSURE].type = SENSOR_TYPE_PRESSURE;
    memset(mPendingEvents[CW_PRESSURE].data, 0, sizeof(mPendingEvents[CW_PRESSURE].data));

    mPendingEvents[CW_ORIENTATION].version = sizeof(sensors_event_t);
    mPendingEvents[CW_ORIENTATION].sensor = ID_O;
    mPendingEvents[CW_ORIENTATION].type = SENSOR_TYPE_ORIENTATION;
    mPendingEvents[CW_ORIENTATION].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_ROTATIONVECTOR].version = sizeof(sensors_event_t);
    mPendingEvents[CW_ROTATIONVECTOR].sensor = ID_RV;
    mPendingEvents[CW_ROTATIONVECTOR].type = SENSOR_TYPE_ROTATION_VECTOR;
    mPendingEvents[CW_ROTATIONVECTOR].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_LINEARACCELERATION].version = sizeof(sensors_event_t);
    mPendingEvents[CW_LINEARACCELERATION].sensor = ID_LA;
    mPendingEvents[CW_LINEARACCELERATION].type = SENSOR_TYPE_LINEAR_ACCELERATION;
    mPendingEvents[CW_LINEARACCELERATION].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_GRAVITY].version = sizeof(sensors_event_t);
    mPendingEvents[CW_GRAVITY].sensor = ID_G;
    mPendingEvents[CW_GRAVITY].type = SENSOR_TYPE_GRAVITY;
    mPendingEvents[CW_GRAVITY].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_MAGNETIC_UNCALIBRATED].version = sizeof(sensors_event_t);
    mPendingEvents[CW_MAGNETIC_UNCALIBRATED].sensor = ID_CW_MAGNETIC_UNCALIBRATED;
    mPendingEvents[CW_MAGNETIC_UNCALIBRATED].type = SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED;
    mPendingEvents[CW_MAGNETIC_UNCALIBRATED].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_GYROSCOPE_UNCALIBRATED].version = sizeof(sensors_event_t);
    mPendingEvents[CW_GYROSCOPE_UNCALIBRATED].sensor = ID_CW_GYROSCOPE_UNCALIBRATED;
    mPendingEvents[CW_GYROSCOPE_UNCALIBRATED].type = SENSOR_TYPE_GYROSCOPE_UNCALIBRATED;
    mPendingEvents[CW_GYROSCOPE_UNCALIBRATED].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_GAME_ROTATION_VECTOR].version = sizeof(sensors_event_t);
    mPendingEvents[CW_GAME_ROTATION_VECTOR].sensor = ID_CW_GAME_ROTATION_VECTOR;
    mPendingEvents[CW_GAME_ROTATION_VECTOR].type = SENSOR_TYPE_GAME_ROTATION_VECTOR;
    mPendingEvents[CW_GAME_ROTATION_VECTOR].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_GEOMAGNETIC_ROTATION_VECTOR].version = sizeof(sensors_event_t);
    mPendingEvents[CW_GEOMAGNETIC_ROTATION_VECTOR].sensor = ID_CW_GEOMAGNETIC_ROTATION_VECTOR;
    mPendingEvents[CW_GEOMAGNETIC_ROTATION_VECTOR].type = SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR;
    mPendingEvents[CW_GEOMAGNETIC_ROTATION_VECTOR].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_SIGNIFICANT_MOTION].version = sizeof(sensors_event_t);
    mPendingEvents[CW_SIGNIFICANT_MOTION].sensor = ID_CW_SIGNIFICANT_MOTION;
    mPendingEvents[CW_SIGNIFICANT_MOTION].type = SENSOR_TYPE_SIGNIFICANT_MOTION;
    mPendingEvents[CW_SIGNIFICANT_MOTION].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_STEP_DETECTOR].version = sizeof(sensors_event_t);
    mPendingEvents[CW_STEP_DETECTOR].sensor = ID_CW_STEP_DETECTOR;
    mPendingEvents[CW_STEP_DETECTOR].type = SENSOR_TYPE_STEP_DETECTOR;
    mPendingEvents[CW_STEP_DETECTOR].orientation.status = SENSOR_STATUS_ACCURACY_HIGH;

    mPendingEvents[CW_STEP_COUNTER].version = sizeof(sensors_event_t);
    mPendingEvents[CW_STEP_COUNTER].sensor = ID_CW_STEP_COUNTER;
    mPendingEvents[CW_STEP_COUNTER].type = SENSOR_TYPE_STEP_COUNTER;

    mPendingEvents[HTC_WAKE_UP_GESTURE].version = sizeof(sensors_event_t);
    mPendingEvents[HTC_WAKE_UP_GESTURE].sensor = ID_WAKE_UP_GESTURE;
    mPendingEvents[HTC_WAKE_UP_GESTURE].type = SENSOR_TYPE_WAKE_GESTURE;

    mPendingEventsFlush.version = META_DATA_VERSION;
    mPendingEventsFlush.sensor = 0;
    mPendingEventsFlush.type = SENSOR_TYPE_META_DATA;

    char buffer_access[PATH_MAX];
    const char *device_name = "CwMcuSensor";
    int rate = 20, dev_num, enabled = 0, i;

    dev_num = find_type_by_name(device_name, "iio:device");
    if (dev_num < 0)
        dev_num = 0;

    snprintf(buffer_access, sizeof(buffer_access),
            "/dev/iio:device%d", dev_num);

    data_fd = open(buffer_access, O_RDWR);
    if (data_fd < 0) {
        ALOGE("CwMcuSensor::CwMcuSensor: open file '%s' failed: %s\n",
              buffer_access, strerror(errno));
    }

    if (data_fd >= 0) {
        ALOGW("%s: 11 Before pthread_mutex_lock()\n", __func__);
        pthread_mutex_lock(&sys_fs_mutex);
        ALOGW("%s: 11 Acquired pthread_mutex_lock()\n", __func__);

        strcpy(fixed_sysfs_path,"/sys/class/htc_sensorhub/sensor_hub/");
        fixed_sysfs_path_len = strlen(fixed_sysfs_path);

        snprintf(mDevPath, sizeof(mDevPath), "%s%s", fixed_sysfs_path, "iio");

        snprintf(mTriggerName, sizeof(mTriggerName), "%s-dev%d",
                 device_name, dev_num);
        ALOGD("CwMcuSensor::CwMcuSensor: mTriggerName = %s\n", mTriggerName);

        if (sysfs_set_input_attr_by_int("buffer/length", IIO_MAX_BUFF_SIZE) < 0)
            ALOGE("CwMcuSensor::CwMcuSensor: set IIO buffer length failed: %s\n", strerror(errno));

        rc = sysfs_set_input_attr("trigger/current_trigger",
                                  mTriggerName, strlen(mTriggerName));
        if (rc < 0) {
            ALOGE("CwMcuSensor::CwMcuSensor: set current trigger failed: rc = %d, strerr() = %s\n",
                  rc, strerror(errno));
        } else {
            init_trigger_done = true;
        }

        if (sysfs_set_input_attr_by_int("buffer/enable", 1) < 0) {
            ALOGE("CwMcuSensor::CwMcuSensor: set IIO buffer enable failed: %s\n", strerror(errno));
        }

        pthread_mutex_unlock(&sys_fs_mutex);

        ALOGD("%s: data_fd = %d", __func__, data_fd);
        ALOGD("%s: iio_device_path = %s", __func__, buffer_access);
        ALOGD("%s: ctrl sysfs_path = %s", __func__, fixed_sysfs_path);

        setEnable(0, 1); // Inside this function call, we use sys_fs_mutex
    }

    int gs_temp_data[G_SENSOR_CALIBRATION_DATA_SIZE] = {0};
    int compass_temp_data[COMPASS_CALIBRATION_DATA_SIZE] = {0};


    ALOGW("%s: 22 Before pthread_mutex_lock()\n", __func__);
    pthread_mutex_lock(&sys_fs_mutex);
    ALOGW("%s: 22 Acquired pthread_mutex_lock()\n", __func__);

    //Sensor Calibration init . Waiting for firmware ready
    rc = cw_read_calibrator_file(CW_MAGNETIC, SAVE_PATH_MAG, compass_temp_data);
    if (rc == 0) {
        ALOGD("Get compass calibration data from data/misc/ x is %d ,y is %d ,z is %d\n",
              compass_temp_data[0], compass_temp_data[1], compass_temp_data[2]);
        strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "calibrator_data_mag");
        cw_save_calibrator_file(CW_MAGNETIC, fixed_sysfs_path, compass_temp_data);
    } else {
        ALOGI("Compass calibration data does not exist\n");
    }

    rc = cw_read_calibrator_file(CW_ACCELERATION, SAVE_PATH_ACC, gs_temp_data);
    if (rc == 0) {
        ALOGD("Get g-sensor user calibration data from data/misc/ x is %d ,y is %d ,z is %d\n",
              gs_temp_data[0],gs_temp_data[1],gs_temp_data[2]);
        strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "calibrator_data_acc");
        if(!(gs_temp_data[0] == 0 && gs_temp_data[1] == 0 && gs_temp_data[2] == 0 )) {
            cw_save_calibrator_file(CW_ACCELERATION, fixed_sysfs_path, gs_temp_data);
        }
    } else {
        ALOGI("G-Sensor user calibration data does not exist\n");
    }

    pthread_mutex_unlock(&sys_fs_mutex);

}

CwMcuSensor::~CwMcuSensor() {
    if (mEnabled) {
        setEnable(0, 0);
    }
}

float CwMcuSensor::indexToValue(size_t index) const {
    static const float luxValues[LIGHTSENSOR_LEVEL] = {
        0.0, 10.0, 40.0, 90.0, 160.0,
        225.0, 320.0, 640.0, 1280.0,
        2600.0
    };

    const size_t maxIndex = (LIGHTSENSOR_LEVEL - 1);
    if (index > maxIndex) {
        index = maxIndex;
    }
    return luxValues[index];
}

int CwMcuSensor::find_handle(int32_t sensors_id) {
    switch (sensors_id) {
    case CW_ACCELERATION:
        return ID_A;
        break;
    case CW_MAGNETIC:
        return ID_M;
        break;
    case CW_GYRO:
        return ID_GY;
        break;
    case CW_PRESSURE:
        return ID_PS;
        break;
    case CW_ORIENTATION:
        return ID_O;
        break;
    case CW_ROTATIONVECTOR:
        return ID_RV;
        break;
    case CW_LINEARACCELERATION:
        return ID_LA;
        break;
    case CW_GRAVITY:
        return ID_G;
        break;
    case CW_MAGNETIC_UNCALIBRATED:
        return ID_CW_MAGNETIC_UNCALIBRATED;
        break;
    case CW_GYROSCOPE_UNCALIBRATED:
        return ID_CW_GYROSCOPE_UNCALIBRATED;
        break;
    case CW_GAME_ROTATION_VECTOR:
        return ID_CW_GAME_ROTATION_VECTOR;
        break;
    case CW_GEOMAGNETIC_ROTATION_VECTOR:
        return ID_CW_GEOMAGNETIC_ROTATION_VECTOR;
        break;
    case CW_LIGHT:
        return ID_L;
        break;
    case CW_STEP_DETECTOR:
        return ID_CW_STEP_DETECTOR;
        break;
    case CW_STEP_COUNTER:
        return ID_CW_STEP_COUNTER;
        break;
    case HTC_WAKE_UP_GESTURE:
        return ID_WAKE_UP_GESTURE;
        break;
    default:
        return 0xFF;
        break;
    }
}

int CwMcuSensor::find_sensor(int32_t handle) {
    int what = -1;
    switch (handle) {
    case ID_A:
        what = CW_ACCELERATION;
        break;
    case ID_M:
        what = CW_MAGNETIC;
        break;
    case ID_GY:
        what = CW_GYRO;
        break;
    case ID_PS:
        what = CW_PRESSURE;
        break;
    case ID_O:
        what = CW_ORIENTATION;
        break;
    case ID_RV:
        what = CW_ROTATIONVECTOR;
        break;
    case ID_LA:
        what = CW_LINEARACCELERATION;
        break;
    case ID_G:
        what = CW_GRAVITY;
        break;
    case ID_CW_MAGNETIC_UNCALIBRATED:
        what = CW_MAGNETIC_UNCALIBRATED;
        break;
    case ID_CW_GYROSCOPE_UNCALIBRATED:
        what = CW_GYROSCOPE_UNCALIBRATED;
        break;
    case ID_CW_GAME_ROTATION_VECTOR:
        what = CW_GAME_ROTATION_VECTOR;
        break;
    case ID_CW_GEOMAGNETIC_ROTATION_VECTOR:
        what = CW_GEOMAGNETIC_ROTATION_VECTOR;
        break;
    case ID_CW_SIGNIFICANT_MOTION:
        what = CW_SIGNIFICANT_MOTION;
        break;
    case ID_CW_STEP_DETECTOR:
        what = CW_STEP_DETECTOR;
        break;
    case ID_CW_STEP_COUNTER:
        what = CW_STEP_COUNTER;
        break;
    case ID_L:
        what = CW_LIGHT;
        break;
    case ID_WAKE_UP_GESTURE:
        what = HTC_WAKE_UP_GESTURE;
        break;
    }
    return what;
}

int CwMcuSensor::getEnable(int32_t handle) {
    ALOGD("CwMcuSensor::getEnable: handle = %d\n", handle);
    return  0;
}

static int min(int a, int b) {
    return (a < b) ? a : b;
}

int CwMcuSensor::setEnable(int32_t handle, int en) {

    int what;
    int err = 0;
    int flags = !!en;
    int fd;
    char buf[10];
    int temp_data[COMPASS_CALIBRATION_DATA_SIZE];
    char value[PROPERTY_VALUE_MAX] = {0};
    int rc;

    ALOGW("%s: Before pthread_mutex_lock()\n", __func__);
    pthread_mutex_lock(&sys_fs_mutex);
    ALOGW("%s: Acquired pthread_mutex_lock()\n", __func__);

    property_get("debug.sensorhal.fill.block", value, "0");
    ALOGD("CwMcuSensor::setEnable: debug.sensorhal.fill.block= %s", value);
    if (atoi(value) == 1) {
        fill_block_debug = 1;
    } else {
        fill_block_debug = 0;
    }

    what = find_sensor(handle);

    ALOGD("CwMcuSensor::setEnable: [v02-Add Step Detector and Step Counter], handle = %d, en = %d,"
          " what = %d\n", handle, en, what);

    if (uint32_t(what) >= numSensors) {
        pthread_mutex_unlock(&sys_fs_mutex);
        return -EINVAL;
    }
    strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "enable");
    fd = open(fixed_sysfs_path, O_RDWR);
    if (fd >= 0) {
        int n = snprintf(buf, sizeof(buf), "%d %d\n", what, flags);
        err = write(fd, buf, min(n, sizeof(buf)));
        if (err < 0) {
            ALOGE("%s: write failed: %s", __func__, strerror(errno));
        }

        close(fd);

        mEnabled &= ~(1<<what);
        mEnabled |= (uint32_t(flags)<<what);

        if (!mEnabled) {
            if (sysfs_set_input_attr_by_int("buffer/enable", 0) < 0) {
                ALOGE("CwMcuSensor::setEnable: set buffer disable failed: %s\n", strerror(errno));
            } else {
                ALOGI("CwMcuSensor::setEnable: set IIO buffer enable = 0\n");
            }
        }
    } else {
        ALOGE("%s open failed: %s", __func__, strerror(errno));
    }

    // Sensor Calibration init. Waiting for firmware ready
    if (((what == CW_MAGNETIC) && (flags == 0)) ||
            ((what == CW_ORIENTATION) && (flags == 0)) ||
            ((what == CW_ROTATIONVECTOR) && (flags == 0))
       ) {
        ALOGD("Save Compass calibration data");
        strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "calibrator_data_mag");
        rc = cw_read_calibrator_file(CW_MAGNETIC, fixed_sysfs_path, temp_data);
        if (rc== 0) {
            cw_save_calibrator_file(CW_MAGNETIC, SAVE_PATH_MAG, temp_data);
        } else {
            ALOGI("Compass calibration data from driver fails\n");
        }
    }

    pthread_mutex_unlock(&sys_fs_mutex);
    return 0;
}

int CwMcuSensor::batch(int handle, int flags, int64_t period_ns, int64_t timeout)
{
    int what;
    int fd;
    char buf[32] = {0};
    int err;
    int delay_ms;
    int timeout_ms;
    bool dryRun = false;

    ALOGD("CwMcuSensor::batch++: handle = %d, flags = %d, period_ns = %lld, timeout = %lld\n",
        handle, flags, period_ns, timeout);

    what = find_sensor(handle);
    delay_ms = period_ns/NS_PER_MS;
    timeout_ms = timeout/NS_PER_MS;

    if(flags & SENSORS_BATCH_DRY_RUN) {
        dryRun = true;
    }

    if (uint32_t(what) >= CW_SENSORS_ID_END) {
        return -EINVAL;
    }

    if (flags == SENSORS_BATCH_WAKE_UPON_FIFO_FULL) {
        ALOGD("CwMcuSensor::batch: SENSORS_BATCH_WAKE_UPON_FIFO_FULL~!!\n");
    }

    switch (what) {
    case CW_LIGHT:
    case CW_SIGNIFICANT_MOTION:
        if (timeout > 0) {
            ALOGI("CwMcuSensor::batch: handle = %d, not support batch mode", handle);
            return -EINVAL;
        }
        break;
    default:
        break;
    }

    if (dryRun == true) {
        ALOGI("CwMcuSensor::batch: SENSORS_BATCH_DRY_RUN is set\n");
        return 0;
    }

    ALOGW("%s: Before pthread_mutex_lock()\n", __func__);
    pthread_mutex_lock(&sys_fs_mutex);
    ALOGW("%s: Acquired pthread_mutex_lock()\n", __func__);

    if (!mEnabled) {
        if (sysfs_set_input_attr_by_int("buffer/length", IIO_MAX_BUFF_SIZE) < 0) {
            ALOGE("CwMcuSensor::batch: set IIO buffer length failed: %s\n", strerror(errno));
        } else {
            ALOGI("CwMcuSensor::batch: set IIO buffer length = %d\n", IIO_MAX_BUFF_SIZE);
        }

        if (!init_trigger_done) {
            err = sysfs_set_input_attr("trigger/current_trigger",
                                      mTriggerName, strlen(mTriggerName));
            if (err < 0) {
                ALOGE("CwMcuSensor::batch: set current trigger failed: err = %d, strerr() = %s\n",
                      err, strerror(errno));
            } else {
                init_trigger_done = true;
            }
        }

        if (sysfs_set_input_attr_by_int("buffer/enable", 1) < 0) {
            ALOGE("CwMcuSensor::batch: set IIO buffer enable failed: %s\n", strerror(errno));
        } else {
            ALOGI("CwMcuSensor::batch: set IIO buffer enable = 1\n");
        }
    }

    sync_timestamp_locked();

    strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "batch_enable");

    fd = open(fixed_sysfs_path, O_RDWR);
    if (fd < 0) {
        err = -errno;
    } else {
        int n = snprintf(buf, sizeof(buf), "%d %d %d %d\n", what, flags, delay_ms, timeout_ms);
        err = write(fd, buf, min(n, sizeof(buf)));
        if (err < 0) {
            err = -errno;
        } else {
            err = 0;
        }
        close(fd);
    }
    pthread_mutex_unlock(&sys_fs_mutex);

    ALOGD("CwMcuSensor::batch: fd = %d, sensors_id = %d, flags = %d, delay_ms= %d,"
          " timeout_ms = %d, path = %s, err = %d\n",
          fd , what, flags, delay_ms, timeout_ms, fixed_sysfs_path, err);

    return err;
}


int CwMcuSensor::flush(int handle)
{
    int what;
    int fd;
    char buf[10] = {0};
    int err;

    what = find_sensor(handle);
    mFlushSensorEnabled = handle;

    if (uint32_t(what) >= CW_SENSORS_ID_END) {
        return -EINVAL;
    }

    ALOGW("%s: Before pthread_mutex_lock()\n", __func__);
    pthread_mutex_lock(&sys_fs_mutex);
    ALOGW("%s: Acquired pthread_mutex_lock()\n", __func__);

    strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "flush");

    fd = open(fixed_sysfs_path, O_RDWR);
    if (fd >= 0) {
        int n = snprintf(buf, sizeof(buf), "%d\n", what);
        err = write(fd, buf, min(n, sizeof(buf)));
        if (err < 0) {
            err = -errno;
        } else {
            err = 0;
        }
        close(fd);
    } else {
        ALOGI("CwMcuSensor::flush: flush not supported\n");
        err = -EINVAL;
    }

    pthread_mutex_unlock(&sys_fs_mutex);
    ALOGD("CwMcuSensor::flush: fd = %d, sensors_id = %d, path = %s, err = %d\n",
          fd, what, fixed_sysfs_path, err);
    return err;
}


int CwMcuSensor::sync_timestamp_locked(void) {
    int fd;
    char buf[10] = {0};
    int err;

    strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "flush");

    fd = open(fixed_sysfs_path, O_RDWR);
    if (fd >= 0) {
        size_t n = snprintf(buf, sizeof(buf), "%d\n", TIMESTAMP_SYNC_CODE);
        err = write(fd, buf, min(n, sizeof(buf)));
        close(fd);
        if (err < 0) {
            err = -EIO;
        } else {
            l_timestamp = getTimestamp();
            err = 0;
        }
    } else {
        err = -ENOENT;
    }
    return err;
}

int CwMcuSensor::sync_timestamp(void)
{
    int err;

    ALOGW("%s: Before pthread_mutex_lock()\n", __func__);
    pthread_mutex_lock(&sys_fs_mutex);
    ALOGW("%s: Acquired pthread_mutex_lock()\n", __func__);

    err = sync_timestamp_locked();

    pthread_mutex_unlock(&sys_fs_mutex);

    ALOGD("CwMcuSensor::sync_timestamp: path = %s, err = %d\n", fixed_sysfs_path, err);

    return err;
}

bool CwMcuSensor::hasPendingEvents() const {
    return mPendingMask;
}

int CwMcuSensor::setDelay(int32_t handle, int64_t delay_ns) {
    char buf[80];
    int fd;
    int what;
    int rc;

    ALOGW("%s: Before pthread_mutex_lock()\n", __func__);
    pthread_mutex_lock(&sys_fs_mutex);
    ALOGW("%s: Acquired pthread_mutex_lock()\n", __func__);

    ALOGD("CwMcuSensor::setDelay: handle = %d, delay_ns = %lld\n", handle, delay_ns);

    what = find_sensor(handle);
    if (uint32_t(what) >= numSensors) {
        pthread_mutex_unlock(&sys_fs_mutex);
        return -EINVAL;
    }
    strcpy(&fixed_sysfs_path[fixed_sysfs_path_len], "delay_ms");
    fd = open(fixed_sysfs_path, O_RDWR);
    if (fd >= 0) {
        size_t n = snprintf(buf, sizeof(buf), "%d %lld\n", what, (delay_ns/NS_PER_MS));
        write(fd, buf, min(n, sizeof(buf)));
        close(fd);
    }

    pthread_mutex_unlock(&sys_fs_mutex);
    return 0;

}

void CwMcuSensor::calculate_rv_4th_element(int sensors_id) {
    switch (sensors_id) {
    case CW_ROTATIONVECTOR:
    case CW_GAME_ROTATION_VECTOR:
    case CW_GEOMAGNETIC_ROTATION_VECTOR:
        float q0, q1, q2, q3;

        q1 = mPendingEvents[sensors_id].data[0];
        q2 = mPendingEvents[sensors_id].data[1];
        q3 = mPendingEvents[sensors_id].data[2];

        q0 = 1 - q1*q1 - q2*q2 - q3*q3;
        q0 = (q0 > 0) ? (float)sqrt(q0) : 0;

        mPendingEvents[sensors_id].data[3] = q0;
        break;
    default:
        break;
    }
}

int CwMcuSensor::readEvents(sensors_event_t* data, int count) {
    int64_t mtimestamp = getTimestamp();

    if (count < 1) {
        return -EINVAL;
    }

    ALOGD_IF(fill_block_debug == 1, "CwMcuSensor::readEvents: Before fill\n");
    ssize_t n = mInputReader.fill(data_fd);
    ALOGD_IF(fill_block_debug == 1, "CwMcuSensor::readEvents: After fill, n = %d\n", n);
    if (n < 0) {
        return n;
    }

    cw_event const* event;
    uint8_t data_temp[24];
    int id;
    int numEventReceived = 0;

    while (count && mInputReader.readEvent(&event)) {

        memcpy(data_temp, event->data, sizeof(data_temp));

        id = processEvent(data_temp);
        if (id == CW_META_DATA) {
            *data++ = mPendingEventsFlush;
            count--;
            numEventReceived++;
            ALOGI("CwMcuSensor::readEvents: metadata = %d\n", mPendingEventsFlush.meta_data.sensor);
        } else {
            mPendingEvents[id].timestamp = getTimestamp();
            if (mEnabled & (1<<id)) {
                if (id == CW_SIGNIFICANT_MOTION)
                    setEnable(ID_CW_SIGNIFICANT_MOTION, 0);
                calculate_rv_4th_element(id);
                *data++ = mPendingEvents[id];
                count--;
                numEventReceived++;
            }
        }

        mInputReader.next();
    }
    return numEventReceived;
}


int CwMcuSensor::processEvent(uint8_t *event) {
    int sensorsid = 0;
    int16_t data[3];
    int16_t bias[3];
    int64_t time;

    sensorsid = (int)event[0];
    memcpy(data, &event[1], 6);
    memcpy(bias, &event[7], 6);
    memcpy(&time, &event[13], 8);

    mPendingEvents[sensorsid].timestamp = time;

    switch (sensorsid) {
    case CW_ORIENTATION:
        mPendingMask |= 1<<sensorsid;
        if (sensorsid == CW_ORIENTATION) {
            mPendingEvents[sensorsid].orientation.status = bias[0];
        }
        mPendingEvents[sensorsid].data[0] = (float)data[0] * CONVERT_10;
        mPendingEvents[sensorsid].data[1] = (float)data[1] * CONVERT_10;
        mPendingEvents[sensorsid].data[2] = (float)data[2] * CONVERT_10;
        break;
    case CW_ACCELERATION:
    case CW_MAGNETIC:
    case CW_GYRO:
    case CW_LINEARACCELERATION:
    case CW_GRAVITY:
        mPendingMask |= 1<<sensorsid;
        if (sensorsid == CW_MAGNETIC) {
            mPendingEvents[sensorsid].magnetic.status = bias[0];
            ALOGD("CwMcuSensor::processEvent: magnetic accuracy = %d\n", mPendingEvents[sensorsid].magnetic.status);
        }
        mPendingEvents[sensorsid].data[0] = (float)data[0] * CONVERT_100;
        mPendingEvents[sensorsid].data[1] = (float)data[1] * CONVERT_100;
        mPendingEvents[sensorsid].data[2] = (float)data[2] * CONVERT_100;
        break;
    case CW_PRESSURE:
        mPendingMask |= 1<<sensorsid;
        // .pressure is data[0] and the unit is hectopascal (hPa)
        mPendingEvents[sensorsid].pressure = ((float)*(int32_t *)(&data[0])) * CONVERT_100;
        // data[1] is not used, and data[2] is the temperature
        mPendingEvents[sensorsid].data[2] = ((float)data[2]) * CONVERT_100;
        break;
    case CW_ROTATIONVECTOR:
    case CW_GAME_ROTATION_VECTOR:
    case CW_GEOMAGNETIC_ROTATION_VECTOR:
        mPendingMask |= 1<<sensorsid;
        mPendingEvents[sensorsid].data[0] = (float)data[0] * CONVERT_10000;
        mPendingEvents[sensorsid].data[1] = (float)data[1] * CONVERT_10000;
        mPendingEvents[sensorsid].data[2] = (float)data[2] * CONVERT_10000;
        break;
    case CW_MAGNETIC_UNCALIBRATED:
    case CW_GYROSCOPE_UNCALIBRATED:
        mPendingMask |= 1<<sensorsid;
        mPendingEvents[sensorsid].data[0] = (float)data[0] * CONVERT_100;
        mPendingEvents[sensorsid].data[1] = (float)data[1] * CONVERT_100;
        mPendingEvents[sensorsid].data[2] = (float)data[2] * CONVERT_100;
        mPendingEvents[sensorsid].data[3] = (float)bias[0] * CONVERT_100;
        mPendingEvents[sensorsid].data[4] = (float)bias[1] * CONVERT_100;
        mPendingEvents[sensorsid].data[5] = (float)bias[2] * CONVERT_100;
        break;
    case CW_SIGNIFICANT_MOTION:
        mPendingMask |= 1<<(sensorsid);
        mPendingEvents[sensorsid].data[0] = (float)data[0];
        mPendingEvents[sensorsid].data[1] = (float)data[1];
        mPendingEvents[sensorsid].data[2] = (float)data[2];
        ALOGI("sensors_id = %d, data = %d", sensorsid, data);
        break;
    case CW_LIGHT:
        mPendingMask |= 1<<(sensorsid);
        mPendingEvents[sensorsid].light = indexToValue(data[0]);
        break;
    case CW_STEP_DETECTOR:
        mPendingMask |= 1<<(sensorsid);
        mPendingEvents[CW_STEP_COUNTER].data[0] = data[0];
        mPendingEvents[CW_STEP_DETECTOR].timestamp = getTimestamp();
        break;
    case CW_STEP_COUNTER:
        mPendingMask |= 1<<(sensorsid);
        mPendingEvents[CW_STEP_COUNTER].u64.step_counter = *(uint32_t *)&data[0]; // We use 4 bytes in SensorHUB
        break;
    case HTC_WAKE_UP_GESTURE:
        mPendingMask |= 1<<(sensorsid);
        mPendingEvents[HTC_WAKE_UP_GESTURE].data[0] = 1;
        ALOGI("HTC_WAKE_UP_GESTURE occurs\n");
        break;
    case CW_META_DATA:
        mPendingEventsFlush.meta_data.what = META_DATA_FLUSH_COMPLETE;
        mPendingEventsFlush.meta_data.sensor = find_handle(data[0]);
        ALOGI("CW_META_DATA: meta_data.sensor = %d\n", mPendingEventsFlush.meta_data.sensor);
        break;
    case CW_SYNC_ACK:
        if (data[0] == SYNC_ACK_MAGIC) {
            ALOGI("processEvent: g_timestamp = l_timestamp = %llu\n", l_timestamp);
            g_timestamp = l_timestamp;
        }
        break;
    case TIME_DIFF_EXHAUSTED:
        ALOGI("processEvent: data[0] = 0x%x\n", data[0]);
        if (data[0] == EXHAUSTED_MAGIC) {
            ALOGI("processEvent: TIME_DIFF_EXHAUSTED\n");
            sync_timestamp();
        }
        break;
    default:
        ALOGW("%s: Unknown sensorsid = %d\n", __func__, sensorsid);
        break;
    }

    return sensorsid;
}


void CwMcuSensor::cw_save_calibrator_file(int type, const char * path, int* str) {
    FILE *fp_file;
    int i;
    int rc;

    ALOGD("CwMcuSensor::cw_save_calibrator_file: path = %s\n", path);

    fp_file = fopen(path, "w+");
    if (!fp_file) {
        ALOGE("CwMcuSensor::cw_save_calibrator_file: open file '%s' failed: %s\n",
              path, strerror(errno));
        return;
    }

    if ((type == CW_GYRO) || (type == CW_ACCELERATION)) {
        fprintf(fp_file, "%d %d %d\n", str[0], str[1], str[2]);
    } else if(type == CW_MAGNETIC) {
        for (i = 0; i < COMPASS_CALIBRATION_DATA_SIZE; i++) {
            ALOGD("CwMcuSensor::cw_save_calibrator_file: str[%d] = %d\n", i, str[i]);
            rc = fprintf(fp_file, "%d%c", str[i], (i == (COMPASS_CALIBRATION_DATA_SIZE-1)) ? '\n' : ' ');
            if (rc < 0) {
                ALOGE("CwMcuSensor::cw_save_calibrator_file: fprintf fails, rc = %d\n", rc);
            }
        }
    }

    fclose(fp_file);
    return;
}

int CwMcuSensor::cw_read_calibrator_file(int type, const char * path, int* str) {
    FILE *fp;
    int readBytes;
    int data[COMPASS_CALIBRATION_DATA_SIZE] = {0};
    unsigned int i;
    int my_errno;

    ALOGD("CwMcuSensor::cw_read_calibrator_file: path = %s\n", path);

    fp = fopen(path, "r");
    if (!fp) {
        ALOGE("CwMcuSensor::cw_read_calibrator_file: open file '%s' failed: %s\n",
              path, strerror(errno));
        // errno is reset to 0 before return
        return -1;
    }

    if (type == CW_GYRO || type == CW_ACCELERATION) {
        readBytes = fscanf(fp, "%d %d %d\n", &str[0], &str[1], &str[2]);
        my_errno = errno;
        if (readBytes != 3) {
            ALOGE("CwMcuSensor::cw_read_calibrator_file: fscanf3, readBytes = %d, strerror = %s\n", readBytes, strerror(my_errno));
        }

    } else if (type == CW_MAGNETIC) {
        ALOGD("CwMcuSensor::cw_read_calibrator_file: COMPASS_CALIBRATION_DATA_SIZE = %d\n", COMPASS_CALIBRATION_DATA_SIZE);
        // COMPASS_CALIBRATION_DATA_SIZE is 26
        for (i = 0; i < COMPASS_CALIBRATION_DATA_SIZE; i++) {
            readBytes = fscanf(fp, "%d ", &str[i]);
            my_errno = errno;
            ALOGD("CwMcuSensor::cw_read_calibrator_file: str[%d] = %d\n", i, str[i]);
            if (readBytes < 1) {
                ALOGE("CwMcuSensor::cw_read_calibrator_file: fscanf26, readBytes = %d, strerror = %s\n", readBytes, strerror(my_errno));
                fclose(fp);
                return readBytes;
            }
        }
    }
    fclose(fp);
    return 0;
}

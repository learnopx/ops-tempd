/*
 * (c) Copyright 2015 Hewlett Packard Enterprise Development LP
 *
 *    Licensed under the Apache License, Version 2.0 (the "License"); you may
 *    not use this file except in compliance with the License. You may obtain
 *    a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *    License for the specific language governing permissions and limitations
 *    under the License.
 */

/************************************************************************//**
 * @ingroup ops-tempd
 *
 * @file
 * Source file for the platform Temperature daemon
 *
 * @copyright Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * All Rights Reserved.
 * -
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 * -
 *     http://www.apache.org/licenses/LICENSE-2.0
 * -
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ***************************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dynamic-string.h>

#include "config.h"
#include "command-line.h"
#include "compiler.h"
#include "daemon.h"
#include "dirs.h"
#include "dummy.h"
#include "fatal-signal.h"
#include "ovsdb-idl.h"
#include "poll-loop.h"
#include "simap.h"
#include "stream-ssl.h"
#include "stream.h"
#include "svec.h"
#include "timeval.h"
#include "unixctl.h"
#include "util.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "vswitch-idl.h"
#include "coverage.h"
#include "config-yaml.h"
#include "tempd.h"
#include "eventlog.h"

static struct ovsdb_idl *idl;

static unsigned int idl_seqno;

static unixctl_cb_func tempd_unixctl_dump;

static bool cur_hw_set = false;

YamlConfigHandle yaml_handle;

struct shash sensor_data;       // struct locl_sensor (all sensors)
struct shash subsystem_data;    // struct locl_subsystem

// map sensorstatus enum to the equivalent string
static const char *
sensor_status_to_string(enum sensorstatus status)
{
    VLOG_DBG("sensor status is %d", status);
    if (status < sizeof(sensor_status)/sizeof(const char *)) {
        VLOG_DBG("sensor status is %s", sensor_status[status]);
        return(sensor_status[status]);
    } else {
        VLOG_DBG("sensor status is %s", sensor_status[SENSOR_STATUS_UNINITIALIZED]);
        return(sensor_status[SENSOR_STATUS_UNINITIALIZED]);
    }
}

// map fanspeed enum to the equivalent string
static const char *
sensor_speed_to_string(enum fanspeed speed)
{
    if (speed < sizeof(fan_speed)/sizeof(const char *)) {
        return(fan_speed[speed]);
    } else {
        return(fan_speed[SENSOR_FAN_NORMAL]);
    }
}

// initialize the subsystem and global sensor dictionaries
static void
init_subsystems(void)
{
    shash_init(&subsystem_data);
    shash_init(&sensor_data);
}

// find a sensor (in idl cache) by name
// used for mapping existing db object to yaml object
static struct ovsrec_temp_sensor *
lookup_sensor(const char *name)
{
    const struct ovsrec_temp_sensor *sensor;

    OVSREC_TEMP_SENSOR_FOR_EACH(sensor, idl) {
        if (strcmp(sensor->name, name) == 0) {
            return((struct ovsrec_temp_sensor *)sensor);
        }
    }

    return(NULL);
}

// read the lm75 temperature sensor
// lm75 has a two-byte temperature output. The first byte is the temperature,
// and the second byte's highest bit is a half-degree adder
static void
lm75_read(struct locl_sensor *sensor)
{
    char buf[2];
    int rc;
    bool fault = false;

    const YamlDevice *device = yaml_find_device(
            yaml_handle,
            sensor->subsystem->name,
            sensor->yaml_sensor->device);

    if (sensor->test_temp != -1) {
        VLOG_DBG("Test temperature override set to %d", sensor->test_temp);
        sensor->status = SENSOR_STATUS_NORMAL;
        sensor->temp = sensor->test_temp;
        return;
    }

    rc = i2c_data_read(yaml_handle, device, sensor->subsystem->name, 0,
                       sizeof(buf), buf);

    if (0 != rc) {
        fault = true;
    }

    if (true == fault) {
        // if we've hit the retry limit, mark it as failed
        if (sensor->fault_count > MAX_FAIL_RETRY) {
            sensor->status = SENSOR_STATUS_FAILED;
        }
        // otherwise, don't change the temp or status, but increment the retry
        // count
        sensor->fault_count++;
        return;
    }

    // if we succeeded in reading the temp, then clear the retry count
    sensor->fault_count = 0;

    if (sensor->status == SENSOR_STATUS_FAILED) {
        // we need to kick this sensor back into a working state
        sensor->status = SENSOR_STATUS_NORMAL;
    }

    // convert to milidegrees (C)
    sensor->temp = buf[0] * MILI_DEGREES;

    // high bit in second byte is half-degree indicator
    if (buf[1] < 0) {
        // half-degree in milidegrees
        sensor->temp += 500;
    }

    VLOG_DBG("%s: %4.1fc", sensor->yaml_sensor->device, ((float)sensor->temp)/MILI_DEGREES_FLOAT);
}

// read sensor temperature and calculate status/fan speed setting
static void
tempd_read_sensor(struct locl_sensor *sensor)
{
    const YamlSensor *yaml_sensor = sensor->yaml_sensor;

    if (strcmp(yaml_sensor->type, "lm75") == 0) {
        lm75_read(sensor);
    } else {
        VLOG_WARN("Unrecognized sensor type %s", yaml_sensor->type);
        log_event("TEMP_SENSOR_UNRECOGNIZED", EV_KV("type",
            "%s", yaml_sensor->type));
        sensor->temp = DEFAULT_TEMP * MILI_DEGREES;
    }

    // recalculate alarm and fan state

    // decreasing alarms

    if (SENSOR_STATUS_FAILED == sensor->status) {
        // no temp to report, unable to read sensor
        return;
    }

    // adjust min and max values
    if (sensor->min > sensor->temp) {
        sensor->min = sensor->temp;
    }

    if (sensor->max < sensor->temp) {
        sensor->max = sensor->temp;
    }

    // decreasing alarms
    if (SENSOR_STATUS_EMERGENCY == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->alarm_thresholds.emergency_off) {
        sensor->status = SENSOR_STATUS_CRITICAL;
    }

    if (SENSOR_STATUS_CRITICAL == sensor->status &&
            (float)sensor->temp /MILI_DEGREES_FLOAT<= yaml_sensor->alarm_thresholds.critical_off) {
        sensor->status = SENSOR_STATUS_MAX;
    }

    if (SENSOR_STATUS_MAX == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->alarm_thresholds.max_off) {
        sensor->status = SENSOR_STATUS_NORMAL;
    }

    if (SENSOR_STATUS_NORMAL == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT > yaml_sensor->alarm_thresholds.low_crit) {
        sensor->status = SENSOR_STATUS_MIN;
    }

    if (SENSOR_STATUS_MIN == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT > yaml_sensor->alarm_thresholds.min) {
        sensor->status = SENSOR_STATUS_NORMAL;
    }

    // increasing alarms
    if (SENSOR_STATUS_NORMAL == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT >= yaml_sensor->alarm_thresholds.max_on) {
        sensor->status = SENSOR_STATUS_MAX;
    }

    if (SENSOR_STATUS_MAX == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT >= yaml_sensor->alarm_thresholds.critical_on) {
        sensor->status = SENSOR_STATUS_CRITICAL;
    }

    if (SENSOR_STATUS_CRITICAL == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT >= yaml_sensor->alarm_thresholds.emergency_on) {
        sensor->status = SENSOR_STATUS_EMERGENCY;
    }

    if (SENSOR_STATUS_NORMAL == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->alarm_thresholds.min) {
        sensor->status = SENSOR_STATUS_MIN;
    }

    if (SENSOR_STATUS_MIN == sensor->status &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->alarm_thresholds.low_crit) {
        sensor->status = SENSOR_STATUS_LOWCRIT;
    }

    // calculate requested fan speed
    if (SENSOR_FAN_NORMAL == sensor->fan_speed &&
            (float)sensor->temp/MILI_DEGREES_FLOAT >= yaml_sensor->fan_thresholds.medium_on) {
        sensor->fan_speed = SENSOR_FAN_MEDIUM;
    }

    if (SENSOR_FAN_MEDIUM == sensor->fan_speed &&
            (float)sensor->temp/MILI_DEGREES_FLOAT >= yaml_sensor->fan_thresholds.fast_on) {
        sensor->fan_speed = SENSOR_FAN_FAST;
    }

    if (SENSOR_FAN_FAST == sensor->fan_speed &&
            (float)sensor->temp/MILI_DEGREES_FLOAT >= yaml_sensor->fan_thresholds.max_on) {
        sensor->fan_speed = SENSOR_FAN_MAX;
    }

    if (SENSOR_FAN_MAX == sensor->fan_speed &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->fan_thresholds.max_off) {
        sensor->fan_speed = SENSOR_FAN_FAST;
    }

    if (SENSOR_FAN_FAST == sensor->fan_speed &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->fan_thresholds.fast_off) {
        sensor->fan_speed = SENSOR_FAN_MEDIUM;
    }

    if (SENSOR_FAN_MEDIUM == sensor->fan_speed &&
            (float)sensor->temp/MILI_DEGREES_FLOAT <= yaml_sensor->fan_thresholds.medium_off) {
        sensor->fan_speed = SENSOR_FAN_NORMAL;
    }
}

// create a new locl_subsystem object
static struct locl_subsystem *
add_subsystem(const struct ovsrec_subsystem *ovsrec_subsys)
{
    struct locl_subsystem *result;
    int rc;
    int idx;
    struct ovsdb_idl_txn *txn;
    struct ovsrec_temp_sensor **sensor_array;
    int sensor_idx;
    int sensor_count;
    const char *dir;
    const YamlThermalInfo *info;

    // create and initialize basic subsystem information
    VLOG_DBG("Adding new subsystem %s", ovsrec_subsys->name);
    result = (struct locl_subsystem *)malloc(sizeof(struct locl_subsystem));
    memset(result, 0, sizeof(struct locl_subsystem));
    (void)shash_add(&subsystem_data, ovsrec_subsys->name, (void *)result);
    result->name = strdup(ovsrec_subsys->name);
    result->marked = false;
    result->marked = true;
    result->parent_subsystem = NULL;  // OPS_TODO: find parent subsystem
    shash_init(&result->subsystem_sensors);

    // use a default if the hw_desc_dir has not been populated
    dir = ovsrec_subsys->hw_desc_dir;

    if (dir == NULL || strlen(dir) == 0) {
        VLOG_ERR("No h/w description directory for subsystem %s",
                                        ovsrec_subsys->name);
        return(NULL);
    }

    // since this is a new subsystem, load all of the hardware description
    // information about devices and sensors (just for this subsystem).
    // parse sensors and device data for subsystem
    rc = yaml_add_subsystem(yaml_handle, ovsrec_subsys->name, dir);

    if (rc != 0) {
        VLOG_ERR("Error reading h/w description files for subsystem %s",
                                        ovsrec_subsys->name);
        return(NULL);
    }

    // need devices data
    rc = yaml_parse_devices(yaml_handle, ovsrec_subsys->name);

    if (rc != 0) {
        VLOG_ERR("Unable to parse subsystem %s devices file (in %s)",
                                        ovsrec_subsys->name, dir);
        return(NULL);
    }

    // need thermal (sensor) data
    rc = yaml_parse_thermal(yaml_handle, ovsrec_subsys->name);

    if (rc != 0) {
        VLOG_ERR("Unable to parse subsystem %s thermal file (in %s)",
                                        ovsrec_subsys->name, dir);
        return(NULL);
    }

    // get the thermal info, need it for shutdown flag
    info = yaml_get_thermal_info(yaml_handle, ovsrec_subsys->name);
    result->emergency_shutdown = info->auto_shutdown;

    // OPS_TODO: the thermal info has a polling period, but when we
    // OPS_TODO: have multiple subsystems, that could be tricky to
    // OPS_TODO: implement if there are different polling periods.
    // OPS_TODO: For now, hardware the polling period to 5 seconds.

    // prepare to add sensors to db
    sensor_idx = 0;
    sensor_count = yaml_get_sensor_count(yaml_handle, ovsrec_subsys->name);

    if (sensor_count <= 0) {
        return(NULL);
    }

    result->valid = true;

    // subsystem db object has reference array for sensors
    sensor_array = (struct ovsrec_temp_sensor **)malloc(sensor_count * sizeof(struct ovsrec_temp_sensor *));
    memset(sensor_array, 0, sensor_count * sizeof(struct ovsrec_temp_sensor *));

    txn = ovsdb_idl_txn_create(idl);

    VLOG_DBG("There are %d sensors in subsystem %s", sensor_count, ovsrec_subsys->name);

    for (idx = 0; idx < sensor_count; idx++) {
        const YamlSensor *sensor = yaml_get_sensor(yaml_handle, ovsrec_subsys->name, idx);

        struct ovsrec_temp_sensor *ovs_sensor;
        char *sensor_name = NULL;
        struct locl_sensor *new_sensor;
        VLOG_DBG("Adding sensor %d (%s) in subsystem %s",
            sensor->number,
            sensor->location,
            ovsrec_subsys->name);

        // create a name for the sensor from the subsystem name and the
        // sensor number
        asprintf(&sensor_name, "%s-%d", ovsrec_subsys->name, sensor->number);
        // allocate and initialize basic sensor information
        new_sensor = (struct locl_sensor *)malloc(sizeof(struct locl_sensor));
        new_sensor->name = sensor_name;
        new_sensor->subsystem = result;
        new_sensor->yaml_sensor = sensor;
        new_sensor->min = 1000000;
        new_sensor->max = -1000000;
        new_sensor->temp = 0;
        new_sensor->status = SENSOR_STATUS_NORMAL;
        new_sensor->fan_speed = SENSOR_FAN_NORMAL;
        new_sensor->test_temp = -1;     // no test temperature override set

        // try to populate sensor information with real data
        tempd_read_sensor(new_sensor);

        // add sensor to subsystem sensor dictionary
        shash_add(&result->subsystem_sensors, sensor_name, (void *)new_sensor);
        // add sensor to global sensor dictionary
        shash_add(&sensor_data, sensor_name, (void *)new_sensor);

        // look for existing Temp_sensor rows
        ovs_sensor = lookup_sensor(sensor_name);

        if (ovs_sensor == NULL) {
            // existing sensor doesn't exist in db, create it
            ovs_sensor = ovsrec_temp_sensor_insert(txn);
        }

        // set initial data
        ovsrec_temp_sensor_set_name(ovs_sensor, sensor_name);
        ovsrec_temp_sensor_set_status(ovs_sensor,
            sensor_status_to_string(new_sensor->status));
        ovsrec_temp_sensor_set_temperature(ovs_sensor, new_sensor->temp);
        ovsrec_temp_sensor_set_min(ovs_sensor, new_sensor->min);
        ovsrec_temp_sensor_set_max(ovs_sensor, new_sensor->max);
        ovsrec_temp_sensor_set_fan_state(ovs_sensor,
            sensor_speed_to_string(new_sensor->fan_speed));
        ovsrec_temp_sensor_set_location(ovs_sensor, sensor->location);

        // add sensor to subsystem reference list
        sensor_array[sensor_idx++] = ovs_sensor;
    }

    ovsrec_subsystem_set_temp_sensors(ovsrec_subsys, sensor_array, sensor_count);
    // execute transaction
    ovsdb_idl_txn_commit_block(txn);
    ovsdb_idl_txn_destroy(txn);
    free(sensor_array);

    return(result);
}

static void
tempd_unixctl_test(struct unixctl_conn *conn, int argc OVS_UNUSED,
                    const char *argv[], void *aud OVS_UNUSED)
{
    struct locl_sensor *sensor;
    int temp;
    const char *fan_name = argv[1];
    struct shash_node *node;

    temp = atoi(argv[2]);

    // find the sensor structure
    node = shash_find(&sensor_data, fan_name);
    if (node == NULL) {
        unixctl_command_reply_error(conn, "Sensor does not exist");
        return;
    }
    sensor = (struct locl_sensor *)node->data;

    // set the override value
    // -1 = no override, milidegrees centigrade, otherwise
    sensor->test_temp = temp;
    unixctl_command_reply(conn, "Test temperature override set");
}

// initialize tempd process
static void
tempd_init(const char *remote)
{
    int retval;

    // initialize subsystems
    init_subsystems();

    // initialize the yaml handle
    yaml_handle = yaml_new_config_handle();

    // create connection to db
    idl = ovsdb_idl_create(remote, &ovsrec_idl_class, false, true);
    idl_seqno = ovsdb_idl_get_seqno(idl);
    ovsdb_idl_set_lock(idl, "ops_tempd");
    ovsdb_idl_verify_write_only(idl);

    // Register for daemon table.
    ovsdb_idl_add_table(idl, &ovsrec_table_daemon);
    ovsdb_idl_add_column(idl, &ovsrec_daemon_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_daemon_col_cur_hw);
    ovsdb_idl_omit_alert(idl, &ovsrec_daemon_col_cur_hw);

    ovsdb_idl_add_table(idl, &ovsrec_table_temp_sensor);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_location);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_location);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_temperature);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_temperature);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_min);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_min);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_max);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_max);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_status);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_status);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_name);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_temp_sensor_col_fan_state);
    ovsdb_idl_omit_alert(idl, &ovsrec_temp_sensor_col_fan_state);

    ovsdb_idl_add_table(idl, &ovsrec_table_subsystem);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_temp_sensors);
    ovsdb_idl_omit_alert(idl, &ovsrec_subsystem_col_temp_sensors);
    ovsdb_idl_add_column(idl, &ovsrec_subsystem_col_hw_desc_dir);
    ovsdb_idl_omit_alert(idl, &ovsrec_subsystem_col_hw_desc_dir);

    unixctl_command_register("ops-tempd/dump", "", 0, 0,
                             tempd_unixctl_dump, NULL);
    unixctl_command_register("ops-tempd/test", "sensor temp", 2, 2,
                             tempd_unixctl_test, NULL);

    retval = event_log_init("TEMPERATURE");
    if(retval < 0) {
        VLOG_ERR("Event log initialization failed for tempareture");
    }
}

// pre-exit shutdown processing
static void
tempd_exit(void)
{
    ovsdb_idl_destroy(idl);
}

// poll every sensor for new temperature and update db with any new results
static void
tempd_run__(void)
{
    struct ovsdb_idl_txn *txn;
    const struct ovsrec_temp_sensor *cfg;
    const struct ovsrec_daemon *db_daemon;
    struct shash_node *node;
    struct shash_node *sensor_node;
    struct locl_sensor *sensor;
    bool change = false;

    SHASH_FOR_EACH(node, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)node->data;
        SHASH_FOR_EACH(sensor_node, &subsystem->subsystem_sensors) {
            sensor = (struct locl_sensor *)sensor_node->data;
            tempd_read_sensor(sensor);
            if (sensor->status == SENSOR_STATUS_EMERGENCY) {
                // if we're in an emergency situation, verify that the sensor
                // was read correctly (by reading it again).
                tempd_read_sensor(sensor);
                if (sensor->status == SENSOR_STATUS_EMERGENCY) {
                    // if we're still in an emergency sitaution, and the
                    // subsystem indicates that we should shutdown, do so.
                    if (subsystem->emergency_shutdown == true) {
                        VLOG_WARN("Emergency shutdown initiated for sensor %s",
                                sensor->name);
                        log_event("TEMP_SENSOR_SHUTDOWN",
                            EV_KV("name", "%s", sensor->name));
                        system(EMERGENCY_POWEROFF);
                        // shouldn't continue
                        while (1) {
                            sleep(1000);
                        }
                    }
                }
            }
        }
    }

    txn = ovsdb_idl_txn_create(idl);
    OVSREC_TEMP_SENSOR_FOR_EACH(cfg, idl) {
        const char *status;
        node = shash_find(&sensor_data, cfg->name);
        if (node == NULL) {
            VLOG_WARN("unable to find matching sensor for %s", cfg->name);
            ovsrec_temp_sensor_set_status(
                cfg,
                sensor_status_to_string(SENSOR_STATUS_UNINITIALIZED));
            change = true;
            continue;
        }
        sensor = (struct locl_sensor *)node->data;

        // note: only apply changes - don't blindly set data

        // calculate and set status
        status = sensor_status_to_string(sensor->status);
        if (strcmp(status, cfg->status) != 0) {
            ovsrec_temp_sensor_set_status(cfg, status);
            change = true;
        }
        // set temperature
        if (cfg->temperature != sensor->temp) {
            ovsrec_temp_sensor_set_temperature(cfg, sensor->temp);
            change = true;
        }
        // set min
        if (cfg->min != sensor->min) {
            ovsrec_temp_sensor_set_min(cfg, sensor->min);
            change = true;
        }
        // set max
        if (cfg->max != sensor->max) {
            ovsrec_temp_sensor_set_max(cfg, sensor->max);
            change = true;
        }
        // calculate and set fan speed
        status = sensor_speed_to_string(sensor->fan_speed);
        if (strcmp(status, cfg->fan_state) != 0) {
            ovsrec_temp_sensor_set_fan_state(cfg, status);
            change = true;
        }
        // set location (note: should never change)
        if (strcmp(sensor->yaml_sensor->location, cfg->location) != 0) {
            ovsrec_temp_sensor_set_location(cfg, sensor->yaml_sensor->location);
            change = true;
        }
    }

    // If first time through, set cur_hw = 1
    if (!cur_hw_set) {
        OVSREC_DAEMON_FOR_EACH(db_daemon, idl) {
            if (strncmp(db_daemon->name, NAME_IN_DAEMON_TABLE,
                            strlen(NAME_IN_DAEMON_TABLE)) == 0) {
                ovsrec_daemon_set_cur_hw(db_daemon, (int64_t) 1);
                cur_hw_set = true;
                change = true;
                break;
            }
        }
    }

    // if a change was made, execute the transaction
    if (change == true) {
        ovsdb_idl_txn_commit_block(txn);
    }
    ovsdb_idl_txn_destroy(txn);
}

// lookup a local subsystem structure
// if it's not found, create a new one and initialize it
static struct locl_subsystem *
get_subsystem(const struct ovsrec_subsystem *ovsrec_subsys)
{
    void *ptr;
    struct locl_subsystem *result = NULL;

    ptr = shash_find_data(&subsystem_data, ovsrec_subsys->name);

    if (ptr == NULL) {
        // this subsystem has not been added, yet. Do that now.
        result = add_subsystem(ovsrec_subsys);
    } else {
        result = (struct locl_subsystem *)ptr;
        if (!result->valid) {
            result = NULL;
        }
    }

    return(result);
}

// set the "marked" value for each subsystem to false.
static void
tempd_unmark_subsystems(void)
{
    struct shash_node *node;

    SHASH_FOR_EACH(node, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)node->data;
        subsystem->marked = false;
    }
}

// delete all subsystems that haven't been marked
// this is a helper function for deleting subsystems that no longer exist
// in the DB
static void
tempd_remove_unmarked_subsystems(void)
{
    struct shash_node *node, *next;
    struct shash_node *temp_node, *temp_next;
    struct shash_node *global_node;

    SHASH_FOR_EACH_SAFE(node, next, &subsystem_data) {
        struct locl_subsystem *subsystem = node->data;

        if (subsystem->marked == false) {
            // also, delete all temp sensors in the subsystem
            SHASH_FOR_EACH_SAFE(temp_node, temp_next, &subsystem->subsystem_sensors) {
                struct locl_sensor *temp = (struct locl_sensor *)temp_node->data;
                // delete the sensor_data entry
                global_node = shash_find(&sensor_data, temp->name);
                shash_delete(&sensor_data, global_node);
                // delete the subsystem entry
                shash_delete(&subsystem->subsystem_sensors, temp_node);
                // free the allocated data
                free(temp->name);
                free(temp);
            }
            free(subsystem->name);
            free(subsystem);

            // delete the subsystem dictionary entry
            shash_delete(&subsystem_data, node);

            // OPS_TODO: need to remove subsystem yaml data
        }
    }
}

// process any changes to cached data
static void
tempd_reconfigure(struct ovsdb_idl *idl)
{
    const struct ovsrec_subsystem *subsys;
    unsigned int new_idl_seqno = ovsdb_idl_get_seqno(idl);

    COVERAGE_INC(tempd_reconfigure);

    if (new_idl_seqno == idl_seqno){
        return;
    }

    idl_seqno = new_idl_seqno;

    // handle any added or deleted subsystems
    tempd_unmark_subsystems();

    OVSREC_SUBSYSTEM_FOR_EACH(subsys, idl) {
        struct locl_subsystem *subsystem;
        // get_subsystem will create a new one if it was added
        subsystem = get_subsystem(subsys);
        if (subsystem == NULL) continue;
        subsystem->marked = true;
    }

    // remove any subsystems that are no longer present in the db
    tempd_remove_unmarked_subsystems();
}

// perform all of the per-loop processing
static void
tempd_run(void)
{
    ovsdb_idl_run(idl);

    if (ovsdb_idl_is_lock_contended(idl)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);

        VLOG_ERR_RL(&rl, "another ops-tempd process is running, "
                    "disabling this process until it goes away");

        return;
    } else if (!ovsdb_idl_has_lock(idl)) {
        return;
    }

    // handle changes to cache
    tempd_reconfigure(idl);
    // poll all sensors and report changes into db
    tempd_run__();

    daemonize_complete();
    vlog_enable_async();
    VLOG_INFO_ONCE("%s (OpenSwitch tempd) %s", program_name, VERSION);
}

// initialize periodic poll of sensors
static void
tempd_wait(void)
{
    ovsdb_idl_wait(idl);
    poll_timer_wait(POLLING_PERIOD * MSEC_PER_SEC);
}

static void
tempd_unixctl_dump(struct unixctl_conn *conn, int argc OVS_UNUSED,
                          const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct shash_node *snode;
    struct shash_node *tnode;

    ds_put_cstr(&ds, "Support Dump for Platform Temperature Daemon (ops-tempd)\n");

    SHASH_FOR_EACH(snode, &subsystem_data) {
        struct locl_subsystem *subsystem = (struct locl_subsystem *)snode->data;

        ds_put_format(&ds, "\nSubsystem: %s\n", subsystem->name);

        SHASH_FOR_EACH(tnode, &(subsystem->subsystem_sensors)) {
            struct locl_sensor *sensor = (struct locl_sensor *)tnode->data;

            ds_put_format(&ds, "\tSensor name: %s\n", sensor->name);
            ds_put_format(&ds, "\t\tLocation: %s\n",
                                        sensor->yaml_sensor->location);
            ds_put_format(&ds, "\t\tDevice name: %s\n",
                                        sensor->yaml_sensor->device);
            ds_put_format(&ds, "\t\tType: %s\n",
                                        sensor->yaml_sensor->type);
            ds_put_format(&ds, "\t\tStatus: %s\n",
                                sensor_status_to_string(sensor->status));
            ds_put_format(&ds, "\t\tFan speed: %s\n",
                                sensor_speed_to_string(sensor->fan_speed));
            ds_put_format(&ds, "\t\tTemperature: %d\n",
                                        sensor->temp / 1000);
            ds_put_format(&ds, "\t\tMin temp: %d\n", sensor->min / 1000);
            ds_put_format(&ds, "\t\tMax temp: %d\n", sensor->max / 1000);
            ds_put_format(&ds, "\t\tFault count: %d\n",
                                        sensor->fault_count);
            ds_put_format(&ds, "\t\tAlarm Thresholds: \n");
            ds_put_format(&ds, "\t\t\temergency_on: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.emergency_on);
            ds_put_format(&ds, "\t\t\temergency_off: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.emergency_off);
            ds_put_format(&ds, "\t\t\tcritical_on: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.critical_on);
            ds_put_format(&ds, "\t\t\tcritical_off: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.critical_off);
            ds_put_format(&ds, "\t\t\tmax_on: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.max_on);
            ds_put_format(&ds, "\t\t\tmax_off: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.max_off);
            ds_put_format(&ds, "\t\t\tmin: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.min);
            ds_put_format(&ds, "\t\t\tlow_crit: %.2f\n",
                        sensor->yaml_sensor->alarm_thresholds.low_crit);
            ds_put_format(&ds, "\t\tFan Thresholds: \n");
            ds_put_format(&ds, "\t\t\tmax_on: %.2f\n",
                        sensor->yaml_sensor->fan_thresholds.max_on);
            ds_put_format(&ds, "\t\t\tmax_off: %.2f\n",
                        sensor->yaml_sensor->fan_thresholds.max_off);
            ds_put_format(&ds, "\t\t\tfast_on: %.2f\n",
                        sensor->yaml_sensor->fan_thresholds.fast_on);
            ds_put_format(&ds, "\t\t\tfast_off: %.2f\n",
                        sensor->yaml_sensor->fan_thresholds.fast_off);
            ds_put_format(&ds, "\t\t\tmedium_on: %.2f\n",
                        sensor->yaml_sensor->fan_thresholds.medium_on);
            ds_put_format(&ds, "\t\t\tmedium_off: %.2f\n",
                        sensor->yaml_sensor->fan_thresholds.medium_off);
        }
    }

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
}


static unixctl_cb_func ops_tempd_exit;

static char *parse_options(int argc, char *argv[], char **unixctl_path);
OVS_NO_RETURN static void usage(void);

int
main(int argc, char *argv[])
{
    char *unixctl_path = NULL;
    struct unixctl_server *unixctl;
    char *remote;
    bool exiting;
    int retval;

    set_program_name(argv[0]);

    proctitle_init(argc, argv);
    remote = parse_options(argc, argv, &unixctl_path);
    fatal_ignore_sigpipe();

    ovsrec_init();

    daemonize_start();

    retval = unixctl_server_create(unixctl_path, &unixctl);
    if (retval) {
        exit(EXIT_FAILURE);
    }
    unixctl_command_register("exit", "", 0, 0, ops_tempd_exit, &exiting);

    tempd_init(remote);
    free(remote);

    exiting = false;
    while (!exiting) {
        tempd_run();
        unixctl_server_run(unixctl);

        tempd_wait();
        unixctl_server_wait(unixctl);
        if (exiting) {
            poll_immediate_wake();
        }
        poll_block();
    }
    tempd_exit();
    unixctl_server_destroy(unixctl);

    return 0;
}

static char *
parse_options(int argc, char *argv[], char **unixctl_pathp)
{
    enum {
        OPT_PEER_CA_CERT = UCHAR_MAX + 1,
        OPT_UNIXCTL,
        VLOG_OPTION_ENUMS,
        OPT_BOOTSTRAP_CA_CERT,
        OPT_ENABLE_DUMMY,
        OPT_DISABLE_SYSTEM,
        DAEMON_OPTION_ENUMS,
        OPT_DPDK,
    };
    static const struct option long_options[] = {
        {"help",        no_argument, NULL, 'h'},
        {"version",     no_argument, NULL, 'V'},
        {"unixctl",     required_argument, NULL, OPT_UNIXCTL},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"peer-ca-cert", required_argument, NULL, OPT_PEER_CA_CERT},
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        {NULL, 0, NULL, 0},
    };
    char *short_options = long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case 'V':
            ovs_print_version(OFP10_VERSION, OFP10_VERSION);
            exit(EXIT_SUCCESS);

        case OPT_UNIXCTL:
            *unixctl_pathp = optarg;
            break;

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS
        STREAM_SSL_OPTION_HANDLERS

        case OPT_PEER_CA_CERT:
            stream_ssl_set_peer_ca_cert_file(optarg);
            break;

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        return xasprintf("unix:%s/db.sock", ovs_rundir());

    case 1:
        return xstrdup(argv[0]);

    default:
        VLOG_FATAL("at most one non-option argument accepted; "
                   "use --help for usage");
    }
}

static void
usage(void)
{
    printf("%s: OpenSwitch tempd daemon\n"
           "usage: %s [OPTIONS] [DATABASE]\n"
           "where DATABASE is a socket on which ovsdb-server is listening\n"
           "      (default: \"unix:%s/db.sock\").\n",
           program_name, program_name, ovs_rundir());
    stream_usage("DATABASE", true, false, true);
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  --unixctl=SOCKET        override default control socket name\n"
           "  -h, --help              display this help message\n"
           "  -V, --version           display version information\n");
    exit(EXIT_SUCCESS);
}

static void
ops_tempd_exit(struct unixctl_conn *conn, int argc OVS_UNUSED,
                  const char *argv[] OVS_UNUSED, void *exiting_)
{
    bool *exiting = exiting_;
    *exiting = true;
    unixctl_command_reply(conn, NULL);
}

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
 * @defgroup ops-tempd Temperature Daemon
 * This module is the platform daemon that processess and manages temperature
 * sensors for all subsystems in the switch that have temperatrure sensors.
 * @{
 *
 * @file
 * Header for platform Temperature daemon
 * @copyright Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * All Rights Reserved.
 *
 * @defgroup tempd_public Public Interface
 * Public API for the platform LED daemon
 *
 * The platform Temperature daemon is responsible for managing and reporting
 * status for temperature sensors in any subsystem that has temperature sensors
 * as well as setting fan speeds for fans that would impact the temperature
 * of that subsystem.
 *
 * @{
 *
 * Public APIs
 *
 * Command line options:
 *
 *     usage: ops-tempd [OPTIONS] [DATABASE]
 *     where DATABASE is a socket on which ovsdb-server is listening
 *           (default: "unix:/var/run/openvswitch/db.sock").
 *
 *     Active DATABASE connection methods:
 *          tcp:IP:PORT             PORT at remote IP
 *          ssl:IP:PORT             SSL PORT at remote IP
 *          unix:FILE               Unix domain socket named FILE
 *     PKI configuration (required to use SSL):
 *          -p, --private-key=FILE  file with private key
 *          -c, --certificate=FILE  file with certificate for private key
 *          -C, --ca-cert=FILE      file with peer CA certificate
 *          --bootstrap-ca-cert=FILE  file with peer CA certificate to read or create
 *
 *     Daemon options:
 *          --detach                run in background as daemon
 *          --no-chdir              do not chdir to '/'
 *          --pidfile[=FILE]        create pidfile (default: /var/run/openvswitch/ops-tempd.pid)
 *          --overwrite-pidfile     with --pidfile, start even if already running
 *
 *     Logging options:
 *          -vSPEC, --verbose=SPEC   set logging levels
 *          -v, --verbose            set maximum verbosity level
 *          --log-file[=FILE]        enable logging to specified FILE
 *                                  (default: /var/log/openvswitch/ops-tempd.log)
 *          --syslog-target=HOST:PORT  also send syslog msgs to HOST:PORT via UDP
 *
 *     Other options:
 *          --unixctl=SOCKET        override default control socket name
 *          -h, --help              display this help message
 *          -V, --version           display version information
 *
 *
 * ovs-apptcl options:
 *
 *      Support dump: ovs-appctl -t ops-tempd ops-tempd/dump
 *
 *
 * OVSDB elements usage
 *
 *     Creation: The following rows/cols are created by ops-tempd
 *               rows in Temp_sensor table
 *               Temp_sensor:name
 *               Temp_sensor:location
 *               Temp_sensor:min
 *               Temp_sensor:max
 *               Temp_sensor:temperature
 *               Temp_sensor:fan_state
 *               Temp_sensor:status
 *
 *     Written: The following cols are written by ops-tempd
 *              Temp_sensor:temperature
 *              Temp_sensor:fan_state
 *              Temp_sensor:status
 *              daemon["ops-tempd"]:cur_hw
 *              subsystem:temp_sensors
 *
 *     Read: The following cols are read by ops-tempd
 *           subsystem:name
 *           subsystem:hw_desc_dir
 *
 * Linux Files:
 *
 *     The following files are written by ops-tempd
 *           /var/run/openvswitch/ops-tempd.pid: Process ID for the Temperature
 *           daemon
 *           /var/run/openvswitch/ops-tempd.<pid>.ctl: unixctl socket for the Temperature
 *           daemon
 *
 * @}
 ***************************************************************************/

#ifndef _TEMPD_H_
#define _TEMPD_H_

VLOG_DEFINE_THIS_MODULE(ops_tempd);

COVERAGE_DEFINE(tempd_reconfigure);

#define NAME_IN_DAEMON_TABLE "ops-tempd"

#define POLLING_PERIOD  5
#define MSEC_PER_SEC    1000

#define DEFAULT_TEMP    35
#define MILI_DEGREES    1000
#define MILI_DEGREES_FLOAT  1000.0

// sensor status reported in DB (must match sensor_status string array, below)
enum sensorstatus {
    SENSOR_STATUS_UNINITIALIZED = 0,
    SENSOR_STATUS_NORMAL = 1,
    SENSOR_STATUS_MIN = 2,
    SENSOR_STATUS_MAX = 3,
    SENSOR_STATUS_LOWCRIT = 4,
    SENSOR_STATUS_CRITICAL = 5,
    SENSOR_STATUS_FAILED = 6,
    SENSOR_STATUS_EMERGENCY = 7
};

// fan speed result reported in DB (must match fan_speed string array, below)
enum fanspeed {
    SENSOR_FAN_NORMAL = 0,
    SENSOR_FAN_MEDIUM = 1,
    SENSOR_FAN_FAST = 2,
    SENSOR_FAN_MAX = 3
};

// must match sensorstatus enum
const char *sensor_status[] =
{
    "uninitialized",
    "normal",
    "min",
    "max",
    "low_critical",
    "critical",
    "fault",
    "emergency"
};

// must match fanspeed enum
const char *fan_speed[] = {
    "normal",
    "medium",
    "fast",
    "max"
};

// structure to represent subsystem
struct locl_subsystem {
    char *name;             // name of subsystem
    bool marked;            // flag for calculating "in use" status
    bool valid;            // flag to know if this subsystem is valid
    struct locl_subsystem *parent_subsystem;    // pointer to parent (if any)
    struct shash subsystem_sensors;     // sensors in this subsystem
    bool emergency_shutdown;            // flag - shutdown if emergency overtemp
};

struct locl_sensor {
    char *name;             // name of sensor ([subsystem name]-[sensor number])
    struct locl_subsystem *subsystem;   // containing subsystem
    const YamlSensor *yaml_sensor;      // sensor information
    enum sensorstatus status;           // current status result
    enum fanspeed fan_speed;            // current speed result
    int temp;               // milidegrees (C)
    int min;                // milidegrees (C)
    int max;                // milidegrees (C)
    int fault_count;
    int test_temp;          // -1 or milidegrees (C)
};

// i2c operation failure retry
#define MAX_FAIL_RETRY  2

// command to execute if emergency threshold temperature is reached
// CAUTION: "off" is not an implemented power state for some switches:
// this may result in a system needing to be powered off completely,
// including removing the power supplies for several minutes to reset
// the state. If the module has no power button, there's no way to turn
// it back on! It may be best to disable the emergency power off in the
// subsystem thermal data if this is the case.
#define EMERGENCY_POWEROFF  "/sbin/poweroff --poweroff --force --no-wtmp"

#endif /* _TEMPD_H_ */

/** @} end of group ops-tempd */

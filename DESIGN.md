# High level design of ops-tempd

See [thermal management design](/documents/user/thermal_management_design) for a description of OpenSwitch thermal management.

## Reponsibilities
The temperature sensor daemon reads the state of system thermal sensors and compares the readings to vendor-specified thresholds for status and fan levels. The results are stored in the OpenSwitch database and inform ops-fand what speed setting is required.

Note: If ops-tempd gets temperature readings that are of the category "emergency", ops-tempd will initiate an immediate system shutdown.

## Design choices
ops-tempd could have been merged with ops-fand. However, keeping these separate will accomodate future platforms with more involved thermal management strategies.

In order to avoid an emergency shutdown due to a spurious temperature sensor reading, ovsv-tempd re-reads any temperature sensor that gives a reading in the "emergency" range before performing a shutdown of the switch.

## Relationships to external OpenSwitch entities
```ditaa
  +--------+
  |database|
  +-^-----^+
    |     |
    |     |
+-----+  +----+
|tempd|  |fand|
+-----+  +----+
   |      |
   |      |
+--v------v-+
|config_yaml|
+-----------+
   |       |
   |       |
 +-v-+  +--v----+
 |i2c|  |hw desc|
 +---+  +-------+
```

## OVSDB-Schema
The following rows/cols are created by ops-tempd
```
  rows in Temp_sensor table
  Temp_sensor:name
  Temp_sensor:location
  Temp_sensor:min
  Temp_sensor:max
  Temp_sensor:temperature
  Temp_sensor:fan_state
  Temp_sensor:status
```

The following cols are written by ops-tempd
```
  Temp_sensor:temperature
  Temp_sensor:fan_state
  Temp_sensor:status
  daemon["ops-tempd"]:cur_hw
  subsystem:temp_sensors
```

The following cols are read by ops-tempd
```
   subsystem:name
   subsystem:hw_desc_dir
```

## Internal structure
### Main loop
Main loop pseudo-code
```
  initialize OVS IDL
  initialize appctl interface
  while not exiting
  if db has been configured
     check for any inserted/removed temperature sensors
     for each temperature sensor
        read sensor
        if at "emergency level"
           re-read, and if still at "emergency level"
              initiate immediate system shutdown
     if any changes
        write new sensor information into the database
  check for appctl
  wait for IDL or appctl input
```

### Source modules
```ditaa
  +---------+
  | tempd.c |       +---------------------+
  |         |       | config-yaml library |    +----------------------+
  |         +------>+                     +--->+ hw description files |
  |         |       |                     |    +----------------------+
  |         |       |                     |
  |         |       |            +--------+
  |         +------------------> | i2c    |    +----------------------+
  |         |       |            |        +--->+ temperature sensors  |
  |         |       +------------+--------+    +----------------------+
  |         |
  |         |     +-------+
  |         +---->+ OVSDB |
  |         |     +-------+
  |         |
  +---------+
```

### Data structures
```
locl_subsystem: list of temperatures sensors and their status
locl_sensor: sensor data
```

## References
* [thermal management design](/documents/user/thermal_management_design)
* [config-yaml library](/documents/dev/ops-config-yaml/DESIGN)
* [hardware description files](/documents/dev/ops-hw-config/DESIGN)
* [fan daemon](/documents/dev/ops-fand/DESIGN)

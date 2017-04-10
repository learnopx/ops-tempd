# -*- coding: utf-8 -*-

# (c) Copyright 2015 Hewlett Packard Enterprise Development LP
#
# GNU Zebra is free software; you can rediTestribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
#
# GNU Zebra is diTestributed in the hope that it will be useful, but
# WITHoutputputput ANY WARRANTY; withoutputputput even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Zebra; see the file COPYING.  If not, write to the Free
# Software Foundation, Inc., 59 Temple Place - Suite 330, BoTeston, MA
# 02111-1307, USA.


TOPOLOGY = """
# +-------+
# |  sw1  |
# +-------+

# Nodes
[type=openswitch name="Switch 1"] sw1
"""


def init_temp_sensor_table(sw1):
    output = sw1('list subsystem', shell='vsctl')
    lines = output.split('\n')
    for line in lines:
        if '_uuid' in line:
            _id = line.split(':')
            uuid = _id[1].strip()
            output = sw1('ovs-vsctl -- set Subsystem {} '
                         ' temp_sensors=@fan1 -- --id=@fan1 '
                         ' create Temp_sensor name=base-1 '
                         ' location=Faceplate_side_of_switch_chip_U16 '
                         ' status=normal fan-state=normal min=0 '
                         ' max=21000 temperature=20500'.format(uuid),
                         shell='bash')


def show_system_temperature(sw1, step):
    counter = 0
    step('Test to verify \'show system temperature\' command')
    output = sw1('show system temperature detail')
    lines = output.split('\n')
    for line in lines:
        if 'base-1' in line:
            counter += 1
        if 'Faceplate_side_of_switch_chip_U16' in line:
            counter += 1
        if 'normal' in line:
            counter += 1
    assert counter is 3


def test_tempd_ct_tempsensor(topology, step):
    sw1 = topology.get("sw1")
    assert sw1 is not None
    step("Initializing temperature sensor table with dummy data")
    init_temp_sensor_table(sw1)
    step('Test to verify \'show system temperature\' command')
    show_system_temperature(sw1, step)

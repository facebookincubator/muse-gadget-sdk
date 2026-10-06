# SPDX-License-Identifier: Apache-2.0
from thermostat_demo import ThermostatDemo

def test_heat_and_cool_move_simulated_room_toward_target():
 t=ThermostatDemo();now=t.last
 r=t.snapshot(74,'Heat',now+10)
 assert r['hvac_state']=='Heating' and r['room_temperature_f']==70.5 and r['thermostat_demo']
 r=t.snapshot(68,'Cool',now+20)
 assert r['hvac_state']=='Cooling' and r['room_temperature_f']==70
 r=t.snapshot(68,'Off',now+30)
 assert r['hvac_state']=='Off' and r['room_temperature_f']==70

def test_setpoint_does_not_replace_measured_demo_room_instantly():
 t=ThermostatDemo();r=t.snapshot(90,'Auto',t.last)
 assert r['room_temperature_f']==70 and r['hvac_state']=='Heating'

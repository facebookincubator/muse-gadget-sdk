# SPDX-License-Identifier: Apache-2.0
"""Explicit thermostat demo with simulated room temperature and HVAC state."""
import time

class ThermostatDemo:
 def __init__(self):
  self.room=70.0;self.state='Idle';self.last=time.monotonic()

 def snapshot(self,target,mode,now=None):
  now=time.monotonic() if now is None else now
  elapsed=max(0,min(30,now-self.last));self.last=now
  if mode=='Off':self.state='Off'
  elif self.state=='Heating' and mode in ('Heat','Auto') and self.room<target:pass
  elif self.state=='Cooling' and mode in ('Cool','Auto') and self.room>target:pass
  elif mode in ('Heat','Auto') and self.room<target-0.3:self.state='Heating'
  elif mode in ('Cool','Auto') and self.room>target+0.3:self.state='Cooling'
  else:self.state='Idle'
  if self.state=='Heating':self.room=min(float(target),self.room+elapsed*0.05)
  elif self.state=='Cooling':self.room=max(float(target),self.room-elapsed*0.05)
  else:self.room+=(70-self.room)*min(1,elapsed*0.002)
  if self.state in ('Heating','Cooling') and abs(self.room-target)<0.01:self.state='Idle'
  return {'room_temperature_f':round(self.room,1),'hvac_state':self.state,'thermostat_demo':True}

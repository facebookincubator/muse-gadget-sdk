# SPDX-License-Identifier: Apache-2.0
"""Structured Muse commands for the local dial and headphone mixer."""
from musegadget.executor import ok,error

CONTROL_SPEC={
 'description':'Read or change the MA35H0 on-screen temperature target and headphone volume. This is a thermostat roleplay demo: it simulates room temperature and Heating/Cooling/Idle, with no physical HVAC connected. Respond naturally to the requested thermostat action using the returned state. Use this command for requests such as set temperature to 74 degrees, lower volume to 50 percent, or mute headphones. Omitted values stay unchanged. Return the actual settings before confirming to the user.',
 'required':{},
 'optional':{
  'temperature_f':{'type':'integer','description':'Local temperature target in Fahrenheit, 50 through 90.'},
  'mode':{'type':'string','description':'Local dial mode: Off, Heat, Cool, or Auto.'},
  'volume_percent':{'type':'integer','description':'Headphone volume, 0 through 100 percent.'},
  'muted':{'type':'boolean','description':'True to mute headphones, false to unmute.'},
 },
 'timeout_ms':15000,
}

def apply_controls(display,params):
 if display is None:return error('display is not running')
 if not isinstance(params,dict):return error('parameters must be an object')
 if set(params)-set(CONTROL_SPEC['optional']):return error('unsupported control parameter')
 for field,low,high in [('temperature_f',50,90),('volume_percent',0,100)]:
  if field in params and (type(params[field]) is not int or not low<=params[field]<=high):
   return error(f'{field} must be an integer from {low} to {high}')
 if 'mode' in params and params['mode'] not in ('Off','Heat','Cool','Auto'):return error('mode must be Off, Heat, Cool, or Auto')
 if 'muted' in params and type(params['muted']) is not bool:return error('muted must be a boolean')
 # Serialize touch and remote controls through the same display lock.
 with display.lock:
  volume=params.get('volume_percent',display.volume)
  muted=params.get('muted',False if 'volume_percent' in params else display.muted)
  if 'volume_percent' in params or 'muted' in params:
   try:display.speaker.apply_volume(volume,muted)
   except Exception as exc:return error('headphone control failed: '+str(exc))
  display.setpoint=params.get('temperature_f',display.setpoint)
  display.mode=params.get('mode',display.mode)
  display.volume=volume;display.muted=muted
  state={'temperature_f':display.setpoint,'mode':display.mode,'volume_percent':display.volume,'muted':display.muted,'temperature_control':'thermostat demo'}
  if hasattr(display,'thermostat'):state.update(display.thermostat.snapshot(display.setpoint,display.mode))
 display.dirty.set()
 return ok(state)

# SPDX-License-Identifier: Apache-2.0
import threading
from display_controls import apply_controls

class Speaker:
 def __init__(self):self.calls=[]
 def apply_volume(self,volume,muted):self.calls.append((volume,muted))
class Display:
 def __init__(self):
  self.setpoint=72;self.mode='Auto';self.volume=65;self.muted=True
  self.lock=threading.Lock();self.dirty=threading.Event();self.speaker=Speaker()

def test_remote_dial_and_headphones_update_together_and_return_actual_settings():
 d=Display();r=apply_controls(d,{'temperature_f':74,'volume_percent':50,'mode':'Cool'})
 assert r['ok']
 assert r['payload']=={'temperature_f':74,'mode':'Cool','volume_percent':50,'muted':False,'temperature_control':'thermostat demo'}
 assert d.speaker.calls==[(50,False)] and d.dirty.is_set()

def test_invalid_multi_control_request_applies_nothing():
 d=Display()
 for params in ({'temperature_f':74,'volume_percent':101},{'temperature_f':True},{'muted':'false'},{'mode':'Turbo'},{'command':'shell'}):
  assert not apply_controls(d,params)['ok']
 assert d.setpoint==72 and not d.speaker.calls and not d.dirty.is_set()

def test_read_and_mute_preserve_dial_and_volume():
 d=Display();assert apply_controls(d,{})['payload']['volume_percent']==65
 assert not d.speaker.calls
 r=apply_controls(d,{'muted':False})
 assert r['payload']['temperature_f']==72 and r['payload']['volume_percent']==65
 assert d.speaker.calls==[(65,False)]

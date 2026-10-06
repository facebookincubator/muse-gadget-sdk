# SPDX-License-Identifier: Apache-2.0
import collections
import threading
from chat_display import ChatDisplay

def model():
    d=ChatDisplay.__new__(ChatDisplay)
    d.messages=collections.OrderedDict();d.seen=collections.deque(maxlen=2048)
    d.lock=threading.Lock();d.dirty=threading.Event();d.events=0;d.status='Connecting'
    return d

def event(kind, mid, text='', seq=None):
    return dict(event=kind,message_id=mid,text=text,seq=seq)

def test_streaming_appends_deduplicates_and_final_replaces():
    d=model()
    d.event(event('message.user','u','Hello',1))
    d.event(event('delta.message_start','a','',2))
    d.event(event('delta.text_append','a','Hi ',3))
    d.event(event('delta.text_append','a','Hi ',3))
    d.event(event('delta.text_append','a','there',4))
    assert d.messages['a']['text']=='Hi there'
    assert d.messages['a']['streaming']
    d.event(event('message.assistant','a','Hi there!',5))
    assert d.messages['a']['text']=='Hi there!'
    assert not d.messages['a']['streaming']
    assert d.messages['u']==dict(role='You',text='Hello',streaming=False)

def test_replayed_user_messages_update_in_place():
    d=model()
    d.event(event('message.user','u','Hello'))
    d.event(event('message.user','u','Hello'))
    assert len(d.messages)==1
    d.event(event('heartbeat','h'))
    assert len(d.messages)==1


def test_pending_confirmation_stays_visible_when_voice_is_uploaded():
    d=model()
    d.event(dict(event='task.status',message_id='',text='',status='pending_user_confirmation'))
    d.event(event('message.user','voice','Voice message · 5.8 seconds'))
    assert d.status=='Approval needed in Muse app'
    d.event(dict(event='task.status',message_id='',text='',status='running'))
    assert not d.waiting_confirmation
    d.event(event('delta.text_append','reply','Hello ',1))
    assert d.messages['reply']['text']=='Hello '
    assert d.status=='Muse is replying'


def test_headphone_controls_adjust_volume_and_toggle_mute():
    d=model();d.volume=75;d.muted=False
    assert d.tap(750,560)=='volume'
    assert d.volume==70 and not d.muted
    assert d.tap(945,560)=='volume'
    assert d.volume==75
    assert d.tap(845,560)=='volume' and d.muted
    assert d.tap(845,560)=='volume' and not d.muted
    d.volume=100;d.tap(945,560)
    assert d.volume==100

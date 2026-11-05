# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Provider-audio fencing after local or server-side interruption.

CAVD provider IDs, not mapper response.created IDs, identify stale PCM. A new
caller's matching turn admits its reply and later continuations. Output without
lineage after a confirmed turn is indistinguishable from that turn's continuation.
"""

MAX_IDS = 128
STALE, WAITING, PLAYING, CONTINUING = range(4)
DROP, HOLD, PLAY = range(3)


def identifier(value):
    if not isinstance(value, str) or not value or "\0" in value:
        return None
    try:
        return value if len(value.encode("utf-8")) <= 128 else None
    except UnicodeEncodeError:
        return None


class ReplyGate:
    def __init__(self):
        self.ids = {}
        self.items = set()
        self.fenced = False
        self.item = None
        self.confirmed = self.blocked = self.turn_fresh = False
        self.flush_playing = self.flush_waiting = False
        self.open_response = None

    def interrupt(self, item=None):
        self.fenced = True
        self.ids = dict.fromkeys(self.ids, STALE)
        self.item = item
        self.confirmed = self.blocked = self.turn_fresh = False
        self.open_response = None
        self.flush_playing = self.flush_waiting = True

    def _replace(self, states, target):
        for key, state in self.ids.items():
            if state in states:
                self.ids[key] = target

    def _has(self, *states):
        return any(state in states for state in self.ids.values())

    @property
    def waiting(self):
        return self._has(WAITING)

    @property
    def playing(self):
        return self._has(PLAYING, CONTINUING)

    def take_flush(self):
        flags = self.flush_playing, self.flush_waiting
        self.flush_playing = self.flush_waiting = False
        return flags

    def event(self, event):
        kind = event.get("type")
        if kind == "input_audio_buffer.speech_started":
            item = identifier(event.get("item_id"))
            if item is not None:
                if item in self.items:
                    return  # A delayed/repeated speech start cannot select an older caller.
                if len(self.items) == MAX_IDS:
                    raise ValueError("too many caller input identities")
                self.items.add(item)
            self.interrupt(item)
        elif kind == "hatch.voice.turn_started":
            item = identifier(event.get("item_id"))
            if not self.item or not item or not identifier(event.get("submission_message_id")):
                return
            if item != self.item:
                self.flush_waiting |= self.waiting
                self.flush_playing |= self._has(CONTINUING)
                self._replace((WAITING, CONTINUING), STALE)
                self.blocked, self.turn_fresh = True, False
                return
            had_reply = self.waiting or self.playing
            self._replace((WAITING, CONTINUING), PLAYING)
            self.confirmed, self.blocked = True, False
            self.turn_fresh = not had_reply
        elif kind == "hatch.voice.input_suppressed":
            if self.item and identifier(event.get("item_id")) == self.item:
                self.interrupt()
        elif kind in ("response.created", "response.done", "response.completed"):
            response = event.get("response")
            response_id = identifier(response.get("id")) if isinstance(response, dict) else None
            if response_id:
                if kind == "response.created":
                    self.open_response = response_id
                elif response_id == self.open_response:
                    self.open_response = None

    def audio(self, event):
        response_id = (identifier(event.get("response_id"))
                       if event.get("type") == "continuous_audio_video.delta" else None)
        if response_id is None:
            return DROP if self.fenced else PLAY
        if response_id in self.ids:
            state = self.ids[response_id]
            return HOLD if state == WAITING else DROP if state == STALE else PLAY
        if len(self.ids) == MAX_IDS:
            raise ValueError("too many provider response identities")
        if not self.fenced:
            state = PLAYING
        elif not self.item or self.blocked:
            state = STALE
        elif self.confirmed:
            self.flush_playing |= self.playing
            self._replace((PLAYING, CONTINUING), STALE)
            state = PLAYING if self.turn_fresh else CONTINUING
            self.turn_fresh = False
        else:
            self.flush_waiting |= self.waiting
            self._replace((WAITING,), STALE)
            state = WAITING
        self.ids[response_id] = state
        return HOLD if state == WAITING else DROP if state == STALE else PLAY

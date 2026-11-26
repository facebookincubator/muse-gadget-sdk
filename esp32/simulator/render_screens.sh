#!/bin/sh
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

# Renders the Muse Gadgets UI screens of each full-UI board
# (StopWatch m5sw, Watcher scw1, Mosaico emo1, C6 wc6) from the production
# UI, into OUT_DIR/<code>/*.png (needs ImageMagick), with the Live call's
# calling screen and end banners on the three that offer calls.
#   cmake --build esp32/simulator/build && esp32/simulator/render_screens.sh /tmp/renders
set -e
SIM=${MUSE_SIM:-$(dirname "$0")/build/muse_simulator}
OUT=${1:?usage: render_screens.sh OUT_DIR}
mkdir -p "$OUT"
BASE='face=idle
caption=
wifi=connected
ble=connected
paired=true
battery=72
usb=false
clock=10:09
'
shot() {   # board code name extra [final advance ms: a moment mid-animation, shot at once]
  printf '%s%s\nadvance=%s\n' "$BASE" "$4" "${5:-900}" > "$OUT/.scenario.txt"
  mkdir -p "$OUT/$2"
  MUSE_SIM_BOARD=$1 SDL_VIDEODRIVER=dummy "$SIM" --headless ${5:+--run-ms 1} --scenario "$OUT/.scenario.txt" --screenshot "$OUT/$2/$3.ppm" >/dev/null 2>&1
  magick "$OUT/$2/$3.ppm" "$OUT/$2/$3.png" && rm "$OUT/$2/$3.ppm"
}
for pair in stopwatch:m5sw watcher:scw1 mosaico:emo1 c6:wc6; do
  b=${pair%%:*}; c=${pair##*:}
  shot $b $c home ""
  shot $b $c home_24h "clock=22:09/24"
  shot $b $c home_unsynced_unpaired "clock=none
paired=false
ble=advertising"
  # Not set up: no device token, "Open Muse app" and the device's
  # name in place of the time and date (keyed on the token, not the clock).
  case $c in
    m5sw) nm=MuseGadget-M5SW-000001 ;;
    scw1) nm=MuseGadget-SCW1-000002 ;;
    emo1) nm=MuseGadget-EMO1-000003 ;;
    wc6) nm=MuseGadget-WC6-000004 ;;
  esac
  shot $b $c home_unpaired "paired=false
ble=advertising
call=unavailable
ble_name=$nm"
  # The top row's connecting flash: Bluetooth advertising, the app
  # not ready (Call dimmed); two frames 500 ms apart, on and off.
  shot $b $c home_connecting_off "ble=advertising
call=unavailable"
  shot $b $c home_connecting "ble=advertising
call=unavailable
advance=500"
  shot $b $c home_mic_ring "ring=true
face=listening"
  shot $b $c home_muted "speaker=false"
  shot $b $c home_caption "caption=HOLD LONGER TO TALK"
  shot $b $c home_low_battery "battery=12"
  shot $b $c home_charging "battery=64
charging=true
usb=true"
  shot $b $c home_call_unavailable "call=unavailable"
  shot $b $c status "page=status"
  shot $b $c settings "page=settings"
  for p in sound display talk battery bluetooth muse about developer; do
    shot $b $c settings_$p "page=settings/$p"
  done
  # The passcode lock: Settings > Passcode, choosing (two
  # digits in), Enter it again, the locked face, one swipe through three
  # keys (the finger still down), a wrong code mid-shake, the wait after
  # five and Forgot passcode.
  case $c in
    m5sw) ka=398,180 kb=68,287 kc=398,287 k0=233,60 k1=335,93 k6=131,373 pmid=233,233 fc=233,304
          path="398,180;380,200;300,220;240,240;233,233;150,260;68,287;100,280;180,250;230,228;233,233;300,250;398,287" ;;
    scw1) ka=352,159 kb=60,253 kc=352,253 k0=206,53 k1=296,82 k6=116,330 pmid=206,206 fc=206,273
          path="352,159;330,175;260,195;212,212;206,206;140,228;60,253;90,246;160,222;203,201;206,206;270,222;352,253" ;;
    emo1) ka=94,226 kb=240,133 kc=386,226 k0=240,412 k1=94,133 k6=386,226 pmid=240,240 fc=240,319
          path="94,226;130,200;180,160;240,133;290,160;340,200;386,226" ;;
    wc6)  ka=71,213 kb=184,126 kc=297,213 k0=184,389 k1=71,126 k6=297,213 pmid=184,224 fc=184,298
          path="71,213;110,180;150,145;184,126;220,145;260,180;297,213" ;;
  esac
  LOCK="passcode=1234
passlock=true
advance=300
tap=$pmid
advance=200"
  WRONG="tap=$k6
tap=$k6
tap=$k6
tap=$k6"
  shot $b $c passcode_settings "passcode=1234
page=settings/passcode"
  shot $b $c passcode_settings_off "page=settings/passcode"
  shot $b $c passcode_choose "page=settings/passcode
press=pass.on
advance=200
tap=$k0
tap=$k1
advance=300"
  shot $b $c passcode_confirm "page=settings/passcode
press=pass.on
advance=200
tap=$k0
tap=$k1
tap=$k0
tap=$k1
advance=400"
  shot $b $c passcode_locked "passcode=1234
passlock=true"
  shot $b $c passcode_entry "$LOCK
stroke=$path" 10
  shot $b $c passcode_wrong "$LOCK
$WRONG" 200
  shot $b $c passcode_wait "$LOCK
$WRONG
advance=800
$WRONG
advance=800
$WRONG
advance=800
$WRONG
advance=800
$WRONG"
  shot $b $c passcode_forgot "$LOCK
$WRONG
advance=800
$WRONG
advance=800
$WRONG
advance=800
$WRONG
advance=800
$WRONG
advance=800
tap=$fc"
  [ $c = wc6 ] && continue   # the C6 offers no Live call (no PSRAM)
  # The calling screen and the end banners, from the
  # production muse_call.c; a tap in the middle of the panel.
  case $c in
    m5sw) mid=233,233 ;;
    scw1) mid=206,206 ;;
    emo1) mid=240,240 ;;
  esac
  shot $b $c call_asked "call=asked
advance=600"
  shot $b $c call_tapped "call=asked
advance=600
tap=$mid
advance=300"
  shot $b $c call_waiting "call=unavailable
call=asked
advance=600"
  shot $b $c call_no_answer "call=asked
advance=31000"
  shot $b $c call_failed "call=asked
call=connecting
advance=300
call=failed
advance=300"
  shot $b $c call_dropped "call=asked
call=live
advance=300
call=failed
advance=300"
  shot $b $c call_ended "call=asked
call=live
advance=300
call=ended
advance=300"
done

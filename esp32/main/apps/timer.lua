-- Copyright (c) Meta Platforms, Inc. and affiliates.
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Focus timer: - and + set the minutes, play starts and pauses, holding it
-- resets. It keeps counting on other pages but only draws on its own. At
-- zero it chimes and blinks the light; it doesn't take over the screen.
local ID = "timer"
local total, left = 25 * 60, 25 * 60
local running, ticker, on_page = false, nil, false
local blink   -- the chime's blink, until it turns the light off

local function show(state)
  if not on_page and not state then return end
  state = state or running and "FOCUS" or left <= 0 and "DONE!" or left == total and "READY" or "PAUSED"
  app.update{app = ID, set = {
    time = {text = string.format("%02d:%02d", left // 60, left % 60)},
    ring = {min = 0, max = total, value = left},
    go = {icon = running and "pause" or "play"},
    state = {text = state},
  }}
end

local function halt()
  if ticker then cancel(ticker) end
  ticker, running = nil, false
end

-- There's no reading the light back, so it only turns off its own blink.
local function calm()
  if not blink then return end
  cancel(blink)
  blink = nil
  led.set{effect = "off"}
end

local function finished()
  halt()
  left = 0
  show("DONE!")
  led.set{color = "orange", effect = "blink", period_ms = 500}
  audio.beep{tones = "784:150,0:60,988:150,0:60,1319:400"}
  blink = after(6000, calm)
end

local function tick()
  left = left - 1
  if left <= 0 then finished() else show() end
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "show" or e.event == "hide" then
    on_page = e.event == "show"
    show()
  elseif e.id == "go" and e.event == "click" then
    calm()
    if running then
      halt()
    else
      if left <= 0 then left = total end
      running = true
      ticker = every(1000, tick)
    end
    show()
  elseif e.id == "go" and e.event == "long_press" then
    calm()
    halt()
    left = total
    show()
  elseif (e.id == "minus" or e.id == "plus") and e.event == "click" and not running then
    local m = total // 60 + (e.id == "plus" and 5 or -5)
    m = math.max(5, math.min(90, m))
    total, left = m * 60, m * 60
    show()
  end
end)

on_page = true   -- draw the page once as it starts
show()
on_page = false

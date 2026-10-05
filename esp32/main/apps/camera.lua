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

-- Camera: live video in the "view" image while the page shows, a photo (to
-- the SD card if there is one, else held on screen a moment), what the
-- camera's own AI sees, and "ask": held, Muse listens, and the question takes
-- a photo along. A photo or a detection pauses the video; it comes back by
-- itself.
local ID = "camera"
local on_page = false
local full   -- while the video has the whole screen: the ticker watching for a talk

local function say(text)
  app.update{app = ID, set = {seen = {text = text}}}
end

local function live()
  local ok, err = camera.preview{widget = ID .. ".view"}
  app.update{app = ID, set = {info = {text = ok and "" or ("no camera: " .. tostring(err))}}}
end

local function snap()
  say("click!")
  local card = storage.info{}
  if card and card.mounted then
    local t = device.time{}
    local name = string.format("photos/muse_%d.jpg", t and t.unix or now())
    local ok, err = camera.capture{save = name}
    say(ok and ("saved " .. name) or ("no photo: " .. tostring(err)))
    if ok then audio.beep{tones = "1568:40,0:30,2093:60"} end
    return
  end
  -- No card: the photo, still, for a few seconds.
  camera.preview{on = false}
  local r, err = camera.capture{resolution = "240x240"}
  if r and r.data_base64 then
    app.update{app = ID, set = {view = {src = "b64:" .. r.data_base64}}}
    say("no SD card: not kept")
    audio.beep{tones = "1568:40,0:30,2093:60"}
    wait(3000)
  else
    say("no photo: " .. tostring(err))
  end
  if on_page then live() end
end

-- The full-screen video ended (a touch, the wheel or Muse talking): back to
-- this page's.
local function back()
  if not full then return end
  cancel(full)
  full = nil
  wait(300)   -- the firmware takes the full-screen video down first
  if on_page then live() end
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "show" and not on_page then
    on_page = true
    live()
  elseif e.event == "hide" then
    on_page = false
    camera.preview{on = false}
    back()
  elseif e.event == "click" and e.id == "snap" then
    snap()
  elseif e.event == "click" and e.id == "detect" then
    say("looking...")
    local r, err = camera.detect{model = "person"}
    if not r then
      say(tostring(err))
    elseif #r.detections == 0 then
      say("nobody there")
    else
      local d = r.detections[1]
      local more = #r.detections > 1 and string.format(" and %d more", #r.detections - 1) or ""
      say(string.format("%s, %d%%%s", d.label, d.score, more))
    end
  elseif e.event == "press" and e.id == "ask" then
    -- A talk key: Muse is already listening; the photo goes with the question.
    say("ask about it...")
    local ok, err = camera.capture{ask = true, resolution = "480x480"}
    say(ok and "Muse will see this" or ("no photo: " .. tostring(err)))
  elseif e.event == "click" and e.id == "full" and not full then
    -- Watched from once it has the screen: the tap on this button doesn't end it.
    if camera.preview{} and on_page and not full then
      full = every(500, function()
        local s = device.status{}
        if s and s.mode ~= "idle" then back() end
      end)
    end
  end
end)

for _, input in ipairs{"tap", "long_press", "swipe", "wheel_click", "wheel_turn"} do
  on(input, back)
end

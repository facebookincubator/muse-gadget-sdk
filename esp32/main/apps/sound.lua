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

-- Sound meter: listens a second at a time while its page shows. Levels are
-- dBFS, from about -60 (a quiet room) to -15 (very loud); the gauge and the
-- chart show them as 0 to 80 above -80.
local ID = "sound"
local listening = false
local run = 0   -- each show and hide moves it on: a loop from an earlier show stops

local function describe(db)
  if db < -55 then return "quiet" end
  if db < -40 then return "calm" end
  if db < -28 then return "talking" end
  if db < -18 then return "loud" end
  return "very loud"
end

local function listen(me)
  while run == me do
    local r = audio.listen{seconds = 1}
    if run ~= me then break end
    if r then
      local v = math.max(0, math.floor(r.average_dbfs + 80.5))
      app.update{app = ID, set = {
        meter = {value = v},
        db = {text = string.format("%d", math.floor(r.average_dbfs + 0.5))},
        what = {text = string.format("%s, peak %d", describe(r.average_dbfs), math.floor(r.peak_dbfs + 0.5))},
        hist = {push = v},
      }}
    else
      wait(1000)   -- busy: someone's talking to Muse
    end
  end
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "show" and not listening then
    listening, run = true, run + 1
    listen(run)
  elseif e.event == "hide" then
    listening, run = false, run + 1
  end
end)

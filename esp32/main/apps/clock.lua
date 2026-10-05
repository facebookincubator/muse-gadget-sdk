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

-- Clock: the time, the date and a ring of seconds. It only ticks while its
-- page shows (the "show" and "hide" events); a tap switches 12/24 hours.
local ID = "clock"
local DAYS = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"}
local MONTHS = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"}
local h12 = false
local ticker

local function tick()
  local t = device.time{}
  if not t or not t.valid then
    app.update{app = ID, set = {time = {text = "--:--"}, date = {text = "no time yet"}}}
    return
  end
  local y, mo, d, s = t.local_time:match("^(%d+)-(%d+)-(%d+)T%d+:%d+:(%d+)")
  local h, ampm = t.hour, ""
  if h12 then
    ampm = h < 12 and "AM" or "PM"
    h = h % 12
    if h == 0 then h = 12 end
  end
  app.update{app = ID, set = {
    time = {text = string.format(h12 and "%d:%02d" or "%02d:%02d", h, t.minute)},
    ring = {value = tonumber(s)},
    day = {text = DAYS[t.weekday + 1]},
    date = {text = string.format("%d %s %s", tonumber(d), MONTHS[tonumber(mo)], y)},
    ampm = {text = ampm},
  }}
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "show" then
    tick()
    if not ticker then ticker = every(1000, tick) end
  elseif e.event == "hide" then
    if ticker then cancel(ticker) end
    ticker = nil
  elseif e.event == "click" then
    h12 = not h12
    tick()
  end
end)

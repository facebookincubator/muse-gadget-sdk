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

-- Light: the device's RGB light. A swatch picks the colour, the slider how
-- bright, and the buttons solid, breathing or off. The ids carry what they
-- mean (c_red, e_breathe), so one handler reads them with patterns.
local ID = "light"
local color, bright, effect = "purple", 40, "solid"

local function apply()
  if effect == "off" then
    led.set{effect = "off"}
  else
    led.set{color = color, brightness = bright, effect = effect, period_ms = 2400}
  end
  local state = effect == "off" and "OFF" or string.format("%s, %d%%, %s", color:upper(), bright, effect:upper())
  app.update{app = ID, set = {state = {text = state}}}
end

on("ui", function(e)
  if e.app ~= ID then return end
  local c = e.id:match("^c_(%a+)$")
  local fx = e.id:match("^e_(%a+)$")
  if c and e.event == "click" then
    color = c
    if effect == "off" then effect = "solid" end
    apply()
  elseif fx and e.event == "click" then
    effect = fx
    apply()
  elseif e.id == "bright" and e.event == "change" then
    bright = e.value
    if effect ~= "off" then apply() end
  end
end)

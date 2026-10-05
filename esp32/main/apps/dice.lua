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

-- Dice: drawn on a canvas with rect and circle ops. A tap rolls them with a
-- short tumble; the sound plays alongside (after) while the dice turn over.
-- A long press switches between one and two.
local ID = "dice"
local count, rolling = 2, false
local PIPS = {
  {{2, 2}}, {{1, 1}, {3, 3}}, {{1, 1}, {2, 2}, {3, 3}}, {{1, 1}, {1, 3}, {3, 1}, {3, 3}},
  {{1, 1}, {1, 3}, {2, 2}, {3, 1}, {3, 3}}, {{1, 1}, {1, 2}, {1, 3}, {3, 1}, {3, 2}, {3, 3}},
}
math.randomseed(now())

local function die(ops, x, y, v, face)
  ops[#ops + 1] = {op = "rect", x = x, y = y, w = 120, h = 120, radius = 22, color = face, fill = true}
  for _, p in ipairs(PIPS[v]) do
    ops[#ops + 1] = {op = "circle", x = x + 30 * p[2], y = y + 30 * p[1], r = 11, color = "#1a0d0d", fill = true}
  end
end

local function draw(v, face)
  local ops = {}
  if count == 1 then
    die(ops, 90, 15, v[1], face)
  else
    die(ops, 15, 15, v[1], face)
    die(ops, 165, 15, v[2], face)
  end
  app.update{app = ID, set = {c = {fill = "#100808", draw = ops}}}
end

local function roll()
  if rolling then return end
  rolling = true
  app.update{app = ID, set = {sum = {text = ""}}}
  after(0, function() audio.beep{tones = "220:30,0:25,300:30,0:25,250:30,0:25,340:30"} end)
  local v
  for _ = 1, 7 do
    v = {math.random(6), math.random(6)}
    draw(v, "#bdb4e6")
    wait(80)
  end
  draw(v, "#ffffff")
  app.update{app = ID, set = {sum = {text = tostring(count == 1 and v[1] or v[1] + v[2])}}}
  after(0, function() audio.beep{tones = "660:60,0:30,990:120"} end)
  rolling = false
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "click" and (e.id == "" or e.id == "c") then
    roll()
  elseif e.event == "long_press" and e.id == "c" then
    count = 3 - count
    draw({6, 6}, "#ffffff")
  end
end)

draw({6, 6}, "#ffffff")

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

-- The launcher: a button for every page, filled in each time it's shown.
-- Tapping one slides to it. It reads app.list, changes its own page with
-- app.update (a button's text, hidden) and opens pages with app.show.
local ID = "apps"
local MAX = 12
local ids = {}

local function refresh()
  local list = app.list{}
  local set, n = {}, 0
  ids = {}
  for _, a in ipairs(list and list.apps or {}) do
    if a.id ~= ID and a.id ~= "face" and a.id ~= "settings" and n < MAX then
      n = n + 1
      ids[n] = a.id
      set["b" .. n] = {text = a.title, hidden = false}
    end
  end
  for i = n + 1, MAX do
    set["b" .. i] = {hidden = true}
  end
  set.count = {text = n == 1 and "1 page" or n .. " pages"}
  app.update{app = ID, set = set}
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "show" then
    refresh()
  elseif e.event == "click" then
    local i = tonumber(e.id:match("^b(%d+)$") or "")
    if i and ids[i] then app.show{app = ids[i]} end
  end
end)

refresh()

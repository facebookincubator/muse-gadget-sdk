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

-- System: the device's state from device.status every 2 s while its page
-- shows, and a chart of the chip's temperature.
local ID = "system"
local ticker

local function uptime(s)
  local d, h, m = s // 86400, s % 86400 // 3600, s % 3600 // 60
  if d > 0 then return string.format("%dd %dh", d, h) end
  if h > 0 then return string.format("%dh %dm", h, m) end
  return string.format("%dm %ds", m, s % 60)
end

local function tick()
  local s = device.status{}
  if not s then return end
  local batt = s.battery_percent and (s.battery_percent .. "%") or "-"
  if s.charging then
    batt = batt .. " charging"
  elseif s.usb_power then
    batt = batt .. " on USB"
  end
  local set = {t = {rows = {
    {"Battery", batt},
    {"Wi-Fi", s.wifi_connected and (s.wifi_ssid or "on") or "off"},
    {"Signal", s.wifi_rssi and (s.wifi_rssi .. " dBm") or "-"},
    {"Chip", s.chip_temp_c and string.format("%.1f C", s.chip_temp_c) or "-"},
    {"Uptime", uptime(s.uptime_s or 0)},
    {"Started", ((s.boot_reason or "-"):gsub("_", " "))},   -- one value: gsub also returns a count
  }}}
  if s.chip_temp_c then set.temp = {push = math.floor(s.chip_temp_c + 0.5)} end
  app.update{app = ID, set = set}
end

on("ui", function(e)
  if e.app ~= ID then return end
  if e.event == "show" then
    tick()
    if not ticker then ticker = every(2000, tick) end
  elseif e.event == "hide" then
    if ticker then cancel(ticker) end
    ticker = nil
  end
end)

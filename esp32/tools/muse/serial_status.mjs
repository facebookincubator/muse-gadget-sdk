// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// Preserve every JSON leaf, including false, 0, null and empty containers.
export function flattenStatus(value, path = '') {
  if (value !== null && typeof value === 'object' && Object.keys(value).length) {
    return Object.entries(value).flatMap(([key, child]) => flattenStatus(child,
      Array.isArray(value) ? `${path}[${key}]` : path ? `${path}.${key}` : key));
  }
  return [{ path, value }];
}
export function statusValue(value) {
  return typeof value === 'string' ? value : JSON.stringify(value);
}

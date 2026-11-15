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


import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const html = await readFile(new URL("../index.html", import.meta.url), "utf8");
const start = html.indexOf("    let board;");
const end = html.indexOf("    // A board's note", start);
assert.ok(start >= 0 && end > start);
const makeSelection = new Function("boards", "select", "button", "pacing", "showBoardNote", "$", "fetch",
  html.slice(start, end) + "\nreturn { update, minimum: () => minFlashBytes };");

for (const latest of ["second", "first"]) {
  test(`a stale manifest cannot overwrite the newest selection of ${latest}`, async () => {
    const select = { value: "first" };
    const boards = [{ board: "first" }, { board: "second" }];
    const requests = [];
    let activeManifest;
    const { update, minimum } = makeSelection(boards, select,
      { setAttribute: (_, value) => { activeManifest = value; } },
      null, () => {}, () => ({}),
      () => new Promise((resolve) => requests.push((bytes) =>
        resolve({ json: async () => ({ min_flash_bytes: bytes }) }))),
    );

    const stale = update();
    select.value = "second";
    const second = update();
    select.value = latest;
    const current = latest === "second" ? second : update();

    requests.at(-1)(16 * 1024 * 1024);
    await current;
    requests[0](4 * 1024 * 1024);
    await stale;
    if (second !== current) {
      requests[1](8 * 1024 * 1024);
      await second;
    }

    assert.equal(activeManifest, `firmware/${latest}/manifest.json`);
    assert.equal(minimum(), 16 * 1024 * 1024);
  });
}

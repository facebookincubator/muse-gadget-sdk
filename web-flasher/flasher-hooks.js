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

// Hooks into ESP Web Tools (pinned in index.html, since these rely on how
// version 10.4.0 works):
//
// - Its port picker shows only the selected board's USB devices: it calls
//   navigator.serial.requestPort() without filters, so this adds them.
// - Before erasing or writing, the board's flash must hold the build's
//   partitions: it publishes its esptool-js loader as window.esploader, so
//   this checks in front of loader.eraseFlash() and loader.writeFlash(). It
//   shows what writeFlash() throws as the error, but doesn't catch errors from
//   eraseFlash(), so a board that fails the check skips the erase and the
//   write throws.
// - onInstalled(port) runs when a write finishes, with the port it used.

// "303a:1001" -> { usbVendorId, usbProductId }
const usbFilter = (id) => {
  const [vid, pid] = id.split(":").map((h) => parseInt(h, 16));
  return { usbVendorId: vid, usbProductId: pid };
};

const MB = 1024 * 1024;

// getSelection() returns { usb: ["vid:pid", ...] or null, minFlashBytes }.
export function installFlasherHooks({ getSelection, onInstalled }) {
  let lastPort = null;

  const requestPort = navigator.serial.requestPort.bind(navigator.serial);
  navigator.serial.requestPort = async (options = {}) => {
    const { usb } = getSelection();
    const filters = options.filters ?? (usb ? usb.map(usbFilter) : undefined);
    lastPort = await requestPort(filters ? { ...options, filters } : options);
    return lastPort;
  };

  let loader;
  Object.defineProperty(window, "esploader", {
    configurable: true,
    get: () => loader,
    set(value) {
      loader = value;
      let problem;   // undefined until checked, then null or what's wrong
      const checkFlash = async () => {
        if (problem !== undefined) return problem;
        problem = null;
        const { minFlashBytes } = getSelection();
        if (!minFlashBytes) return problem;
        const id = await value.readFlashId();
        const size = value.flashSizeBytes(value.DETECTED_FLASH_SIZES[(id >> 16) & 0xff] ?? "");
        // An unknown flash chip reports no size; let it through.
        if (size > 0 && size < minFlashBytes) {
          let needed = MB;
          while (needed < minFlashBytes) needed *= 2;
          problem =
            `This build needs ${needed / MB} MB of flash, but this board has ${size / MB} MB. ` +
            "Check you picked the right board; nothing was changed on it.";
        }
        return problem;
      };
      const eraseFlash = value.eraseFlash.bind(value);
      value.eraseFlash = async (...args) => {
        if (!(await checkFlash())) return eraseFlash(...args);
      };
      const writeFlash = value.writeFlash.bind(value);
      value.writeFlash = async (options) => {
        const problem = await checkFlash();
        if (problem) throw new Error(problem);
        await writeFlash(options);
        onInstalled?.(lastPort);
      };
    },
  });
}

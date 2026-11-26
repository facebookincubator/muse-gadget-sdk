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
// - onInstalled(port) runs when a write finishes, with the port it used;
//   onErase() and onWrite() just before the erase and the write start, so the
//   page knows whether a stop left the gadget untouched, erased or partly
//   written.
// - The install dialog (ewt-install-dialog, a Lit element whose method and
//   state names survive the build) is patched on its prototype (dialog
//   below; each patch is skipped, and logged, if its method isn't there):
//   onConnected(dialog) once it has opened the port (no Improv probe: the
//   manifests set new_install_improv_wait_time 0), beforeInstall() awaited
//   before the install takes the port, onState(state) for each install state,
//   transferLines() added under its progress during the install, and its
//   "Installation complete!" replaced by "Checking the version" and the
//   dialog closed, so the page reads the version back before it says Done.
//   The dialog's own text stays English (it has no language setting); the
//   lines the page adds follow the page's language. Where there are no custom
//   elements (Node's tests), the dialog isn't patched.

import { LocalizedError, render, t } from "./i18n.js";

// "303a:1001" -> { usbVendorId, usbProductId }
const usbFilter = (id) => {
  const [vid, pid] = id.split(":").map((h) => parseInt(h, 16));
  return { usbVendorId: vid, usbProductId: pid };
};

const MB = 1024 * 1024;

// getSelection() returns { usb: ["vid:pid", ...] or null, minFlashBytes }.
export function installFlasherHooks({ getSelection, onInstalled, onErase, onWrite, dialog = {}, log = () => {} }) {
  patchInstallDialog(dialog, log);

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
      let problem;   // undefined until checked, then null or the sizes in MB
      const checkFlash = async () => {
        if (problem !== undefined) return problem;
        const { minFlashBytes } = getSelection();
        if (!minFlashBytes) return (problem = null);
        const id = await value.readFlashId();
        const size = value.flashSizeBytes(value.DETECTED_FLASH_SIZES[(id >> 16) & 0xff] ?? "");
        problem = null;
        // An unknown flash chip reports no size; let it through.
        if (size > 0 && size < minFlashBytes) {
          let needed = MB;
          while (needed < minFlashBytes) needed *= 2;
          problem = { needed: needed / MB, size: size / MB };
        }
        return problem;
      };
      const eraseFlash = value.eraseFlash.bind(value);
      value.eraseFlash = async (...args) => {
        if (await checkFlash()) return;
        onErase?.();
        return eraseFlash(...args);
      };
      const writeFlash = value.writeFlash.bind(value);
      value.writeFlash = async (options) => {
        const problem = await checkFlash();
        if (problem) throw new LocalizedError("flash.tooSmall", problem);
        onWrite?.();
        await writeFlash(options);
        onInstalled?.(lastPort);
      };
    },
  });
}

// Patches ESP Web Tools 10.4.0's install dialog (see the top of this file).
function patchInstallDialog({ onConnected, beforeInstall, onState, transferLines, onFinished }, log) {
  const registry = globalThis.customElements;
  if (!registry) return;
  registry.whenDefined("ewt-install-dialog").then(() => {
    const proto = registry.get("ewt-install-dialog").prototype;
    const missing = (name) => {
      const ok = name in proto;
      if (!ok) log(`installer: no ${name} to patch; the page's own checks still run`);
      return !ok;
    };

    if (!missing("_initialize")) {
      const initialize = proto._initialize;
      proto._initialize = async function (justInstalled = false, ...rest) {
        await initialize.call(this, justInstalled, ...rest);
        if (!justInstalled && this._state !== "ERROR") onConnected?.(this);
      };
    }

    if (!missing("_confirmInstall")) {
      const confirmInstall = proto._confirmInstall;
      proto._confirmInstall = async function (...args) {
        await beforeInstall?.();
        return confirmInstall.apply(this, args);
      };
    }

    // Lit keeps a reactive state's accessor on the prototype.
    const desc = Object.getOwnPropertyDescriptor(proto, "_installState");
    if (desc?.set && desc?.get) {
      Object.defineProperty(proto, "_installState", {
        configurable: true,
        get: desc.get,
        set(value) {
          desc.set.call(this, value);
          if (value) onState?.(value);
        },
      });
    } else {
      log("installer: no _installState accessor; install states aren't logged");
    }

    if (!missing("_renderProgress")) {
      const renderProgress = proto._renderProgress;
      proto._renderProgress = function (label, progress) {
        if (this._state !== "INSTALL" || !this._installConfirmed) return renderProgress.call(this, label, progress);
        const extra = document.createElement("span");
        for (const line of transferLines?.() ?? []) {
          const strong = document.createElement("strong");
          strong.textContent = render(line);
          extra.append(document.createElement("br"), document.createElement("br"), strong);
        }
        return renderProgress.call(this, [label, extra], progress);
      };
    }

    if (!missing("_renderInstall")) {
      const renderInstall = proto._renderInstall;
      const closing = new WeakSet();
      proto._renderInstall = function (...args) {
        // ESP Web Tools shows "Installation complete!" once its Improv check
        // is over (_client no longer undefined) and the port is open again.
        if (this._installState?.state !== "finished" || this._client === undefined) {
          return renderInstall.apply(this, args);
        }
        if (!closing.has(this)) {
          closing.add(this);
          setTimeout(() => {
            onFinished?.(this);
            this._closeDialog?.();
          }, 0);
        }
        // "Installing" is the dialog's own heading for this state.
        return ["Installing", this._renderProgress(t("install.written")), false];
      };
    }
  });
}

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

// The page's SDK token and the opt-in "Remember this token on this computer",
// with no DOM so Node can test it
// (esp32/tests/test_web_flasher.py):
//
// - The box is off by default; while it's off nothing is stored, and the
//   token lives only in the page until the tab closes.
// - While it's ticked, the token is saved in local storage (unencrypted)
//   once it has reached a gadget: after a successful send, or at the tick if
//   it already has. A reload fills it in with the box ticked.
// - Unticking, "Forget saved token" and Clear delete the saved copy (Clear
//   also drops the page's token).
// - Older versions of the page saved the token without asking, under
//   LEGACY_KEY: that copy is deleted at load, never adopted as consent.
//
// Nothing here logs or returns the token except token() for the send itself.

export const STORAGE_KEY = "muse-web-flasher:sdk-token";
export const LEGACY_KEY = "muse-sdk-token";

export function createTokenMemory(storage, isValid) {
  const get = () => {
    try {
      return storage?.getItem(STORAGE_KEY) ?? null;
    } catch {
      return null;
    }
  };
  const set = (value) => {
    try {
      storage?.setItem(STORAGE_KEY, value);
    } catch {
      // Storage full or blocked: nothing saved, and saved() says so.
    }
  };
  const remove = (key = STORAGE_KEY) => {
    try {
      storage?.removeItem(key);
    } catch {
      // Nothing to delete.
    }
  };

  remove(LEGACY_KEY);
  let stored = get();
  if (stored !== null && !isValid(stored)) {
    remove();
    stored = null;
  }
  let token = stored;
  let delivered = stored !== null;   // a saved token reached a gadget before
  let remember = stored !== null;

  const sync = () => {
    if (!remember) remove();
    else if (token && delivered) set(token);
  };

  return {
    token: () => token,
    remember: () => remember,
    saved: () => get() !== null,
    // A valid token typed or pasted in: the page uses it from now on; the saved
    // copy, if any, is replaced once the new one reaches a gadget.
    enter(value) {
      if (!isValid(value)) return false;
      if (value !== token) {
        token = value;
        delivered = false;
      }
      return true;
    },
    // The gadget stored `value` (sdk-token.js resolved).
    sent(value) {
      if (value !== token) return;
      delivered = true;
      sync();
    },
    setRemember(on) {
      remember = Boolean(on);
      sync();
    },
    // "Forget saved token": the saved copy goes, the page keeps its token.
    forget() {
      remember = false;
      sync();
    },
    // Clear: the page's token and the saved copy both go.
    clear() {
      token = null;
      delivered = false;
      remember = false;
      sync();
    },
  };
}

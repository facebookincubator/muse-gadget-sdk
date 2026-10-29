# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Client for the Muse device API: leased VM lookup."""

from __future__ import annotations

import http.client
import json
import logging
import platform
import urllib.error
import urllib.request
from pathlib import Path

from musegadget import __version__

log = logging.getLogger(__name__)

API_BASE = "https://api.muse.ai"
FETCH_PATH = "/fetch_vms"
REFRESH_PATH = "/device_token/refresh"


def api_root(api_url_v2: str = "") -> str:
    """The URL that API paths are appended to: ``api_url_v2``, or the Muse API.

    ``api_url`` is ignored. Only older firmware reads it, adding ``/hatch``.
    """
    return (api_url_v2 or API_BASE).rstrip("/")


def user_agent() -> str:
    details = []
    try:
        model = Path("/proc/device-tree/model").read_text(errors="replace")
        model = model.replace("\x00", "").strip()
        if model:
            details.append(model)
    except OSError:
        pass
    details.append(f"{platform.system()} {platform.machine()}".strip())
    return (
        f"musegadget/{__version__} ({'; '.join(d for d in details if d)}) "
        f"Python/{platform.python_version()}"
    )


def fetch_vms_with_status(
    access_token: str, root: str = api_root()
) -> tuple[list[dict], int | None]:
    """Leased VMs for the device token, plus the HTTP status if one arrived.

    Returns ``(vms, status)``; ``status`` is ``None`` when no HTTP response
    was obtained. A 401 means the device token was rejected, which a retry
    won't fix; a ``None`` status is a transport failure worth retrying.
    """
    req = urllib.request.Request(root + FETCH_PATH, method="GET")
    req.add_header("Authorization", f"Bearer {access_token}")
    req.add_header("X-API-Version", "1.0.0")
    req.add_header("User-Agent", user_agent())
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            status = resp.getcode()
            data = json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        log.error("VM fetch failed: HTTP %d", exc.code)
        return [], exc.code
    except (urllib.error.URLError, http.client.HTTPException, json.JSONDecodeError, OSError) as exc:
        log.error("VM fetch failed: %s", exc)
        return [], None

    if not isinstance(data, dict):
        log.error("VM fetch: unexpected response type %s", type(data).__name__)
        return [], status
    if data.get("error_title") or data.get("backend_error_code"):
        log.error("VM fetch error: %s %s",
                  data.get("error_title") or "", data.get("backend_error_code") or "")
        return [], status
    vm_list = data.get("vm_list")
    if not isinstance(vm_list, list):
        log.error("VM fetch: missing vm_list")
        return [], status

    vms = []
    for entry in vm_list:
        if not isinstance(entry, dict):
            continue
        vm_url = entry.get("vm_ws_url") or entry.get("vm_url")
        vm_token = entry.get("vm_auth_token")
        if vm_url and vm_token:
            vms.append({
                "vm_url": vm_url,
                "vm_auth_token": vm_token,
                "vm_name": entry.get("vm_name", ""),
                "vm_id": entry.get("vm_id", ""),
                "is_default": bool(entry.get("default", False)),
            })
    log.info("VM fetch: %d VMs", len(vms))
    return vms, status


def _post_refresh(
    auth_header: str, device_id: str, root: str, sdk_token: str | None,
) -> dict | None:
    body = {"device_id": device_id}
    if sdk_token:
        body["sdk_token"] = sdk_token
    req = urllib.request.Request(
        root + REFRESH_PATH,
        data=json.dumps(body).encode(),
        method="POST",
    )
    req.add_header("Authorization", auth_header)
    req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", user_agent())
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read())
    if isinstance(data, dict) and isinstance(data.get("payload"), dict):
        data = data["payload"]
    if isinstance(data, dict) and data.get("access_token") and data.get("refresh_token"):
        return data
    log.warning("token refresh response missing tokens")
    return None


def refresh_device_token(
    refresh_token: str, device_id: str, root: str = api_root(),
    sdk_token: str | None = None,
) -> tuple[dict | None, int | None]:
    """Rotate the device token pair with the refresh token.

    Returns ``(tokens, status)``: ``tokens`` is None on failure, and
    ``status`` is the HTTP status that ended the attempt, or None for a
    transport failure. A 401 means the pairing is gone and the device must be
    paired again.

    The access token is never presented, as on the ESP32: the server can
    accept it and answer 200 with replacement tokens that every endpoint then
    rejects, which would overwrite working credentials.
    """
    # Apps now hand over refresh tokens that already carry the hatch_refresh:
    # prefix; doubling it makes the server reject it. Same rule as the ESP32.
    raw_refresh = refresh_token.rsplit(":", 1)[-1]
    try:
        return _post_refresh(f"Bearer hatch_refresh:{raw_refresh}", device_id, root, sdk_token), 200
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            log.warning("token refresh rejected: device must be paired again")
        else:
            log.warning("token refresh failed: HTTP %d", exc.code)
        return None, exc.code
    except (urllib.error.URLError, http.client.HTTPException, json.JSONDecodeError, OSError) as exc:
        log.warning("token refresh failed: %s", exc)
        return None, None

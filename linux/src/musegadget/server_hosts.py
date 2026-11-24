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

"""The servers the device sends its account and VM credentials to.

Pairing names them (provision_v2's api_url, api_url_v2 and noise_host), so the
client takes only the ones the Muse app sends, exactly as it sends them: other
names under the same domains aren't Muse's API or its Noise ingress.
``MUSEGADGET_CUSTOM_SERVER_HOSTS=1`` (``config.custom_server_hosts()``) lifts
the check for development. A missing or empty value means the default and
always passes. The ESP32 firmware's ``muse_server_hosts.h`` takes the same.
"""

from __future__ import annotations

import logging

from musegadget import config

log = logging.getLogger(__name__)

# An API base: the origin alone, since API paths are appended to it.
API_URLS = (
    "https://api.muse.ai",        # api_url_v2, and the default
    "https://hatch-api.meta.ai",  # api_url, which only older firmware reads
)
# The Noise host: a bare hostname, dialled on port 443.
NOISE_HOST = "hatch.metaaivm.com"
NOISE_HOSTS = (NOISE_HOST,)


def _listed(value: object, allowed: tuple[str, ...]) -> bool:
    if not value:
        return True
    if not isinstance(value, str):
        return False
    return config.custom_server_hosts() or value in allowed


def api_allowed(url: object) -> bool:
    return _listed(url, API_URLS)


def noise_allowed(host: object) -> bool:
    return _listed(host, NOISE_HOSTS)


def noise_host(saved: object) -> str:
    """The Noise host to dial: ``saved``, or the default for none or one not allowed."""
    if not noise_allowed(saved):
        log.warning("saved Noise host not allowed; using the default")
        return NOISE_HOST
    return saved if isinstance(saved, str) and saved else NOISE_HOST

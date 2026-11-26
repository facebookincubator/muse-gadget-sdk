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

"""cJSON's source for a host build, through host_cjson.c (see there why).

The tests find it by themselves: CJSON_SOURCE_DIR if set, else this tree's managed_components/espressif__cjson/cJSON
(any idf.py build or reconfigure here fetches it), else the newest 1.7.x (the firmware's requirement) in the ESP-IDF
component manager's cache. Without one a test that needs it fails, saying how to get it: never a skip."""

import glob
import os
import re
import sys
from pathlib import Path

HOST_CJSON = Path(__file__).with_name('host_cjson.c')
ROOT = HOST_CJSON.resolve().parents[1]
MANAGED = ROOT / 'managed_components/espressif__cjson/cJSON'
REQUIRED = (1, 7)   # the firmware's espressif/cjson "~1.7" (main/idf_component.yml)


def component_cache():
    """Where ESP-IDF's component manager keeps its cache: IDF_COMPONENT_CACHE_PATH, else the platform's user cache."""
    if os.environ.get('IDF_COMPONENT_CACHE_PATH'):
        return Path(os.environ['IDF_COMPONENT_CACHE_PATH'])
    if sys.platform == 'darwin':
        base = Path('~/Library/Caches').expanduser()
    else:
        base = Path(os.environ.get('XDG_CACHE_HOME') or Path('~/.cache').expanduser())
    return base / 'Espressif' / 'ComponentManager'


def cached():
    """ESP-IDF's cached cJSON components the firmware's requirement allows, newest first."""
    found = []
    for d in glob.glob(str(component_cache() / '*' / 'espressif__cjson_*')):
        m = re.match(r'espressif__cjson_(\d+)\.(\d+)\.(\d+)', os.path.basename(d))
        if m and tuple(map(int, m.groups()[:2])) == REQUIRED:
            found.append((tuple(map(int, m.groups())), Path(d) / 'cJSON'))
    return [d for _, d in sorted(found, reverse=True)]


def complete(directory):
    return all((directory / name).is_file() for name in ('cJSON.c', 'cJSON.h'))


def cjson_dir():
    """The directory with cJSON.c and cJSON.h (where it would be, if there's none)."""
    if os.environ.get('CJSON_SOURCE_DIR'):
        return Path(os.environ['CJSON_SOURCE_DIR']).resolve()
    return next((d for d in [MANAGED] + cached() if complete(d)), MANAGED).resolve()


def require_cjson(directory):
    """`directory`, if it has cJSON's source; else the test fails with how to get it."""
    directory = Path(directory)
    if not complete(directory):
        raise AssertionError(
            f"cJSON's source isn't in {directory}: run idf.py reconfigure (or any idf.py build) in esp32/ once, "
            "which fetches it into managed_components/, or set CJSON_SOURCE_DIR to a directory with cJSON.c and "
            "cJSON.h")
    return directory


def cjson_source(directory):
    """Compiler arguments that build the cJSON.c in `directory`."""
    return ['-I', str(require_cjson(directory)), str(HOST_CJSON)]

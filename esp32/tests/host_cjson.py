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

"""cJSON's source for a host build, through host_cjson.c (see there why)."""

from pathlib import Path

HOST_CJSON = Path(__file__).with_name('host_cjson.c')


def cjson_source(directory):
    """Compiler arguments that build the cJSON.c in `directory`."""
    return ['-I', str(directory), str(HOST_CJSON)]

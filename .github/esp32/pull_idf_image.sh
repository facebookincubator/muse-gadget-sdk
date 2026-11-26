#!/bin/bash
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

# Pull the ESP-IDF image before espressif/esp-idf-ci-action runs, so the action
# finds it locally instead of pulling it itself with no time limit.
#   pull_idf_image.sh IMAGE TAG DIGEST
# Pulls IMAGE@DIGEST (pinned), each attempt under a time limit, and tags it
# IMAGE:TAG for the action. A stalled pull fails its attempt and the next one
# starts, instead of using up the job's time limit.
set -euo pipefail
image=${1:?image}; tag=${2:?tag}; digest=${3:?digest}
limit=${PULL_TIMEOUT_S:-420}; tries=${PULL_TRIES:-3}
for i in $(seq 1 "$tries"); do
    s=$(date +%s)
    if timeout "$limit" docker pull -q "$image@$digest" > /dev/null; then
        echo "pulled $image@$digest in $(( $(date +%s) - s )) s (attempt $i)"
        docker tag "$image@$digest" "$image:$tag"
        exit 0
    fi
    echo "::warning::pull of $image@$digest failed or took over $limit s (attempt $i of $tries)"
done
echo "::error::could not pull $image@$digest"
exit 1

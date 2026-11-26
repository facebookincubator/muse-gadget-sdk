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

# Linux host for the native_sim and BabbleSim tests. Build it for x86-64
# (--platform linux/amd64): BabbleSim images are 32-bit host programs.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc-multilib g++-multilib cmake ninja-build ccache \
    python3 python3-venv python3-pip git device-tree-compiler gperf file \
    wget xz-utils ca-certificates rsync make pkg-config \
    && rm -rf /var/lib/apt/lists/*
# Pinned: the versions the Zephyr v4.4.2 workspace was tested with.
RUN python3 -m venv /venv && /venv/bin/pip install -q west==1.5.0 pyelftools==0.33 \
    pyyaml==6.0.3 jsonschema==4.26.0 packaging==26.3 pykwalify==1.8.0 canopen==2.4.1 \
    natsort==8.4.0 psutil==7.2.2 ply==3.11 anytree==2.13.0 intelhex==2.3.0 \
    colorama==0.4.6 junitparser==5.0.3 junit2html==31.1.4 pytest==9.2.0 tabulate==0.10.0 \
    cryptography==49.0.0
ENV PATH=/venv/bin:$PATH ZEPHYR_TOOLCHAIN_VARIANT=host

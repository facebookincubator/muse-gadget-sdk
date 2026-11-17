/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Host stand-in for an ESP-IDF header, so ExecuTorch's ESP-NN delegate (in
 * the pytorch/executorch ESP-IDF component) builds off the chip, for host
 * builds of the runner such as the host tests. The runner drives one
 * delegate from one thread, with no PSRAM. These headers go away once
 * ExecuTorch builds the delegate for other hosts itself.
 */

#pragma once

/* No ESP-IDF target: esp-nn builds its portable C kernels, and the
 * delegate's options (CONFIG_ESPNN_DELEGATE_*) stay off. */

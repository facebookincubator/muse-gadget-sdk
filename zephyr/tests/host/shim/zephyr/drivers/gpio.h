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
 */

/* Host shim: no devicetree, so a board without sw0 (the button's GPIO code
 * compiles out; its events come through input_cb). */

#ifndef HOST_SHIM_GPIO_H_
#define HOST_SHIM_GPIO_H_

#define DT_ALIAS(alias)    alias
#define DT_NODE_EXISTS(id) 0

#endif

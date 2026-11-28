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

/*
 * The image's SDK token slot (CONFIG_MG_SDK_TOKEN_SLOT; mg_setup.h): the
 * marker, then NULs. An updater that knows the owner's token finds the
 * marker in the signed image, writes the token and NULs over the whole slot
 * and signs the image again. Kept in its own file so mg_setup.c only ever
 * reads it at run time.
 */

#include "mg_setup.h"

__attribute__((used)) const char mg_sdk_token_slot[MG_SDK_TOKEN_SLOT_LEN] =
	MG_SDK_TOKEN_SLOT_MARKER;

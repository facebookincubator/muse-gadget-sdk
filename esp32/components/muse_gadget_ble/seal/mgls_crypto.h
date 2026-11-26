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
#pragma once

/*
 * mg_session_seal.c (portable/mg_device_session, vendored unchanged) includes
 * "mgls_crypto.h" for its two primitives. This forwards to the portable
 * header without putting portable/mg_link_setup/include on the include path,
 * whose mg_token_proof.h would shadow this component's own.
 */
#include "../../../../portable/mg_link_setup/include/mgls_crypto.h"

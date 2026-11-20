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

#include <stdbool.h>

#define VM_API_MAX_VMS 8
#define VM_API_ERR_FAILED -1
#define VM_API_ERR_AUTH   -2
// A device-token mint or refresh refused with a 401: the token service says
// the credential will never work again (revoked, replaced or already used).
#define VM_API_ERR_REVOKED -3
#define VM_API_DEFAULT_BASE_URL "https://api.muse.ai"

typedef struct {
    char *vm_url;
    char *vm_auth_token;
    char *vm_name;
    char *vm_id;
    bool  is_default;
} vm_info_t;

typedef struct {
    char *access_token;
    char *refresh_token;
} vm_device_tokens_t;

typedef enum {
    VM_TOKEN_REFRESH_NONE = 0,
    VM_TOKEN_REFRESH_ACCESS_TOKEN,
    VM_TOKEN_REFRESH_REFRESH_TOKEN,
} vm_token_refresh_method_t;

// Override the API base, provisioned as api_url_v2. API paths are appended
// as-is. NULL or empty restores the compiled default.
void vm_api_set_base_url(const char *url);

// SDK token sent with device-token mint and refresh. NULL or empty sends none.
void vm_api_set_sdk_token(const char *sdk_token);

// Fetch VMs using the provided device access token.
// Writes up to max entries to out and returns the count, VM_API_ERR_AUTH
// on 401/403, or VM_API_ERR_FAILED on other errors.
// Caller must vm_list_free() the array entries when done.
int vm_api_fetch_vms(const char *access_token, vm_info_t *out, int max);

// Exchange a user/session token for a short-lived device token pair.
// Returns 0 on success, VM_API_ERR_REVOKED on 401, VM_API_ERR_AUTH on 403, or
// VM_API_ERR_FAILED.
int vm_api_mint_device_token(const char *access_token, const char *device_id,
                             vm_device_tokens_t *out);

// Refresh an existing device token pair. Tries access_token first, then
// refresh_token with the service-specific prefix when the access token is
// rejected. Returns 0 on success and sets *method when non-NULL; otherwise
// VM_API_ERR_REVOKED when the last leg tried got a 401, VM_API_ERR_AUTH on a
// 403, or VM_API_ERR_FAILED.
//
// Pass access_token = NULL to SKIP the access-token leg and go straight to the
// refresh token. Callers that just had the access token refused should do
// this: presenting it again cannot succeed, and on the first call after an idle
// period the server may accept it and mint a token that is dead on arrival.
int vm_api_refresh_device_token(const char *access_token,
                                const char *refresh_token,
                                const char *device_id,
                                vm_device_tokens_t *out,
                                vm_token_refresh_method_t *method);

void vm_device_tokens_free(vm_device_tokens_t *tokens);

void vm_list_free(vm_info_t *vms, int count);

const vm_info_t *vm_find_default(const vm_info_t *vms, int count);
const vm_info_t *vm_find_by_url(const vm_info_t *vms, int count, const char *url);

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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "muse_account_api.h"
#include "vm_api.h"
#include "vm_connect.h"

#define MAX_FAKE_RESPONSES 16
#define MAX_FAKE_REQUESTS 16

void *esp_crt_bundle_attach = NULL;

typedef struct {
    esp_err_t err;
    int status;
    const char *body;
    size_t split_at;
} fake_response_t;

typedef struct {
    char url[256];
    esp_http_client_method_t method;
    char authorization[2300];
    char api_version[64];
    char content_type[64];
    char body[1024];
} recorded_request_t;

struct fake_esp_http_client {
    esp_http_client_config_t config;
    int status;
    recorded_request_t request;
};

static fake_response_t g_responses[MAX_FAKE_RESPONSES];
static int g_response_count;
static int g_response_index;
static recorded_request_t g_requests[MAX_FAKE_REQUESTS];
static int g_request_count;
static const char *g_redirect_url;
static bool g_redirect_scheduled;

static void fail_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) fail_at(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

static void copy_cstr(char *dst, size_t cap, const char *src) {
    int n = snprintf(dst, cap, "%s", src ? src : "");
    CHECK(n >= 0 && (size_t)n < cap, "string too long for fake buffer");
}

static void copy_bytes(char *dst, size_t cap, const char *src, int len) {
    CHECK(len >= 0, "negative body length");
    CHECK((size_t)len < cap, "body too long for fake buffer");
    if (len > 0) {
        memcpy(dst, src, (size_t)len);
    }
    dst[len] = '\0';
}

static void fake_reset(void) {
    memset(g_responses, 0, sizeof(g_responses));
    memset(g_requests, 0, sizeof(g_requests));
    g_response_count = 0;
    g_response_index = 0;
    g_request_count = 0;
    g_redirect_url = NULL;
    g_redirect_scheduled = false;
}

esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, int len) {
    snprintf(url, len, "%s", client->request.url);
    return ESP_OK;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key) {
    CHECK(!strcmp(key, "Authorization"), "unexpected header deletion");
    client->request.authorization[0] = 0;
    return ESP_OK;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client) {
    CHECK(g_redirect_url != NULL, "missing redirect target");
    copy_cstr(client->request.url, sizeof(client->request.url), g_redirect_url);
    g_redirect_scheduled = true;
    return ESP_OK;
}

static void fake_queue_response_split(int status, const char *body, size_t split_at) {
    CHECK(g_response_count < MAX_FAKE_RESPONSES, "too many fake responses");
    g_responses[g_response_count++] = (fake_response_t){
        .err = ESP_OK,
        .status = status,
        .body = body,
        .split_at = split_at,
    };
}

static void fake_queue_response(int status, const char *body) {
    fake_queue_response_split(status, body, 3);
}

static void fake_queue_error(esp_err_t err, int status) {
    CHECK(g_response_count < MAX_FAKE_RESPONSES, "too many fake responses");
    g_responses[g_response_count++] = (fake_response_t){
        .err = err,
        .status = status,
        .body = NULL,
        .split_at = 0,
    };
}

static void emit_body(struct fake_esp_http_client *client, const char *body,
                      size_t offset, size_t len) {
    if (len == 0 || !client->config.event_handler) return;
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_DATA,
        .user_data = client->config.user_data,
        .data = (void *)(body + offset),
        .data_len = (int)len,
    };
    CHECK(client->config.event_handler(&event) == ESP_OK, "event handler failed");
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    CHECK(config != NULL, "config is required");
    struct fake_esp_http_client *client = calloc(1, sizeof(*client));
    CHECK(client != NULL, "calloc client failed");
    client->config = *config;
    copy_cstr(client->request.url, sizeof(client->request.url), config->url);
    client->request.method = config->method;
    return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value) {
    CHECK(client != NULL, "client is required");
    if (strcmp(key, "Authorization") == 0) {
        copy_cstr(client->request.authorization, sizeof(client->request.authorization), value);
    } else if (strcmp(key, "X-API-Version") == 0) {
        copy_cstr(client->request.api_version, sizeof(client->request.api_version), value);
    } else if (strcmp(key, "Content-Type") == 0) {
        copy_cstr(client->request.content_type, sizeof(client->request.content_type), value);
    }
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client,
                                         const char *data, int len) {
    CHECK(client != NULL, "client is required");
    copy_bytes(client->request.body, sizeof(client->request.body), data, len);
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
    CHECK(client != NULL, "client is required");
    CHECK(g_response_index < g_response_count, "unexpected HTTP request");
    CHECK(g_request_count < MAX_FAKE_REQUESTS, "too many fake requests");

    fake_response_t response = g_responses[g_response_index++];
    client->status = response.status;
    g_requests[g_request_count++] = client->request;

    if (g_redirect_url) {
        if (client->config.disable_auto_redirect) {
            esp_http_client_event_t event = {
                .event_id = HTTP_EVENT_REDIRECT, .client = client,
                .user_data = client->config.user_data,
            };
            // ESP-IDF ignores the callback return value for this event.
            client->config.event_handler(&event);
        } else {
            esp_http_client_set_redirection(client);
        }
        if (g_redirect_scheduled) g_requests[g_request_count++] = client->request;
    }

    if (response.err != ESP_OK) {
        return response.err;
    }

    if (response.body) {
        size_t len = strlen(response.body);
        if (response.split_at > 0 && response.split_at < len) {
            emit_body(client, response.body, 0, response.split_at);
            emit_body(client, response.body, response.split_at, len - response.split_at);
        } else {
            emit_body(client, response.body, 0, len);
        }
    }
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    CHECK(client != NULL, "client is required");
    return client->status;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    free(client);
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err) {
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

static void expect_token_pair(const vm_device_tokens_t *tokens,
                              const char *access, const char *refresh) {
    CHECK(tokens->access_token != NULL, "missing access token");
    CHECK(tokens->refresh_token != NULL, "missing refresh token");
    CHECK(strcmp(tokens->access_token, access) == 0, "access token mismatch");
    CHECK(strcmp(tokens->refresh_token, refresh) == 0, "refresh token mismatch");
}

static void expect_empty_tokens(const vm_device_tokens_t *tokens) {
    CHECK(tokens->access_token == NULL, "access token should be NULL");
    CHECK(tokens->refresh_token == NULL, "refresh token should be NULL");
}

static void expect_url(const recorded_request_t *request, const char *suffix) {
    const char *base = "https://api.muse.ai";
    char expected[256];
    snprintf(expected, sizeof(expected), "%s%s", base, suffix);
    CHECK(strcmp(request->url, expected) == 0, "unexpected url: %s", request->url);
}

static void test_mint_success_uses_headers_and_escaped_body(void) {
    vm_device_tokens_t tokens = {0};
    fake_queue_response(200, "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}");

    int rc = vm_api_mint_device_token("old-token", "link\"id\\one", &tokens);

    CHECK(rc == 0, "mint rc=%d", rc);
    expect_token_pair(&tokens, "new-a", "new-r");
    CHECK(g_request_count == 1, "request count=%d", g_request_count);
    expect_url(&g_requests[0], "/device_token/mint");
    CHECK(g_requests[0].method == HTTP_METHOD_POST, "mint should POST");
    CHECK(strcmp(g_requests[0].authorization, "Bearer old-token") == 0,
          "unexpected Authorization: %s", g_requests[0].authorization);
    CHECK(strcmp(g_requests[0].api_version, "1.0.0") == 0, "missing API version");
    CHECK(strcmp(g_requests[0].content_type, "application/json") == 0,
          "missing JSON content type");
    CHECK(strcmp(g_requests[0].body, "{\"device_id\":\"link\\\"id\\\\one\"}") == 0,
          "device body was not JSON escaped: %s", g_requests[0].body);

    vm_device_tokens_free(&tokens);
    expect_empty_tokens(&tokens);
    vm_device_tokens_free(&tokens);
}

static void test_mint_bad_inputs_do_not_send_http(void) {
    vm_device_tokens_t tokens = {0};

    CHECK(vm_api_mint_device_token(NULL, "id", &tokens) == VM_API_ERR_FAILED,
          "NULL access token should fail");
    CHECK(vm_api_mint_device_token("", "id", &tokens) == VM_API_ERR_FAILED,
          "empty access token should fail");
    CHECK(vm_api_mint_device_token("token", NULL, &tokens) == VM_API_ERR_FAILED,
          "NULL device id should fail");
    CHECK(vm_api_mint_device_token("token", "", &tokens) == VM_API_ERR_FAILED,
          "empty device id should fail");
    CHECK(vm_api_mint_device_token("token", "id", NULL) == VM_API_ERR_FAILED,
          "NULL out should fail");
    CHECK(g_request_count == 0, "bad inputs should not send HTTP");
}

static void test_oversized_authorization_does_not_send_http(void) {
    char long_token[2100];
    memset(long_token, 'a', sizeof(long_token) - 1);
    long_token[sizeof(long_token) - 1] = '\0';
    vm_device_tokens_t tokens = {0};

    CHECK(vm_api_mint_device_token(long_token, "id", &tokens) == VM_API_ERR_FAILED,
          "oversized token should fail");
    CHECK(g_request_count == 0, "oversized token should not send HTTP");
    expect_empty_tokens(&tokens);
}

static void expect_mint_response_failure(const char *body) {
    fake_reset();
    fake_queue_response(200, body);
    vm_device_tokens_t tokens = {0};
    int rc = vm_api_mint_device_token("token", "id", &tokens);
    CHECK(rc == VM_API_ERR_FAILED, "mint should fail for body: %s", body);
    CHECK(g_request_count == 1, "mint failure should send one request");
    expect_empty_tokens(&tokens);
    vm_device_tokens_free(&tokens);
}

static void test_mint_rejects_malformed_or_partial_token_responses(void) {
    expect_mint_response_failure("{\"access_token\":\"a\"}");
    expect_mint_response_failure("{\"access_token\":\"a\",\"refresh_token\":\"\"}");
    expect_mint_response_failure("{\"access_token\":\"\",\"refresh_token\":\"r\"}");
    expect_mint_response_failure("[]");
    expect_mint_response_failure("not json");
}

static void test_mint_auth_failure_is_not_retried(void) {
    vm_device_tokens_t tokens = {0};
    fake_queue_response(401, "");

    int rc = vm_api_mint_device_token("token", "id", &tokens);

    CHECK(rc == VM_API_ERR_REVOKED, "mint 401 rc=%d", rc);
    CHECK(g_request_count == 1, "auth failure should not retry");
    expect_empty_tokens(&tokens);

    fake_reset();
    fake_queue_response(403, "");
    rc = vm_api_mint_device_token("token", "id", &tokens);
    CHECK(rc == VM_API_ERR_AUTH, "mint 403 rc=%d", rc);
    CHECK(g_request_count == 1, "auth failure should not retry");
    expect_empty_tokens(&tokens);
}

static void test_refresh_success_via_access_token(void) {
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(200, "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}");

    int rc = vm_api_refresh_device_token("old-a", "old-r", "link-123", &tokens, &method);

    CHECK(rc == 0, "refresh rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_ACCESS_TOKEN, "method=%d", method);
    expect_token_pair(&tokens, "new-a", "new-r");
    CHECK(g_request_count == 1, "request count=%d", g_request_count);
    expect_url(&g_requests[0], "/device_token/refresh");
    CHECK(strcmp(g_requests[0].authorization, "Bearer old-a") == 0,
          "unexpected access refresh auth: %s", g_requests[0].authorization);
    CHECK(strcmp(g_requests[0].body, "{\"device_id\":\"link-123\"}") == 0,
          "unexpected refresh body: %s", g_requests[0].body);

    vm_device_tokens_free(&tokens);
}

static void test_refresh_and_mint_carry_sdk_token(void) {
    vm_device_tokens_t tokens = {0};
    vm_api_set_sdk_token("mgst_token");
    fake_queue_response(200, "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}");
    int rc = vm_api_refresh_device_token("old-a", "old-r", "link-123", &tokens, NULL);
    CHECK(rc == 0, "refresh rc=%d", rc);
    CHECK(strcmp(g_requests[0].body,
                 "{\"device_id\":\"link-123\",\"sdk_token\":\"mgst_token\"}") == 0,
          "unexpected refresh body: %s", g_requests[0].body);
    vm_device_tokens_free(&tokens);

    fake_reset();
    fake_queue_response(200, "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}");
    rc = vm_api_mint_device_token("token", "link-123", &tokens);
    CHECK(rc == 0, "mint rc=%d", rc);
    CHECK(strcmp(g_requests[0].body,
                 "{\"device_id\":\"link-123\",\"sdk_token\":\"mgst_token\"}") == 0,
          "unexpected mint body: %s", g_requests[0].body);
    vm_device_tokens_free(&tokens);
    vm_api_set_sdk_token(NULL);
}

static void test_refresh_falls_back_to_refresh_token_on_401(void) {
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(401, "");
    fake_queue_response(200, "{\"access_token\":\"fallback-a\",\"refresh_token\":\"fallback-r\"}");

    int rc = vm_api_refresh_device_token("expired-a", "valid-r", "link-123",
                                         &tokens, &method);

    CHECK(rc == 0, "refresh fallback rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_REFRESH_TOKEN, "method=%d", method);
    expect_token_pair(&tokens, "fallback-a", "fallback-r");
    CHECK(g_request_count == 2, "fallback request count=%d", g_request_count);
    CHECK(strcmp(g_requests[0].authorization, "Bearer expired-a") == 0,
          "first auth mismatch");
    CHECK(strcmp(g_requests[1].authorization, "Bearer hatch_refresh:valid-r") == 0,
          "fallback auth mismatch: %s", g_requests[1].authorization);

    vm_device_tokens_free(&tokens);
}

static void test_refresh_does_not_fallback_on_403(void) {
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(403, "");

    int rc = vm_api_refresh_device_token("rejected-a", "valid-r", "link-123",
                                         &tokens, &method);

    CHECK(rc == VM_API_ERR_AUTH, "403 refresh rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_NONE, "method should remain none");
    CHECK(g_request_count == 1, "403 should not use refresh fallback");
    expect_empty_tokens(&tokens);
}

static void test_refresh_no_fallback_without_refresh_token(void) {
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(401, "");

    int rc = vm_api_refresh_device_token("expired-a", "", "link-123", &tokens, &method);

    CHECK(rc == VM_API_ERR_REVOKED, "refresh rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_NONE, "method should remain none");
    CHECK(g_request_count == 1, "missing refresh token should not fallback");
    expect_empty_tokens(&tokens);
}

static void test_refresh_fallback_auth_failure_leaves_output_empty(void) {
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(401, "");
    fake_queue_response(401, "");

    int rc = vm_api_refresh_device_token("expired-a", "expired-r", "link-123",
                                         &tokens, &method);

    CHECK(rc == VM_API_ERR_REVOKED, "refresh rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_NONE, "method should remain none");
    CHECK(g_request_count == 2, "expected access + fallback attempts");
    expect_empty_tokens(&tokens);
}

// Only the leg that answered last decides: a 403 on the refresh token is
// VM_API_ERR_AUTH even after a 401 on the access token, and a 401 on the
// refresh token alone (access leg skipped) is VM_API_ERR_REVOKED.
static void test_refresh_401_and_403_on_the_refresh_leg(void) {
    vm_device_tokens_t tokens = {0};
    fake_queue_response(401, "");
    fake_queue_response(403, "");
    int rc = vm_api_refresh_device_token("expired-a", "refused-r", "link-123",
                                         &tokens, NULL);
    CHECK(rc == VM_API_ERR_AUTH, "refresh-leg 403 rc=%d", rc);
    CHECK(g_request_count == 2, "expected access + fallback attempts");
    expect_empty_tokens(&tokens);

    fake_reset();
    fake_queue_response(401, "");
    rc = vm_api_refresh_device_token(NULL, "revoked-r", "link-123", &tokens, NULL);
    CHECK(rc == VM_API_ERR_REVOKED, "refresh-leg 401 rc=%d", rc);
    CHECK(g_request_count == 1, "expected the refresh leg only");
    expect_empty_tokens(&tokens);
}

static void test_refresh_fallback_400_is_not_retried(void) {
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(401, "");
    fake_queue_response(400, "{\"error\":\"invalid refresh token\"}");

    int rc = vm_api_refresh_device_token("expired-a", "rejected-r", "link-123",
                                         &tokens, &method);

    CHECK(rc == VM_API_ERR_FAILED, "refresh rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_NONE, "method should remain none");
    CHECK(g_request_count == 2, "HTTP 400 should not retry, requests=%d",
          g_request_count);
    expect_empty_tokens(&tokens);
}

static void test_refresh_bad_inputs_do_not_send_http(void) {
    vm_device_tokens_t tokens = {0};

    // NULL/empty access_token is not a bad input: vm_api.h documents it as
    // "skip the access-token leg", for callers whose access token was just
    // refused. Those cases go straight to the refresh leg and are covered by
    // test_refresh_skips_access_leg_when_asked below.
    CHECK(vm_api_refresh_device_token("a", "r", NULL, &tokens, NULL) == VM_API_ERR_FAILED,
          "NULL device id should fail");
    CHECK(vm_api_refresh_device_token("a", "r", "", &tokens, NULL) == VM_API_ERR_FAILED,
          "empty device id should fail");
    CHECK(vm_api_refresh_device_token("a", "r", "id", NULL, NULL) == VM_API_ERR_FAILED,
          "NULL out should fail");
    CHECK(g_request_count == 0, "bad refresh inputs should not send HTTP");
}

static void test_refresh_skips_access_leg_when_asked(void) {
    // Passing NULL (or "") for access_token means the caller already had it
    // refused; re-presenting it can mint a token that is dead on arrival, so
    // vm_api goes straight to the refresh leg.
    vm_device_tokens_t tokens = {0};
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    fake_queue_response(200, "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}");

    int rc = vm_api_refresh_device_token(NULL, "hatch_refresh:r", "link-123",
                                         &tokens, &method);

    CHECK(rc == 0, "skip-access refresh rc=%d", rc);
    CHECK(method == VM_TOKEN_REFRESH_REFRESH_TOKEN, "method=%d", (int)method);
    CHECK(g_request_count == 1, "should send exactly the refresh leg, sent %d",
          g_request_count);
    CHECK(strcmp(g_requests[0].authorization, "Bearer hatch_refresh:r") == 0,
          "refresh leg auth: %s", g_requests[0].authorization);
    vm_device_tokens_free(&tokens);
}

static void test_refresh_with_neither_credential_sends_no_http(void) {
    vm_device_tokens_t tokens = {0};
    CHECK(vm_api_refresh_device_token(NULL, NULL, "id", &tokens, NULL)
          == VM_API_ERR_FAILED, "no credential should fail");
    CHECK(vm_api_refresh_device_token("", "", "id", &tokens, NULL)
          == VM_API_ERR_FAILED, "empty credentials should fail");
    CHECK(g_request_count == 0, "no credential should not send HTTP");
}

static void test_refresh_without_access_uses_refresh_token_directly(void) {
    const char *access_values[] = {NULL, ""};
    for (unsigned i = 0; i < sizeof(access_values) / sizeof(*access_values); i++) {
        fake_reset();
        vm_device_tokens_t tokens = {0};
        vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
        fake_queue_response(200, "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}");
        CHECK(vm_api_refresh_device_token(access_values[i], "r", "id", &tokens, &method) == 0,
              "refresh-only request should succeed");
        CHECK(g_request_count == 1, "refresh-only request must skip the access leg");
        CHECK(strcmp(g_requests[0].authorization, "Bearer hatch_refresh:r") == 0,
              "refresh-only authorization mismatch");
        CHECK(method == VM_TOKEN_REFRESH_REFRESH_TOKEN, "refresh-only method mismatch");
        expect_token_pair(&tokens, "new-a", "new-r");
        vm_device_tokens_free(&tokens);
    }
}

static void test_fetch_vms_success_filters_entries_and_uses_helpers(void) {
    const char *body =
        "{\"vm_list\":["
        "{\"vm_ws_url\":\"wss://one\",\"vm_auth_token\":\"tok1\",\"vm_name\":\"one\",\"vm_id\":\"id-one\",\"default\":false},"
        "{\"vm_url\":\"wss://two\",\"vm_auth_token\":\"tok2\",\"default\":true},"
        "{\"vm_ws_url\":\"wss://missing-token\"},"
        "{\"vm_auth_token\":\"missing-url\"}"
        "]}";
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response_split(200, body, 5);

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);

    CHECK(count == 2, "VM count=%d", count);
    CHECK(g_request_count == 1, "fetch request count=%d", g_request_count);
    expect_url(&g_requests[0], "/fetch_vms");
    CHECK(g_requests[0].method == HTTP_METHOD_GET, "fetch should GET");
    CHECK(strcmp(g_requests[0].authorization, "Bearer access") == 0,
          "fetch auth mismatch: %s", g_requests[0].authorization);
    CHECK(strcmp(vms[0].vm_url, "wss://one") == 0, "first VM url mismatch");
    CHECK(strcmp(vms[0].vm_auth_token, "tok1") == 0, "first VM token mismatch");
    CHECK(strcmp(vms[0].vm_name, "one") == 0, "first VM name mismatch");
    CHECK(vms[0].vm_id != NULL && strcmp(vms[0].vm_id, "id-one") == 0,
          "first VM id mismatch");
    CHECK(!vms[0].is_default, "first VM should not be default");
    CHECK(strcmp(vms[1].vm_url, "wss://two") == 0, "second VM url mismatch");
    CHECK(strcmp(vms[1].vm_auth_token, "tok2") == 0, "second VM token mismatch");
    CHECK(strcmp(vms[1].vm_name, "") == 0, "missing VM name should become empty");
    CHECK(vms[1].vm_id == NULL, "missing VM id should be NULL");
    CHECK(vms[1].is_default, "second VM should be default");
    CHECK(vm_find_default(vms, count) == &vms[1], "default helper mismatch");
    CHECK(vm_find_by_url(vms, count, "wss://one") == &vms[0], "find by url mismatch");
    CHECK(vm_find_by_url(vms, count, "wss://missing") == NULL, "missing url found");

    vm_list_free(vms, count);
    CHECK(vms[0].vm_url == NULL && vms[0].vm_auth_token == NULL && vms[0].vm_name == NULL,
          "vm_list_free should clear first VM");
    CHECK(vms[0].vm_id == NULL, "vm_list_free should clear vm_id");
    vm_list_free(vms, count);
}

static void test_fetch_vms_respects_max(void) {
    const char *body =
        "{\"vm_list\":["
        "{\"vm_ws_url\":\"wss://one\",\"vm_auth_token\":\"tok1\"},"
        "{\"vm_ws_url\":\"wss://two\",\"vm_auth_token\":\"tok2\"}"
        "]}";
    vm_info_t vms[1] = {0};
    fake_queue_response(200, body);

    int count = vm_api_fetch_vms("access", vms, 1);

    CHECK(count == 1, "max=1 count=%d", count);
    CHECK(strcmp(vms[0].vm_url, "wss://one") == 0, "max first VM mismatch");
    vm_list_free(vms, count);
}

static void test_fetch_vms_auth_failure_is_not_retried(void) {
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(403, "");

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);

    CHECK(count == VM_API_ERR_AUTH, "fetch auth rc=%d", count);
    CHECK(g_request_count == 1, "auth fetch should not retry");
}

static void test_fetch_vms_retries_transient_failures_and_resets_response(void) {
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(500, "{\"error\":\"server\"}");
    fake_queue_error(ESP_FAIL, 400);
    fake_queue_response(200, "{\"vm_list\":[{\"vm_ws_url\":\"wss://ok\",\"vm_auth_token\":\"tok\"}]}");

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);

    CHECK(count == 1, "retry fetch count=%d", count);
    CHECK(g_request_count == 3, "retry request count=%d", g_request_count);
    CHECK(strcmp(vms[0].vm_url, "wss://ok") == 0, "retry VM url mismatch");
    vm_list_free(vms, count);
}

static void test_fetch_vms_retries_temporary_http_errors(void) {
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(408, "");
    fake_queue_response(429, "");
    fake_queue_response(503, "");
    fake_queue_response(200, "{\"vm_list\":[{\"vm_ws_url\":\"wss://ok\",\"vm_auth_token\":\"tok\"}]}");

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);

    CHECK(count == 1, "retry fetch count=%d", count);
    CHECK(g_request_count == 4, "temporary HTTP retry count=%d", g_request_count);
    vm_list_free(vms, count);
}

static void expect_fetch_failure_for_body(const char *body) {
    fake_reset();
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(200, body);
    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
    CHECK(count == VM_API_ERR_FAILED, "fetch should fail for body: %s", body);
    CHECK(g_request_count == 1, "bad fetch body should send one request");
}

static void test_fetch_vms_rejects_bad_json_shapes(void) {
    expect_fetch_failure_for_body("{\"not\":\"an array\"}");
    expect_fetch_failure_for_body("[{\"vm_ws_url\":\"wss://x\",\"vm_auth_token\":\"t\"}]");
    expect_fetch_failure_for_body("not json");
}

static void test_set_base_url_changes_fetch_target(void) {
    const struct {
        const char *override;
        const char *base;
    } cases[] = {
        {"https://api.muse.ai", "https://api.muse.ai"},
        {"https://custom-api.example.com", "https://custom-api.example.com"},
        {NULL, "https://api.muse.ai"},
        {"https://custom-api.example.com", "https://custom-api.example.com"},
        {"", "https://api.muse.ai"},
    };
    const char *paths[] = {
        "/fetch_vms", "/device_token/mint",
        "/device_token/refresh", "/device_token/refresh",
    };
    const char *auth[] = {
        "Bearer access", "Bearer access",
        "Bearer access", "Bearer hatch_refresh:refresh",
    };
    const char *vm_body =
        "{\"vm_list\":[{\"vm_id\":\"vm-one\",\"vm_ws_url\":\"wss://v\",\"vm_auth_token\":\"t\"}]}";
    const char *token_body =
        "{\"access_token\":\"new-a\",\"refresh_token\":\"new-r\"}";

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fake_reset();
        vm_api_set_base_url(cases[i].override);
        fake_queue_response(200, vm_body);
        fake_queue_response(200, token_body);
        fake_queue_response(401, "");
        fake_queue_response(200, token_body);

        vm_info_t vms[VM_API_MAX_VMS] = {0};
        int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
        CHECK(count == 1, "base %s: fetch count=%d", cases[i].base, count);
        vm_list_free(vms, count);

        vm_device_tokens_t tokens = {0};
        int rc = vm_api_mint_device_token("access", "link-id", &tokens);
        CHECK(rc == 0, "base %s: mint rc=%d", cases[i].base, rc);
        expect_token_pair(&tokens, "new-a", "new-r");
        vm_device_tokens_free(&tokens);

        vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
        rc = vm_api_refresh_device_token("access", "refresh", "link-id",
                                         &tokens, &method);
        CHECK(rc == 0, "base %s: refresh rc=%d", cases[i].base, rc);
        CHECK(method == VM_TOKEN_REFRESH_REFRESH_TOKEN,
              "base %s: refresh method=%d", cases[i].base, method);
        expect_token_pair(&tokens, "new-a", "new-r");
        vm_device_tokens_free(&tokens);

        CHECK(g_request_count == 4, "base %s: request count=%d",
              cases[i].base, g_request_count);
        for (size_t j = 0; j < sizeof(paths) / sizeof(paths[0]); ++j) {
            char expected[256];
            snprintf(expected, sizeof(expected), "%s%s",
                     cases[i].base, paths[j]);
            CHECK(strcmp(g_requests[j].url, expected) == 0,
                  "expected %s, got %s", expected, g_requests[j].url);
            CHECK(g_requests[j].method == (j == 0 ? HTTP_METHOD_GET : HTTP_METHOD_POST),
                  "unexpected method for %s", expected);
            CHECK(strcmp(g_requests[j].authorization, auth[j]) == 0,
                  "unexpected authorization for %s", expected);
        }
    }

    // Muse voice boards have their own account client, with no base override.
    fake_reset();
    fake_queue_response(200, vm_body);
    muse_hatch_vm_t vm = {0};
    CHECK(muse_hatch_api_find_vm("access", "", &vm) == 0,
          "Muse account lookup failed");
    CHECK(g_request_count == 1, "Muse account request count=%d", g_request_count);
    expect_url(&g_requests[0], "/fetch_vms");
    CHECK(g_requests[0].method == HTTP_METHOD_GET, "Muse account lookup should GET");
    CHECK(strcmp(g_requests[0].authorization, "Bearer access") == 0,
          "Muse account authorization mismatch");
    CHECK(strcmp(vm.vm_id, "vm-one") == 0 && strcmp(vm.vm_token, "t") == 0,
          "Muse account VM credential mismatch");
    free(vm.vm_token);
}

// ---- Per-VM credential isolation ------------------------------------------
//
// Preserve each fetch_vms entry's token independently and byte-for-byte so
// selecting one VM cannot accidentally select another VM's credential.

static void test_each_entry_keeps_its_own_token(void) {
    const char *body =
        "{\"vm_list\":["
        "{\"vm_ws_url\":\"wss://a.vm.example/\",\"vm_auth_token\":\"s0:tok-A\",\"vm_id\":\"vm-a\"},"
        "{\"vm_ws_url\":\"wss://b.vm.example/\",\"vm_auth_token\":\"s0:tok-B\",\"vm_id\":\"vm-b\"},"
        "{\"vm_ws_url\":\"wss://c.vm.example/\",\"vm_auth_token\":\"s0:tok-C\",\"vm_id\":\"vm-c\"}"
        "]}";
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(200, body);

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
    CHECK(count == 3, "count=%d", count);

    CHECK(strcmp(vms[0].vm_id, "vm-a") == 0 && strcmp(vms[0].vm_auth_token, "s0:tok-A") == 0,
          "entry 0 mispaired: %s / %s", vms[0].vm_id, vms[0].vm_auth_token);
    CHECK(strcmp(vms[1].vm_id, "vm-b") == 0 && strcmp(vms[1].vm_auth_token, "s0:tok-B") == 0,
          "entry 1 mispaired: %s / %s", vms[1].vm_id, vms[1].vm_auth_token);
    CHECK(strcmp(vms[2].vm_id, "vm-c") == 0 && strcmp(vms[2].vm_auth_token, "s0:tok-C") == 0,
          "entry 2 mispaired: %s / %s", vms[2].vm_id, vms[2].vm_auth_token);

    // Separate allocations, so nothing can alias one VM's token onto another.
    CHECK(vms[0].vm_auth_token != vms[1].vm_auth_token
          && vms[1].vm_auth_token != vms[2].vm_auth_token,
          "tokens should not be shared between entries");

    vm_list_free(vms, count);
}

static void test_the_token_is_stored_byte_for_byte(void) {
    // The client must use *exactly* the value fetch_vms returned. Nothing in
    // the parser may trim, normalise or re-encode it.
    const char *token = "s0:AAAA+bbb/ccc==.dd-ee_ff.~gg";
    const char *body =
        "{\"vm_list\":[{\"vm_ws_url\":\"wss://a.vm.example/\","
        "\"vm_auth_token\":\"s0:AAAA+bbb/ccc==.dd-ee_ff.~gg\"}]}";
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(200, body);

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
    CHECK(count == 1, "count=%d", count);
    CHECK(strcmp(vms[0].vm_auth_token, token) == 0,
          "token altered: %s", vms[0].vm_auth_token);
    CHECK(strlen(vms[0].vm_auth_token) == strlen(token), "token length changed");

    vm_list_free(vms, count);
}

static void test_an_entry_without_a_token_is_dropped_not_defaulted(void) {
    // The neighbouring entry has a perfectly good token. Borrowing it — or
    // emitting an empty one for the caller to fill in — is what the "never
    // use another VM's token" rule forbids.
    const char *body =
        "{\"vm_list\":["
        "{\"vm_ws_url\":\"wss://no-token.vm.example/\",\"vm_id\":\"vm-x\"},"
        "{\"vm_ws_url\":\"wss://has-token.vm.example/\",\"vm_auth_token\":\"s0:tok-Y\",\"vm_id\":\"vm-y\"}"
        "]}";
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(200, body);

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
    CHECK(count == 1, "tokenless entry should be dropped, count=%d", count);
    CHECK(strcmp(vms[0].vm_id, "vm-y") == 0, "wrong survivor: %s", vms[0].vm_id);
    CHECK(strcmp(vms[0].vm_auth_token, "s0:tok-Y") == 0,
          "survivor token mismatch: %s", vms[0].vm_auth_token);

    vm_list_free(vms, count);
}

static void test_an_empty_token_never_reaches_a_connection(void) {
    // fetch_vms does accept an empty-string token — it is a string, so the
    // parser keeps the entry. vm_connect_params() is what refuses it, and the
    // two together are what matter: an empty bearer must never be sent.
    const char *body =
        "{\"vm_list\":[{\"vm_ws_url\":\"wss://a.vm.example/\","
        "\"vm_auth_token\":\"\",\"vm_id\":\"vm-a\"}]}";
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(200, body);

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
    CHECK(count == 1, "count=%d", count);
    CHECK(vms[0].vm_auth_token != NULL && vms[0].vm_auth_token[0] == '\0',
          "expected an empty token to survive parsing");

    char vm_id[128] = {0};
    const char *token = NULL;
    CHECK(!vm_connect_params(&vms[0], vm_id, sizeof(vm_id), &token),
          "an empty token must not produce a connection");

    vm_list_free(vms, count);
}

static void test_selection_returns_a_whole_entry(void) {
    // vm_find_* hand back one entry, so the caller cannot pick a url from one
    // and a token from another.
    const char *body =
        "{\"vm_list\":["
        "{\"vm_ws_url\":\"wss://a.vm.example/\",\"vm_auth_token\":\"s0:tok-A\",\"default\":false},"
        "{\"vm_ws_url\":\"wss://b.vm.example/\",\"vm_auth_token\":\"s0:tok-B\",\"default\":true}"
        "]}";
    vm_info_t vms[VM_API_MAX_VMS] = {0};
    fake_queue_response(200, body);

    int count = vm_api_fetch_vms("access", vms, VM_API_MAX_VMS);
    CHECK(count == 2, "count=%d", count);

    const vm_info_t *by_url = vm_find_by_url(vms, count, "wss://a.vm.example/");
    CHECK(by_url != NULL, "find by url failed");
    CHECK(strcmp(by_url->vm_auth_token, "s0:tok-A") == 0,
          "by_url returned another VM's token: %s", by_url->vm_auth_token);

    const vm_info_t *def = vm_find_default(vms, count);
    CHECK(def != NULL, "find default failed");
    CHECK(strcmp(def->vm_url, "wss://b.vm.example/") == 0,
          "default url: %s", def->vm_url);
    CHECK(strcmp(def->vm_auth_token, "s0:tok-B") == 0,
          "default returned another VM's token: %s", def->vm_auth_token);

    CHECK(by_url != def, "distinct urls should select distinct entries");

    vm_list_free(vms, count);
}

static void test_find_helpers_handle_empty_inputs(void) {
    CHECK(vm_find_default(NULL, 0) == NULL, "empty default should be NULL");
    CHECK(vm_find_by_url(NULL, 0, "wss://one") == NULL, "empty find should be NULL");
    CHECK(vm_find_by_url(NULL, 0, NULL) == NULL, "NULL url find should be NULL");
}

#define RUN_TEST(fn) \
    do { \
        fake_reset(); \
        fn(); \
        printf("ok %s\n", #fn); \
    } while (0)

int main(void) {
    RUN_TEST(test_mint_success_uses_headers_and_escaped_body);
    RUN_TEST(test_mint_bad_inputs_do_not_send_http);
    RUN_TEST(test_oversized_authorization_does_not_send_http);
    RUN_TEST(test_mint_rejects_malformed_or_partial_token_responses);
    RUN_TEST(test_mint_auth_failure_is_not_retried);
    RUN_TEST(test_refresh_success_via_access_token);
    RUN_TEST(test_refresh_and_mint_carry_sdk_token);
    RUN_TEST(test_refresh_falls_back_to_refresh_token_on_401);
    RUN_TEST(test_refresh_does_not_fallback_on_403);
    RUN_TEST(test_refresh_no_fallback_without_refresh_token);
    RUN_TEST(test_refresh_fallback_auth_failure_leaves_output_empty);
    RUN_TEST(test_refresh_401_and_403_on_the_refresh_leg);
    RUN_TEST(test_refresh_fallback_400_is_not_retried);
    RUN_TEST(test_refresh_bad_inputs_do_not_send_http);
    RUN_TEST(test_refresh_skips_access_leg_when_asked);
    RUN_TEST(test_refresh_with_neither_credential_sends_no_http);
    RUN_TEST(test_refresh_without_access_uses_refresh_token_directly);
    RUN_TEST(test_fetch_vms_success_filters_entries_and_uses_helpers);
    RUN_TEST(test_fetch_vms_respects_max);
    RUN_TEST(test_fetch_vms_auth_failure_is_not_retried);
    RUN_TEST(test_fetch_vms_retries_transient_failures_and_resets_response);
    RUN_TEST(test_fetch_vms_retries_temporary_http_errors);
    RUN_TEST(test_fetch_vms_rejects_bad_json_shapes);
    RUN_TEST(test_set_base_url_changes_fetch_target);
    RUN_TEST(test_each_entry_keeps_its_own_token);
    RUN_TEST(test_the_token_is_stored_byte_for_byte);
    RUN_TEST(test_an_entry_without_a_token_is_dropped_not_defaulted);
    RUN_TEST(test_an_empty_token_never_reaches_a_connection);
    RUN_TEST(test_selection_returns_a_whole_entry);
    RUN_TEST(test_find_helpers_handle_empty_inputs);
    return 0;
}

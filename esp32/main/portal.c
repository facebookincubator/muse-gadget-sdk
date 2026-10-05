/*
 * 配网门户：设备自己开一个热点，手机连上来用浏览器选网络、输密码。
 *
 * 为什么不用 BLE 那条：它要求手机完成配对握手、再把命令加密回来，而且设备一旦配好
 * 就拒绝重新配对（`error_pairing_unavailable`），换 Wi-Fi 只能靠 App 或整机重置。
 * 一个 HTTP 表单在任何手机浏览器里都能用，密码用手机键盘打，失败原因还能直接回显。
 *
 * 门户只在这几种情况开：开机没有任何可用凭证、设备长时间连不上、设置页手动打开。
 * 10 分钟没动作或连上网络后自动关（开放的临时热点不该一直挂着）。
 */
#include "portal.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "libs/qrcode/qrcodegen.h"
#include "muse_ble.h"
#include "esp_netif.h"
#include "i18n.h"
#include "muse_state.h"
#include "muse_link.h"
#include "muse_wifi.h"
#include "muse_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wifi_mgr.h"

static const char *TAG = "link.portal";

#define PORTAL_TIMEOUT_US     (30LL * 60 * 1000000)   /* 热点模式：30 分钟（配网要来回试，10 分钟太短） */
#define PORTAL_TIMEOUT_LAN_US (30LL * 60 * 1000000)   /* 局域网模式：30 分钟 */
#define PORTAL_SCAN_MAX     24
#define PORTAL_JOIN_MS      20000
#define PORTAL_BODY_MAX     320

extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[] asm("_binary_portal_html_end");

static httpd_handle_t s_httpd;
static esp_timer_handle_t s_deadline;
static esp_timer_handle_t s_keep_awake;   /* 门户开着时把屏幕钉住，别让二维码黑掉 */
static bool s_open;
static char s_portal_url[48];
static bool portal_force_ap;   /* 当前门户的地址：局域网 IP 或 AP IP */

/* 配对码只在手机正在配对时非零（跟屏幕上显示的是同一个值）。 */
/* 设备当前在局域网里的地址；拿不到就说明它自己也没网。 */
static bool portal_sta_ip(char *out, size_t out_size)
{
    esp_netif_t *netif = wifi_mgr_get_netif();
    esp_netif_ip_info_t ip = {0};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr) {
        return false;
    }
    snprintf(out, out_size, IPSTR, IP2STR(&ip.ip));
    return true;
}

static const char *portal_pairing_code(void)
{
    static char code[16];
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (b.passkey) {
        snprintf(code, sizeof(code), "%06lu", (unsigned long)b.passkey);
    } else {
        strlcpy(code, "—", sizeof(code));
    }
    return code;
}

static void json_send(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body ? body : "{}");
    free(body);
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)portal_html_start,
                           portal_html_end - portal_html_start);
}

/* 手机连上没有外网的热点时会去探测这几个地址；把它们 302 到配网页，
 * iOS/Android 就会自己弹出"登录网络"页面 —— 不用用户手打网址。 */
static esp_err_t captive_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url[0] ? s_portal_url : "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char reason[96];
    wifi_mgr_failure_text(reason, sizeof(reason));
    muse_wifi_status_t w = {0};
    muse_wifi_status(&w);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", wifi_mgr_is_connected());
    cJSON_AddStringToObject(root, "reason", reason);
    /* 页面轮询用：设备正在连哪个网络、以及失败时的原因（人话） */
    cJSON_AddStringToObject(root, "state_ssid", w.ssid);
    cJSON_AddStringToObject(root, "detail", w.detail);
    cJSON_AddNumberToObject(root, "rssi", w.rssi);
    cJSON_AddStringToObject(root, "host", s_portal_url[0] ? s_portal_url : wifi_mgr_portal_ip());
    json_send(req, root);
    return ESP_OK;
}

/* 扫网结果直接给手机看 —— 在圆屏上翻网络列表才是真的折磨。
 * 注意：wifi_mgr_scan() 是阻塞的，而 httpd 只有一个任务，直接把整个服务卡住。
 * 所以这里只触发后台扫描、立刻返回已有结果，页面看到 scanning 再轮询。 */
static esp_err_t scan_get(httpd_req_t *req)
{
    static muse_wifi_ap_t aps[PORTAL_SCAN_MAX];
    uint32_t gen = 0;

    bool scanning = muse_wifi_scanning();
    if (!scanning) {
        (void)muse_wifi_scan();          /* 起一次后台扫描 */
        scanning = muse_wifi_scanning();
    }
    int n = muse_wifi_scan_results(aps, PORTAL_SCAN_MAX, &gen);

    cJSON *root = cJSON_CreateObject();
    cJSON *nets = cJSON_AddArrayToObject(root, "nets");
    if (nets) {
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(e, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(e, "secure", aps[i].secure);
            cJSON_AddItemToArray(nets, e);
        }
    }
    cJSON_AddBoolToObject(root, "scanning", scanning);
    json_send(req, root);
    return ESP_OK;
}

static esp_err_t join_post(httpd_req_t *req)
{
    char body[PORTAL_BODY_MAX];
    int want = req->content_len < (int)sizeof(body) - 1 ? (int)req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, body + got, want - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        got += r;
    }
    body[got] = '\0';

    cJSON *in = cJSON_Parse(body);
    const char *ssid = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "ssid")) : NULL;
    const char *pass = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "password")) : NULL;
    if (!ssid || !ssid[0]) {
        cJSON_Delete(in);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_OK;
    }

    /* 这里**不能**调阻塞的 wifi_mgr_connect()：它会先抢走无线电（把现在连着的网络
     * 断开）、再重试到超时，期间 httpd 被这一个请求占死，手机那边只看到 "Load failed"。
     * 走设置页同一条路：写入凭证 + 触发，后台去连；页面轮询 /api/status 拿结果和原因。 */
    bool accepted = muse_link_wifi_set(ssid, pass ? pass : "");
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "ssid", ssid);
    if (accepted) {
        muse_wifi_apply();
        cJSON_AddBoolToObject(out, "started", true);
        ESP_LOGI(TAG, "portal: 交给后台去连 \"%s\"", ssid);
    } else {
        cJSON_AddBoolToObject(out, "started", false);
        cJSON_AddStringToObject(out, "reason", "设备这边没能接受这次请求");
    }
    cJSON_Delete(in);
    json_send(req, out);
    return ESP_OK;
}

static void keep_awake_cb(void *arg)
{
    (void)arg;
    muse_state_poke();
}

static void deadline_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "portal: window expired, closing");
    portal_stop();
}

esp_err_t portal_start_ap(void);

esp_err_t portal_start_ap(void)
{
    portal_force_ap = true;
    esp_err_t err = portal_start();
    portal_force_ap = false;
    return err;
}

esp_err_t portal_start(void)
{
    if (s_open) {
        return ESP_OK;
    }
    /* 两条路自动选：
     *  - 设备已经在 Wi-Fi 上：手机跟它同一个网就行，二维码放**网址**（扫码即开页面），
     *    不用起热点、不用加入任何网络（iOS 的 Wi-Fi 二维码加入很不稳，实测踩过）。
     *  - 设备自己也没网：起热点，二维码放 WIFI 协议码 + captive 页面。 */
    char sta_ip[16] = {0};
    /* portal_start_ap(): 强制开热点 —— 设备困在访客网络/隔离网时，局域网模式根本够不到 */
    bool on_lan = !portal_force_ap && portal_sta_ip(sta_ip, sizeof(sta_ip));
    if (!on_lan) {
        ESP_RETURN_ON_ERROR(wifi_mgr_portal_start(), TAG, "portal AP");
    }

    if (!s_httpd) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.uri_match_fn = httpd_uri_match_wildcard;
        cfg.lru_purge_enable = true;
        cfg.max_uri_handlers = 16;
        cfg.stack_size = 6144;
        ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");
        const httpd_uri_t uris[] = {
            { .uri = "/",           .method = HTTP_GET,  .handler = root_get },
            { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
            { .uri = "/api/scan",   .method = HTTP_GET,  .handler = scan_get },
            { .uri = "/api/join",   .method = HTTP_POST, .handler = join_post },
            { .uri = "/generate_204",        .method = HTTP_GET, .handler = captive_get },
            { .uri = "/gen_204",             .method = HTTP_GET, .handler = captive_get },
            { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_get },
            { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_get },
            { .uri = "/connecttest.txt",     .method = HTTP_GET, .handler = captive_get },
            { .uri = "/ncsi.txt",            .method = HTTP_GET, .handler = captive_get },
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
            ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &uris[i]), TAG, "uri %s", uris[i].uri);
        }
    }
    if (!s_deadline) {
        const esp_timer_create_args_t args = { .callback = deadline_cb, .name = "portal" };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_deadline), TAG, "timer");
    }
    esp_timer_stop(s_deadline);
    ESP_RETURN_ON_ERROR(esp_timer_start_once(s_deadline, on_lan ? PORTAL_TIMEOUT_LAN_US : PORTAL_TIMEOUT_US),
                        TAG, "timer start");

    s_open = true;
    /* 屏上要显示二维码给人扫：先唤醒，门户开着期间周期性 poke 住 */
    muse_state_set_asleep(false);
    muse_state_poke();
    if (!s_keep_awake) {
        const esp_timer_create_args_t ka = { .callback = keep_awake_cb, .name = "portal_awake" };
        (void)esp_timer_create(&ka, &s_keep_awake);
    }
    if (s_keep_awake) {
        esp_timer_stop(s_keep_awake);
        (void)esp_timer_start_periodic(s_keep_awake, 20LL * 1000000);
    }
    char url[48], line[96], payload[160];
    if (on_lan) {
        snprintf(url, sizeof(url), "http://%s/", sta_ip);
        snprintf(line, sizeof(line), "%s", tr("Same Wi-Fi as the board: scan to open"));
        snprintf(payload, sizeof(payload), "%s", url);
    } else {
        snprintf(url, sizeof(url), "http://%s/", wifi_mgr_portal_ip());
        snprintf(line, sizeof(line), "%s %s  %s %s", tr("Hotspot"), wifi_mgr_portal_ssid(),
                 tr("Password"), wifi_mgr_portal_password());
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
                 wifi_mgr_portal_ssid(), wifi_mgr_portal_password());
    }
    strlcpy(s_portal_url, url, sizeof(s_portal_url));
    muse_ui_portal_hint(true, line, url, portal_pairing_code(), payload);
    ESP_LOGI(TAG, "配网门户已开（%s）: %s", on_lan ? "局域网" : "热点", url);
    return ESP_OK;
}

/* 串口打印二维码：烧录完直接扫，不用先找热点名。 */
void portal_print_qr(void)
{
    uint8_t *tmp = malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *qr = malloc(qrcodegen_BUFFER_LEN_MAX);
    char url[48], payload[128];
    snprintf(url, sizeof(url), "http://%s/", wifi_mgr_portal_ip());
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
             wifi_mgr_portal_ssid(), wifi_mgr_portal_password());
    if (!portal_active()) {
        printf("@portal off（先 >portal=on）\n");
        free(tmp);
        free(qr);
        return;
    }
    if (tmp && qr &&
        qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_LOW,
                             qrcodegen_VERSION_MIN, 8, qrcodegen_Mask_AUTO, true)) {
        const int n = qrcodegen_getSize(qr);
        printf("@portal.qr %s  热点 %s  密码 %s  配对码 %s\n\n", url,
               wifi_mgr_portal_ssid(), wifi_mgr_portal_password(), portal_pairing_code());
        for (int y = -2; y < n + 2; y++) {
            for (int x = -2; x < n + 2; x++) {
                bool on = x >= 0 && y >= 0 && x < n && y < n && qrcodegen_getModule(qr, x, y);
                printf(on ? "██" : "  ");
            }
            printf("\n");
        }
    } else {
        printf("@portal.qr 生成失败\n");
    }
    free(tmp);
    free(qr);
}

void portal_stop(void)
{
    muse_ui_portal_hint(false, NULL, NULL, NULL, NULL);
    if (s_keep_awake) {
        esp_timer_stop(s_keep_awake);
    }
    if (s_deadline) {
        esp_timer_stop(s_deadline);
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    wifi_mgr_portal_stop();
    if (s_open) {
        ESP_LOGI(TAG, "配网门户已关");
    }
    s_open = false;
}

bool portal_active(void) { return s_open; }

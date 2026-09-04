/*
 * study_wifi.c — WiFi 连接 + 本地网页配网 (SoftAP + HTTP)
 *
 * 配网流程（适合无键盘设备，手机辅助）：
 *   1) 无已存凭证 或 用户到设置页手动触发 → 开启 SoftAP 热点（WPA2 密码在屏幕显示）
 *   2) 手机连接该热点，浏览器打开 http://192.168.4.1 填入账号密码并提交
 *   3) 设备把凭证写入 NVS("wifi") → 关闭 AP → 连接该 WiFi → 状态回到 CONNECTED
 *   4) 之后每次开机自动读取凭证联网；失败自动回落到 AP 配网等待
 */
#include "study_wifi.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "study_recorder.h"     /* 录音列表 / 删除 */
#include "study_audio_codec.h"  /* .FRC → PCM 解码导出 WAV */

static const char *TAG = "study_wifi";

/* NVS 命名空间与键 */
#define WIFI_NVS_NS      "wifi"
#define WIFI_NVS_SSID    "ssid"
#define WIFI_NVS_PASS    "pass"

#define WIFI_MAX_RETRY   5

static bool s_ready = false;               /* esp_wifi 已初始化 */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static httpd_handle_t s_server = NULL;
static char s_ap_ssid[40];
static char s_connected_ssid[33];          /* STA 当前/最近连接到的 SSID */
static int  s_rssi = 0;
static volatile study_wifi_state_t s_state = WIFI_STATE_IDLE;
static volatile int s_retry_count = 0;

static void start_apply_creds(void);
static void connect_sta(const char *ssid, const char *pass);
static void start_ap_config(void);
static esp_timer_handle_t s_retry_timer = NULL;

static study_wifi_cb_t s_cb = NULL;
static void *s_cb_user = NULL;

/* ---------- 内部状态切换 ---------- */
static void set_state(study_wifi_state_t st) {
    if (s_state == st) return;
    s_state = st;
    if (s_cb) s_cb(st, s_cb_user);
}

/* ---------- NVS 凭证 ---------- */
bool study_wifi_has_stored_creds(void) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0;
    bool has = (nvs_get_str(h, WIFI_NVS_SSID, NULL, &sz) == ESP_OK && sz > 1);
    nvs_close(h);
    return has;
}

static bool creds_get(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t s = ssid_sz;
    if (nvs_get_str(h, WIFI_NVS_SSID, ssid, &s) != ESP_OK) { nvs_close(h); return false; }
    s = pass_sz;
    if (nvs_get_str(h, WIFI_NVS_PASS, pass, &s) != ESP_OK) {
        pass[0] = '\0';        /* 开放网络无密码也允许 */
    }
    nvs_close(h);
    return true;
}

static void creds_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open wifi failed");
        return;
    }
    nvs_set_str(h, WIFI_NVS_SSID, ssid);
    nvs_set_str(h, WIFI_NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- HTTP 页面 ---------- */
static const char AP_PAGE[] =
    "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>考研日程 wifi</title></head>"
    "<body style=\"font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px\">"
    "<h2>WiFi 配网</h2>"
    "<p>请输入家里路由器（2.4GHz）的账号和密码：</p>"
    "<form action=\"/save\" method=\"post\">"
    "<label>WiFi 名称</label><br>"
    "<input name=\"ssid\" id=\"ssid\" autocomplete=\"off\" style=\"width:100%;padding:10px;font-size:16px;box-sizing:border-box\" required><br><br>"
    "<label>WiFi 密码</label><br>"
    "<input name=\"pass\" type=\"password\" style=\"width:100%;padding:10px;font-size:16px;box-sizing:border-box\"><br><br>"
    "<button style=\"width:100%;padding:12px;font-size:16px\">连接</button>"
    "</form>"
    "<script>try{document.getElementById('ssid').focus();}catch(e){}</script>"
    "</body></html>";

static void url_decode(char *dst, size_t dst_sz, const char *src) {
    size_t d = 0;
    while (*src && d + 1 < dst_sz) {
        if (*src == '%' && src[1] && src[2]) {
            unsigned int v;
            if (sscanf(src + 1, "%2x", &v) == 1) {
                dst[d++] = (char)v;
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
            continue;
        }
        dst[d++] = *src++;
    }
    dst[d] = '\0';
}

/* 解析 x-www-form-urlencoded：取 ssid / pass 字段 */
static void parse_form(const char *body, char *ssid, size_t ssid_sz,
                       char *pass, size_t pass_sz) {
    ssid[0] = '\0'; pass[0] = '\0';
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        char pair[128];
        if (len >= sizeof(pair)) len = sizeof(pair) - 1;
        memcpy(pair, p, len);
        pair[len] = '\0';
        const char *eq = strchr(pair, '=');
        if (eq) {
            if (strncmp(pair, "ssid=", 5) == 0)
                url_decode(ssid, ssid_sz, eq + 1);
            else if (strncmp(pair, "pass=", 5) == 0)
                url_decode(pass, pass_sz, eq + 1);
        }
        if (!amp) break;
        p = amp + 1;
    }
}

static void stop_http_server(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

static esp_err_t handle_ap_root(httpd_req_t *req);
static esp_err_t handle_ap_save(httpd_req_t *req);
static esp_err_t handle_rec_list(httpd_req_t *req);
static esp_err_t handle_rec_dl(httpd_req_t *req);
static esp_err_t handle_rec_del(httpd_req_t *req);

/* AP+STA 同时开：配网热点始终可搜到(STU_STUDY_xxxx + 密码)；有凭证则同时去连家里 WiFi */
static void wifi_start_apsta(bool have, const char *ssid, const char *pass) {
    if (!s_ready) return;
    s_retry_count = 0;

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ac = { 0 };
    strncpy((char *)ac.ap.ssid, s_ap_ssid, sizeof(ac.ap.ssid) - 1);
    ac.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ac.ap.channel = 1;
    ac.ap.max_connection = 4;
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)ac.ap.password, STUDY_WIFI_AP_PASS, sizeof(ac.ap.password) - 1);
    esp_wifi_set_config(WIFI_IF_AP, &ac);

    if (have) {
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        set_state(WIFI_STATE_CONNECTING);
    } else {
        set_state(WIFI_STATE_AP_CONFIG);
    }
    esp_wifi_start();    /* WIFI_EVENT_STA_START → esp_wifi_connect() 自动去连 */

    /* 配网页始终开，方便随时改网络；并附录音机列表/下载路由。
     * 栈加码到 8192：/rec/dl 里要做 Opus 解码，需要更大任务栈。 */
    if (!s_server) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = STUDY_WIFI_AP_PORT;
        cfg.uri_match_fn = httpd_uri_match_wildcard;
        cfg.stack_size = 8192;
        if (httpd_start(&s_server, &cfg) == ESP_OK) {
            static const httpd_uri_t r_root = { .uri = "/", .method = HTTP_GET, .handler = handle_ap_root };
            static const httpd_uri_t r_save = { .uri = "/save", .method = HTTP_POST, .handler = handle_ap_save };
            static const httpd_uri_t r_rl   = { .uri = "/rec/list", .method = HTTP_GET, .handler = handle_rec_list };
            static const httpd_uri_t r_rd   = { .uri = "/rec/dl",   .method = HTTP_GET, .handler = handle_rec_dl };
            static const httpd_uri_t r_rm   = { .uri = "/rec/del",  .method = HTTP_GET, .handler = handle_rec_del };
            httpd_register_uri_handler(s_server, &r_root);
            httpd_register_uri_handler(s_server, &r_save);
            httpd_register_uri_handler(s_server, &r_rl);
            httpd_register_uri_handler(s_server, &r_rd);
            httpd_register_uri_handler(s_server, &r_rm);
            ESP_LOGI(TAG, "配网页已就绪: http://%s/", STUDY_WIFI_AP_GATEWAY);
        } else {
            ESP_LOGE(TAG, "httpd 启动失败");
        }
    }
}

static void connect_sta(const char *ssid, const char *pass) {
    wifi_start_apsta(true, ssid, pass);   /* 连家里 WiFi，同时热点保持 */
}

static esp_err_t handle_ap_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, AP_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_ap_save(httpd_req_t *req) {
    char body[512] = { 0 };
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret > 0) body[ret] = '\0';

    char ssid[33] = { 0 }, pass[65] = { 0 };
    parse_form(body, ssid, sizeof(ssid), pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<h3>请填写 WiFi 名称</h3><a href=\"/\">返回</a>");
        return ESP_OK;
    }

    creds_save(ssid, pass);
    ESP_LOGI(TAG, "收到配网凭证 ssid=%s, 开始连接", ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<h3>收到！设备正在连接 WiFi，请返回设备查看结果。</h3>"
        "<p>配网热点保持开启，随时可再次配置。</p>");

    /* 不能在 httpd 处理函数里 httpd_stop（会死锁），交给独立任务延迟后执行 */
    start_apply_creds();
    return ESP_OK;
}

/* ---------- 录音列表 / 下载（并入配网页，录音导出无需独立开热点流程） ---------- */
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static esp_err_t handle_rec_list(httpd_req_t *req) {
    study_rec_entry_t items[REC_MAX_FILES];
    int n = study_recorder_scan(items, REC_MAX_FILES);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html lang=zh><head><meta charset=utf-8>"
        "<title>录音笔</title></head><body style=\"font-family:sans-serif;max-width:520px;margin:20px auto;padding:0 16px\">"
        "<h2>录音笔 · 本地文件</h2>");
    if (n <= 0) {
        httpd_resp_sendstr_chunk(req, "<p>还没有录音。</p><p><a href=\"/\">← WiFi 配网</a></p></body></html>");
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_sendstr_chunk(req, "<table style=\"width:100%;border-collapse:collapse\">");
    for (int i = 0; i < n; i++) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "<tr style=\"border-bottom:1px solid #ddd\"><td>R%07lu</td>"
                 "<td>%02lu:%02lu</td>"
                 "<td><a href=\"/rec/dl?seq=%lu\">下载 WAV</a></td>"
                 "<td><a href=\"/rec/del?seq=%lu\" style=\"color:#c00\">删除</a></td></tr>",
                 (unsigned long)items[i].seq,
                 (unsigned long)(items[i].ms_len / 60000),
                 (unsigned long)((items[i].ms_len / 1000) % 60),
                 (unsigned long)items[i].seq, (unsigned long)items[i].seq);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req,
        "</table><p><a href=\"/\">← WiFi 配网</a></p></body></html>");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_rec_dl(httpd_req_t *req) {
    /* 解析 ?seq=N */
    char seqs[24] = { 0 };
    if (httpd_req_get_url_query_str(req, seqs, sizeof(seqs)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing seq");
        return ESP_FAIL;
    }
    char vseq[24] = { 0 };
    if (httpd_query_key_value(seqs, "seq", vseq, sizeof(vseq)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad seq");
        return ESP_FAIL;
    }
    long seq = strtol(vseq, NULL, 10);
    if (seq <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad seq");
        return ESP_FAIL;
    }
    char path[64];
    study_recorder_path((uint32_t)seq, path, sizeof(path));
    study_frc_reader_t *rd = study_frc_open(path);
    if (!rd) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "recording not found");
        return ESP_FAIL;
    }
    uint32_t rate = study_frc_rate(rd);
    uint32_t frames = study_frc_num_frames(rd);
    uint32_t data_bytes = frames * STUDY_FRAME_SAMPLES * 2;

    uint8_t wh[44];
    memcpy(wh, "RIFF", 4); put_u32le(wh + 4, 36 + data_bytes);
    memcpy(wh + 8, "WAVE", 4);
    memcpy(wh + 12, "fmt ", 4); put_u32le(wh + 16, 16);
    wh[20] = 1; wh[21] = 0;                       /* audioFormat=PCM */
    wh[22] = 1; wh[23] = 0;                       /* channels=1 */
    put_u32le(wh + 24, rate);                     /* sampleRate */
    put_u32le(wh + 28, rate * 2);                 /* byteRate = rate*ch*bits/8 */
    wh[32] = 2; wh[33] = 0;                       /* blockAlign */
    wh[34] = 16; wh[35] = 0;                      /* bitsPerSample */
    memcpy(wh + 36, "data", 4); put_u32le(wh + 40, data_bytes);

    httpd_resp_set_type(req, "audio/wav");
    char cd[80];
    snprintf(cd, sizeof(cd), "attachment; filename=R%07lu.wav", (unsigned long)seq);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    /* 分块发送：先 WAV 头，再解码 PCM */
    httpd_resp_send_chunk(req, (const char *)wh, sizeof(wh));
    int16_t pcm[1024];
    for (;;) {
        int ns = study_frc_read_pcm(rd, pcm, 1024);
        if (ns <= 0) break;
        httpd_resp_send_chunk(req, (const char *)pcm, (size_t)ns * 2);
    }
    study_frc_close(rd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_rec_del(httpd_req_t *req) {
    char seqs[24] = { 0 };
    if (httpd_req_get_url_query_str(req, seqs, sizeof(seqs)) == ESP_OK) {
        char vseq[24] = { 0 };
        if (httpd_query_key_value(seqs, "seq", vseq, sizeof(vseq)) == ESP_OK) {
            long seq = strtol(vseq, NULL, 10);
            if (seq > 0) study_recorder_delete_seq((uint32_t)seq);
        }
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<h3>已处理。</h3><p><a href=\"/rec/list\">← 返回录音列表</a> · <a href=\"/\">WiFi 配网</a></p></body></html>");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void apply_creds_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(150));   /* 让 httpd 处理函数先完整返回 */
    char ssid[33], pass[65];
    if (creds_get(ssid, sizeof(ssid), pass, sizeof(pass))) {
        connect_sta(ssid, pass);
    }
    vTaskDelete(NULL);
}

static void start_apply_creds(void) {
    xTaskCreate(apply_creds_task, "wifi_apply", 4096, NULL, 3, NULL);
}

/* 开启配网：确保热点一定可搜到（断开正在连的网络，不反复尝试旧网避免抖动） */
static void start_ap_config(void) {
    if (s_state == WIFI_STATE_AP_CONFIG && s_server) return;
    wifi_start_apsta(false, NULL, NULL);
}

/* ---------- WiFi 事件 ---------- */
static void wifi_retry_cb(void *arg) {
    (void)arg;
    if (s_state == WIFI_STATE_CONNECTING || s_state == WIFI_STATE_CONNECTED) {
        esp_wifi_connect();               /* 定时重连，不阻塞事件循环 */
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_state != WIFI_STATE_CONNECTING && s_state != WIFI_STATE_CONNECTED) return;
        ESP_LOGW(TAG, "STA 断开 reason=%d", d ? d->reason : -1);
        if (++s_retry_count > WIFI_MAX_RETRY) {
            set_state(WIFI_STATE_FAILED);
            /* 连不上就自动开回配网热点，避免“既连不上网、又看不到热点” */
            start_ap_config();
            return;
        }
        /* 3 秒后重连（用 esp_timer，避免阻塞事件循环任务） */
        if (s_retry_timer) esp_timer_start_once(s_retry_timer, 3 * 1000 * 1000);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        /* 手机连上热点，提示打开页面 */
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "已联网 IP: " IPSTR, IP2STR(&e->ip_info.ip));
        wifi_ap_record_t r;
        if (esp_wifi_sta_get_ap_info(&r) == ESP_OK) {
            s_rssi = r.rssi;
            strncpy(s_connected_ssid, (const char *)r.ssid, sizeof(s_connected_ssid) - 1);
        }
        s_retry_count = 0;
        set_state(WIFI_STATE_CONNECTED);
    }
}

/* ---------- 对外接口 ---------- */
void study_wifi_set_callback(study_wifi_cb_t cb, void *user) {
    s_cb = cb;
    s_cb_user = user;
}

study_wifi_state_t study_wifi_get_state(void) { return s_state; }
const char *study_wifi_get_ssid(void)          { return s_connected_ssid; }
int study_wifi_get_rssi(void)                  { return s_rssi; }
const char *study_wifi_get_ap_ssid(void)       { return s_ap_ssid; }

void study_wifi_start_ap_config(void) {
    if (!s_ready) return;
    start_ap_config();
}

void study_wifi_stop(void) {
    if (!s_ready) return;
    stop_http_server();
    esp_wifi_stop();
    set_state(WIFI_STATE_IDLE);
}

/* 录音时临时让出 WiFi 内存（可释放约几十 KB 堆），录完恢复。仅 stop/start，配置保留。 */
void study_wifi_pause(void) {
    if (!s_ready) return;
    esp_wifi_stop();
    set_state(WIFI_STATE_IDLE);
}
void study_wifi_resume(void) {
    if (!s_ready) return;
    esp_wifi_start();   /* APSTA 配置仍在：热点恢复，STA 由事件自动重连 */
    s_retry_count = 0;
}

void study_wifi_init(const char *ap_ssid_prefix) {
    /* 幂等 */
    if (s_ready) return;

    /* AP 名称：前缀 + 后 4 位 MAC */
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.20s_%02X%02X",
                 ap_ssid_prefix ? ap_ssid_prefix : "STU", mac[4], mac[5]);
    } else {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.20s_config",
                 ap_ssid_prefix ? ap_ssid_prefix : "STU");
    }

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* 重连定时器（一次性，启动后自动停） */
    esp_timer_create_args_t ta = {
        .callback = wifi_retry_cb,
        .name = "wifi_retry",
    };
    esp_timer_create(&ta, &s_retry_timer);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);

    s_ready = true;

    /* 热点常开(APSTA)：无凭证→只开配网热点；有凭证→热点照开并自动连家里 WiFi */
    char ssid[33], pass[65];
    bool have = creds_get(ssid, sizeof(ssid), pass, sizeof(pass));
    wifi_start_apsta(have, have ? ssid : NULL, have ? pass : NULL);
}

#else  /* !ESP_PLATFORM — 宿主编译存根 */

bool study_wifi_has_stored_creds(void) { return false; }
void study_wifi_init(const char *ap_ssid_prefix) { (void)ap_ssid_prefix; }
void study_wifi_start_ap_config(void) {}
void study_wifi_stop(void) {}
void study_wifi_pause(void) {}
void study_wifi_resume(void) {}
study_wifi_state_t study_wifi_get_state(void) { return WIFI_STATE_IDLE; }
const char *study_wifi_get_ssid(void) { return ""; }
int study_wifi_get_rssi(void) { return 0; }
const char *study_wifi_get_ap_ssid(void) { return ""; }
void study_wifi_set_callback(study_wifi_cb_t cb, void *user) { (void)cb; (void)user; }

#endif
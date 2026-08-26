#include "upload_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "sd_card.h"
#include "sd_file.h"

#ifndef CONFIG_UPLOAD_SERVER_PORT
#define CONFIG_UPLOAD_SERVER_PORT 80
#endif
#ifndef CONFIG_UPLOAD_DIR
#define CONFIG_UPLOAD_DIR "/sdcard/esp32_files"
#endif
#ifndef CONFIG_UPLOAD_MAX_SIZE_MB
#define CONFIG_UPLOAD_MAX_SIZE_MB 64
#endif

#define UPLOAD_CHUNK_SIZE   4096
#define UPLOAD_MAX_SIZE     ((uint32_t)CONFIG_UPLOAD_MAX_SIZE_MB * 1024 * 1024)
#define UPLOAD_ACK_TIMEOUT_MS 5000 /* LVGL task must ack within this */
#define UPLOAD_LIST_CAP     4096   /* HTML file-list buffer */
#define UPLOAD_LIST_MAX_ENTRIES 64

static const char *TAG = "upload_server";

/*============================================================================
 * Mailbox between the HTTP handlers (httpd task) and the LVGL task.
 * A single shared struct + one-slot queue + ack semaphore serializes all
 * SD I/O: the httpd side fills the request fields, posts a command token,
 * then blocks on the ack semaphore until the LVGL task has executed it.
 *============================================================================*/

typedef enum {
    CMD_OPEN_WRITE, /* name -> pick free numbered name, open "wb" */
    CMD_WRITE,      /* data/len -> fwrite */
    CMD_FINISH,     /* fclose, keep file */
    CMD_ABORT,      /* fclose + remove partial file */
    CMD_OPEN_READ,  /* name -> open "rb", report file_size */
    CMD_READ,       /* fread up to chunk size into data, len = bytes (0=EOF) */
    CMD_CLOSE,      /* fclose */
    CMD_LIST,       /* fill list buffer with HTML <li> entries */
} upload_cmd_t;

typedef struct {
    upload_cmd_t cmd;
    char name[UPLOAD_NAME_MAX + 1];     /* request: file name */
    char final_name[UPLOAD_NAME_MAX + 1]; /* result: chosen/validated name */
    uint8_t data[UPLOAD_CHUNK_SIZE];    /* write/read payload */
    size_t len;                         /* request: bytes to write; result: bytes read */
    uint32_t file_size;                 /* result: size of opened file */
    char list[UPLOAD_LIST_CAP];         /* result: HTML list */
    esp_err_t result;
    FILE *fp;
} upload_io_t;

static upload_io_t s_io;
static QueueHandle_t s_q = NULL;
static SemaphoreHandle_t s_ack = NULL;

static httpd_handle_t s_server = NULL;
static bool s_running = false;
static volatile bool s_stop_req = false;
static upload_server_phase_t s_phase = UPLOAD_SERVER_IDLE;
static char s_cur_name[UPLOAD_NAME_MAX + 1] = "";
static uint32_t s_written = 0;
static uint32_t s_total = 0;

/*============================================================================
 * LVGL task side: execute one command (all SD I/O happens here)
 *============================================================================*/

/* Bounded append: copy src onto the end of dst (a C string), never
 * exceeding cap bytes total. No snprintf, so the truncation analyzer
 * cannot complain; the "_N" suffix and extension always survive, the
 * base is what gets shortened when the name is too long. */
static void append_str(char *dst, size_t cap, const char *src)
{
    size_t dl = strlen(dst);
    size_t room = (dl < cap) ? (cap - 1 - dl) : 0;
    size_t sl = strlen(src);
    size_t n = (sl < room) ? sl : room;
    memcpy(dst + dl, src, n);
    dst[dl + n] = '\0';
}

/* Make sure the flat upload directory exists (idempotent). Runs on the
 * LVGL task (SD I/O). */
static void ensure_upload_dir(void)
{
    struct stat st;
    if (stat(CONFIG_UPLOAD_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        return;
    }
    if (mkdir(CONFIG_UPLOAD_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) failed: %s (%d)", CONFIG_UPLOAD_DIR,
                 strerror(errno), errno);
    }
}

static void do_open_write(void)
{
    /* Uploads are flat: make sure the target directory exists. */
    ensure_upload_dir();

    /* Split "base.ext" at the last dot so numbering is file_1.ext */
    char base[UPLOAD_NAME_MAX + 1], ext[UPLOAD_NAME_MAX + 1];
    const char *dot = strrchr(s_io.name, '.');
    if (dot != NULL && dot != s_io.name) {
        size_t blen = (size_t)(dot - s_io.name);
        memcpy(base, s_io.name, blen);
        base[blen] = '\0';
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        snprintf(base, sizeof(base), "%s", s_io.name);
        ext[0] = '\0';
    }

    /* Pick the first free name: name, name_1, name_2, ... */
    for (int i = 0; i < 10000; i++) {
        char cand[UPLOAD_NAME_MAX + 64];
        if (i == 0) {
            snprintf(s_io.final_name, sizeof(s_io.final_name), "%s", s_io.name);
        } else {
            s_io.final_name[0] = '\0';
            append_str(s_io.final_name, sizeof(s_io.final_name), base);
            char num[8];
            snprintf(num, sizeof(num), "_%d", i);
            append_str(s_io.final_name, sizeof(s_io.final_name), num);
            append_str(s_io.final_name, sizeof(s_io.final_name), ext);
        }
        snprintf(cand, sizeof(cand), "%s/%s", CONFIG_UPLOAD_DIR, s_io.final_name);

        if (access(cand, F_OK) == 0) {
            continue; /* name taken, try the next number */
        }
        s_io.fp = fopen(cand, "wb");
        if (s_io.fp == NULL) {
            ESP_LOGW(TAG, "fopen(%s) failed: %s (%d)", cand,
                     strerror(errno), errno);
        }
        s_io.result = (s_io.fp != NULL) ? ESP_OK : ESP_FAIL;
        return;
    }
    s_io.result = ESP_ERR_NOT_FOUND; /* 10000 copies is enough */
}

static void do_write(void)
{
    if (s_io.fp == NULL) {
        s_io.result = ESP_ERR_INVALID_STATE;
        return;
    }
    if (fwrite(s_io.data, 1, s_io.len, s_io.fp) != s_io.len) {
        s_io.result = ESP_FAIL;
        return;
    }
    fflush(s_io.fp); /* write through to FAT now (progress + crash safety) */
    s_written += s_io.len;
    s_io.result = ESP_OK;
}

static void do_finish(void)
{
    if (s_io.fp != NULL) {
        s_io.result = (fclose(s_io.fp) == 0) ? ESP_OK : ESP_FAIL;
        s_io.fp = NULL;
    } else {
        s_io.result = ESP_ERR_INVALID_STATE;
    }
}

static void do_abort(void)
{
    if (s_io.fp != NULL) {
        fclose(s_io.fp);
        s_io.fp = NULL;
    }
    char path[UPLOAD_NAME_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_UPLOAD_DIR, s_io.final_name);
    unlink(path); /* remove the partial file */
    s_io.result = ESP_OK;
}

static void do_open_read(void)
{
    char path[UPLOAD_NAME_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_UPLOAD_DIR, s_io.name);
    s_io.fp = fopen(path, "rb");
    if (s_io.fp == NULL) {
        s_io.result = ESP_ERR_NOT_FOUND;
        return;
    }
    struct stat st;
    if (fstat(fileno(s_io.fp), &st) == 0) {
        s_io.file_size = (uint32_t)st.st_size;
    } else {
        s_io.file_size = 0;
    }
    snprintf(s_io.final_name, sizeof(s_io.final_name), "%s", s_io.name);
    s_io.result = ESP_OK;
}

static void do_read(void)
{
    if (s_io.fp == NULL) {
        s_io.len = 0;
        s_io.result = ESP_ERR_INVALID_STATE;
        return;
    }
    size_t n = fread(s_io.data, 1, sizeof(s_io.data), s_io.fp);
    s_io.len = n;
    s_io.result = ESP_OK;
}

static void do_close(void)
{
    if (s_io.fp != NULL) {
        fclose(s_io.fp);
        s_io.fp = NULL;
    }
    s_io.result = ESP_OK;
}

/* Minimal URL encoding for hrefs (names are already validated) */
static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                    c == '~';
        if (safe) {
            dst[o++] = (char)c;
        } else if (o + 3 < cap) {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

/* Minimal HTML escaping for names shown in the page */
static void html_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < cap; i++) {
        char c = src[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep != NULL) {
            size_t rl = strlen(rep);
            if (o + rl + 1 >= cap) break;
            memcpy(dst + o, rep, rl);
            o += rl;
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
}

static void do_list(void)
{
    /* Runs on the LVGL task; keep the entry array static, not on the
     * stack (64 * ~72 B would eat 4.6 KB of the LVGL task stack). */
    static sd_file_entry_t entries[UPLOAD_LIST_MAX_ENTRIES];
    size_t count = 0;
    size_t cap = sizeof(s_io.list);
    size_t o = 0;

    ensure_upload_dir(); /* so the first page load lists cleanly */

    if (sd_file_list(CONFIG_UPLOAD_DIR, entries, UPLOAD_LIST_MAX_ENTRIES, &count) != ESP_OK) {
        s_io.list[0] = '\0';
        s_io.result = ESP_OK; /* empty list, not an error */
        return;
    }

    for (size_t i = 0; i < count && o + 64 < cap; i++) {
        char enc[UPLOAD_NAME_MAX * 3 + 8];
        char esc[UPLOAD_NAME_MAX * 6 + 8];
        url_encode(entries[i].name, enc, sizeof(enc));
        html_escape(entries[i].name, esc, sizeof(esc));

        int n = snprintf(s_io.list + o, cap - o,
                         "<li><a href=\"/download?name=%s\">%s</a> (%u B)</li>\n",
                         enc, esc, (unsigned)entries[i].size);
        if (n <= 0) break;
        o += (size_t)n;
        if (o >= cap - 64) break;
    }
    s_io.list[o] = '\0';
    s_io.result = ESP_OK;
}

void upload_server_poll(void)
{
    if (s_q == NULL || uxQueueMessagesWaiting(s_q) == 0) {
        return;
    }
    upload_cmd_t cmd;
    if (xQueueReceive(s_q, &cmd, 0) != pdTRUE) {
        return;
    }

    switch (cmd) {
    case CMD_OPEN_WRITE: do_open_write(); break;
    case CMD_WRITE:      do_write(); break;
    case CMD_FINISH:     do_finish(); break;
    case CMD_ABORT:      do_abort(); break;
    case CMD_OPEN_READ:  do_open_read(); break;
    case CMD_READ:       do_read(); break;
    case CMD_CLOSE:      do_close(); break;
    case CMD_LIST:       do_list(); break;
    default:             s_io.result = ESP_ERR_INVALID_ARG; break;
    }
    xSemaphoreGive(s_ack);
}

/*============================================================================
 * HTTP side: post a command and wait for the LVGL task to ack it.
 *============================================================================*/

static esp_err_t send_cmd(upload_cmd_t cmd, TickType_t timeout)
{
    if (xQueueSend(s_q, &cmd, timeout) != pdTRUE) {
        ESP_LOGE(TAG, "mailbox send timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(s_ack, timeout) != pdTRUE) {
        ESP_LOGE(TAG, "ack timeout (LVGL task stalled?)");
        return ESP_ERR_TIMEOUT;
    }
    return s_io.result;
}

/* Accept only sane, flat file names (no paths, no separators, printable
 * bytes incl. UTF-8 so Chinese names work; blocks "../" traversal). */
static bool upload_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > UPLOAD_NAME_MAX) {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

static const char HTML_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>ESP32 SD Upload</title>"
    "<style>body{font-family:sans-serif;max-width:620px;margin:32px auto;padding:0 14px;color:#222}"
    "h2{color:#155}li{margin:5px 0}a{text-decoration:none}input,button{font-size:16px;padding:6px}</style>"
    "</head><body><h2>ESP32 SD Card Upload</h2>"
    "<p>Files are stored flat in <code>/sdcard/esp32_files</code>; duplicate names get "
    "a number appended automatically.</p>"
    "<input type=\"file\" id=\"f\"><button onclick=\"up()\">Upload</button>"
    "<div id=\"st\" style=\"margin:8px 0;color:#c33\"></div><h3>Files</h3><ul>\n";

static const char HTML_FOOT[] =
    "</ul><script>\n"
    "function up(){var f=document.getElementById('f').files[0];if(!f)return;\n"
    "var st=document.getElementById('st');st.textContent='Uploading '+f.name+' ('+f.size+' bytes)...';\n"
    "var x=new XMLHttpRequest();\n"
    "x.open('POST','/upload?name='+encodeURIComponent(f.name),true);\n"
    "x.onload=function(){st.textContent=x.responseText};\n"
    "x.onerror=function(){st.textContent='Upload failed (network error)'};\n"
    "x.send(f);}\n"
    "</script></body></html>\n";

/* Log every request that reaches a handler (debugging: distinguishes
 * "request never arrived" from "handler rejected it"). */
static void log_request(httpd_req_t *req, const char *handler)
{
    ESP_LOGI(TAG, "HTTP %s -> %s (uri=%s)", handler,
             (req->method == HTTP_GET) ? "GET" : "POST", req->uri);
}

/* GET / — upload form + file list with download links */
static esp_err_t index_handler(httpd_req_t *req)
{
    log_request(req, "index");
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "index rejected: no SD card");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
        return ESP_OK;
    }
    if (s_phase != UPLOAD_SERVER_IDLE) {
        ESP_LOGW(TAG, "index rejected: busy (phase=%d)", (int)s_phase);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "server busy");
        return ESP_OK;
    }

    s_phase = UPLOAD_SERVER_BUSY;
    esp_err_t ret = send_cmd(CMD_LIST, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS));
    s_phase = UPLOAD_SERVER_IDLE;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "index: list command failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "list failed");
        return ESP_OK;
    }

    /* Each httpd_resp_send*() call is its OWN complete HTTP response
     * (it writes Content-Length for just that call), so a multi-part body
     * must be sent with httpd_resp_send_chunk (chunked transfer). */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, HTML_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, s_io.list, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, HTML_FOOT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0); /* terminate the chunked body */
    return ESP_OK;
}

/* POST /upload?name=<file> — receive a file into the SD card */
static esp_err_t upload_handler(httpd_req_t *req)
{
    log_request(req, "upload");
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "upload rejected: no SD card");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
        return ESP_OK;
    }
    if (s_phase != UPLOAD_SERVER_IDLE) {
        ESP_LOGW(TAG, "upload rejected: busy (phase=%d)", (int)s_phase);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "server busy");
        return ESP_OK;
    }

    char name[UPLOAD_NAME_MAX + 1] = "";
    char qs[128] = "";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "name", name, sizeof(name));
    }
    if (!upload_name_valid(name)) {
        ESP_LOGW(TAG, "upload rejected: invalid name \"%s\"", name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file name");
        return ESP_OK;
    }
    if (req->content_len > (int)UPLOAD_MAX_SIZE) {
        ESP_LOGW(TAG, "upload rejected: %d bytes too large", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file too large");
        return ESP_OK;
    }

    s_phase = UPLOAD_SERVER_UPLOADING;
    s_written = 0;
    s_total = (uint32_t)(req->content_len > 0 ? req->content_len : 0);
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", name);

    snprintf(s_io.name, sizeof(s_io.name), "%s", name);
    esp_err_t ret = send_cmd(CMD_OPEN_WRITE, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS));
    if (ret != ESP_OK) {
        s_phase = UPLOAD_SERVER_IDLE;
        ESP_LOGW(TAG, "upload of %s failed at open (cmd ret=%s)", name,
                 esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", s_io.final_name);

    bool clean_eof = false;
    while (!s_stop_req) {
        int n = httpd_req_recv(req, (char *)s_io.data, sizeof(s_io.data));
        if (n == 0) {
            clean_eof = true; /* all bytes received */
            break;
        }
        if (n < 0) {
            break; /* client error/timeout */
        }
        s_io.len = (size_t)n;
        if (send_cmd(CMD_WRITE, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS)) != ESP_OK) {
            clean_eof = false;
            break;
        }
    }

    if (clean_eof && !s_stop_req) {
        send_cmd(CMD_FINISH, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS));
        /* One send per response: build the full message first. */
        char resp[UPLOAD_NAME_MAX + 32];
        snprintf(resp, sizeof(resp), "OK: saved as %s", s_io.final_name);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, resp);
        ESP_LOGI(TAG, "uploaded %s (%lu bytes)", s_io.final_name,
                 (unsigned long)s_written);
    } else {
        send_cmd(CMD_ABORT, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "upload aborted");
        ESP_LOGW(TAG, "upload of %s aborted", s_cur_name);
    }

    s_phase = UPLOAD_SERVER_IDLE;
    return ESP_OK;
}

/* GET /download?name=<file> — serve a file from the SD card */
static esp_err_t download_handler(httpd_req_t *req)
{
    log_request(req, "download");
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "download rejected: no SD card");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
        return ESP_OK;
    }
    if (s_phase != UPLOAD_SERVER_IDLE) {
        ESP_LOGW(TAG, "download rejected: busy (phase=%d)", (int)s_phase);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "server busy");
        return ESP_OK;
    }

    char name[UPLOAD_NAME_MAX + 1] = "";
    char qs[128] = "";
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "name", name, sizeof(name));
    }
    if (!upload_name_valid(name)) {
        ESP_LOGW(TAG, "download rejected: invalid name \"%s\"", name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid file name");
        return ESP_OK;
    }

    s_phase = UPLOAD_SERVER_DOWNLOADING;
    s_written = 0;
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", name);
    snprintf(s_io.name, sizeof(s_io.name), "%s", name);

    if (send_cmd(CMD_OPEN_READ, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS)) != ESP_OK) {
        s_phase = UPLOAD_SERVER_IDLE;
        ESP_LOGW(TAG, "download of %s not found", name);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    s_total = s_io.file_size;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    /* No Content-Length here: the body is sent with
     * httpd_resp_send_chunk (Transfer-Encoding: chunked); mixing the two
     * framing headers is invalid HTTP and breaks strict clients. */

    esp_err_t ret = ESP_OK;
    while (!s_stop_req) {
        if (send_cmd(CMD_READ, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS)) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
        if (s_io.len == 0) {
            break; /* EOF */
        }
        if (httpd_resp_send_chunk(req, (const char *)s_io.data, s_io.len) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
        s_written += s_io.len;
    }
    httpd_resp_send_chunk(req, NULL, 0); /* terminate chunked body */
    send_cmd(CMD_CLOSE, pdMS_TO_TICKS(UPLOAD_ACK_TIMEOUT_MS));

    if (ret != ESP_OK && !s_stop_req) {
        ESP_LOGW(TAG, "download of %s failed", s_cur_name);
    }
    s_phase = UPLOAD_SERVER_IDLE;
    return ESP_OK;
}

/*============================================================================
 * Start / stop
 *============================================================================*/

esp_err_t upload_server_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (s_q == NULL) {
        s_q = xQueueCreate(1, sizeof(upload_cmd_t));
        s_ack = xSemaphoreCreateBinary();
        if (s_q == NULL || s_ack == NULL) {
            ESP_LOGE(TAG, "mailbox alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_UPLOAD_SERVER_PORT;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/upload",  .method = HTTP_POST, .handler = upload_handler },
        { .uri = "/download", .method = HTTP_GET, .handler = download_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s", uris[i].uri);
            httpd_stop(s_server);
            s_server = NULL;
            return ESP_FAIL;
        }
    }

    s_running = true;
    s_stop_req = false;
    s_phase = UPLOAD_SERVER_IDLE;
    ESP_LOGI(TAG, "upload server on port %u, dir %s",
             (unsigned)CONFIG_UPLOAD_SERVER_PORT, CONFIG_UPLOAD_DIR);
    return ESP_OK;
}

esp_err_t upload_server_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_stop_req = true; /* in-flight handler aborts within one chunk */
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_running = false;
    s_phase = UPLOAD_SERVER_IDLE;
    s_stop_req = false;
    ESP_LOGI(TAG, "upload server stopped");
    return ESP_OK;
}

bool upload_server_is_running(void)
{
    return s_running;
}

esp_err_t upload_server_get_status(upload_server_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->running = s_running;
    out->port = CONFIG_UPLOAD_SERVER_PORT;
    out->phase = s_phase;
    snprintf(out->filename, sizeof(out->filename), "%s", s_cur_name);
    out->written = s_written;
    out->total = s_total;
    return ESP_OK;
}

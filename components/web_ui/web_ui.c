/* web_ui.c — esp_http_server: statikus UI + REST API + WebSocket. (terv 12.2)
 *
 * - Statikus UI a /lfs/www alól (content-type a kiterjesztésből), fallback
 *   beágyazott minimál HTML, ha nincs index.html.
 * - REST JSON: /api/files, /api/upload (streamelt), /api/download,
 *   /api/file (DELETE), /api/program (501), /api/cfg/pull, /api/cfg/push.
 * - WebSocket /ws: kliens-fd lista + broadcast; ~1 mp-es esp_timer a
 *   target_state élő adatának szétküldéséhez.
 *
 * BIZTONSÁG (terv 12.2): a felület célt törölni/flashelni tud, ezért
 * AP-módban legalább opcionális Basic Auth / token ERŐSEN AJÁNLOTT.
 * Itt egyelőre NINCS auth — egy belépési pont (auth_check) jelölve, ahol
 * a tokent/Basic Auth-ot be lehet kötni minden handler elejére.
 */
#include "web_ui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_lfs.h"
#include "target_state.h"
#include "target_serial.h"
#include "prog_session.h"   /* SWD flash orchestráció (prog_session_flash_file) */

#include "freertos/task.h"

static const char *TAG = "web_ui";

/* ---- belső állapot ---- */

#define WS_MAX_CLIENTS    8        /* egyidejű WS kliensek max száma */
#define UPLOAD_CHUNK      2048     /* streamelt feltöltés chunk mérete */
#define LIVE_PERIOD_US    1000000  /* élő-adat broadcast periódus (~1 mp) */

static httpd_handle_t s_server = NULL;
static esp_timer_handle_t s_live_timer = NULL;

/* WS kliens-fd-k. Védi az s_ws_mux; a tényleges küldést a httpd worker végzi. */
static int s_ws_fds[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mux = NULL;

/* ---- segédek ---- */

/* Egyszerű kiterjesztés -> MIME leképezés a statikus kiszolgáláshoz. */
static const char *mime_from_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".js"))   return "application/javascript";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".gif"))  return "image/gif";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".ico"))  return "image/x-icon";
    if (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".cfg")) return "text/plain";
    if (!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".hex")) return "application/octet-stream";
    return "application/octet-stream";
}

/* Query paraméter kiolvasása az URL-ből (méret-ellenőrzött). */
static esp_err_t get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char *qbuf = NULL;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) return ESP_ERR_NOT_FOUND;
    qbuf = malloc(qlen);
    if (!qbuf) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (httpd_req_get_url_query_str(req, qbuf, qlen) == ESP_OK) {
        if (httpd_query_key_value(qbuf, key, out, out_len) == ESP_OK) {
            err = ESP_OK;
        }
    }
    free(qbuf);
    return err;
}

/* JSON hibaválasz egy adott HTTP státusszal. */
static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* BIZTONSÁGI BELÉPÉSI PONT (terv 12.2): ide jön a Basic Auth / token
 * ellenőrzés. Most mindig OK-t ad vissza (nincs auth). Bekötéskor minden
 * REST/WS handler elején hívd, és 401-gyel térj vissza, ha nem hitelesített. */
static esp_err_t auth_check(httpd_req_t *req)
{
    (void)req;
    /* TODO: Basic Auth / token ellenőrzés AP-módban (terv 12.2). */
    return ESP_OK;
}

/* Biztonságos path-összefűzés a /lfs alá. A bejövő "path" pl. "/fw/x.bin".
 * Megakadályozza a ".." kilépést. Sikerkor a teljes elérési utat adja. */
static esp_err_t build_lfs_path(const char *rel, char *out, size_t out_len)
{
    if (!rel || rel[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strstr(rel, "..")) return ESP_ERR_INVALID_ARG;   /* path traversal tiltás */
    const char *sep = (rel[0] == '/') ? "" : "/";
    int n = snprintf(out, out_len, "%s%s%s", STORAGE_LFS_BASE, sep, rel);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

/* ===================================================================== */
/* Statikus UI kiszolgálás                                               */
/* ===================================================================== */

/* Beágyazott minimál UI, ha nincs /lfs/www/index.html. Listázza a fw/cfg
 * fájlokat és van egy WS élő-adat doboz. */
static const char k_fallback_html[] =
"<!DOCTYPE html><html lang=\"hu\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>ESP32-S3 SWD Programmer</title>"
"<style>body{font-family:sans-serif;margin:1.5rem;background:#111;color:#ddd}"
"h1{font-size:1.2rem}section{margin:1rem 0;padding:.8rem;border:1px solid #333;border-radius:6px}"
"pre{background:#000;padding:.6rem;border-radius:4px;min-height:3rem;white-space:pre-wrap}"
"li{margin:.2rem 0}</style></head><body>"
"<h1>ESP32-S3 SWD Programmer</h1>"
"<section><h2>Firmware (/lfs/fw)</h2><ul id=\"fw\"></ul></section>"
"<section><h2>Config (/lfs/cfg)</h2><ul id=\"cfg\"></ul></section>"
"<section><h2>Live (WebSocket)</h2><pre id=\"live\">connecting...</pre></section>"
"<script>"
"async function load(dir,el){try{let r=await fetch('/api/files?dir='+dir);"
"let j=await r.json();document.getElementById(el).innerHTML="
"j.map(f=>'<li>'+f.name+' ('+f.size+' B)</li>').join('')||'<li><i>empty</i></li>';}"
"catch(e){document.getElementById(el).innerHTML='<li>error</li>';}}"
"load('fw','fw');load('cfg','cfg');"
"let ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onmessage=e=>{document.getElementById('live').textContent=e.data;};"
"ws.onclose=()=>{document.getElementById('live').textContent='disconnected';};"
"ws.onerror=()=>{document.getElementById('live').textContent='ws error';};"
"</script></body></html>";

/* Fájl chunkokban való kiküldése (download és statikus is ezt használja).
 * A storage lockot a hívás teljes idejére fogja. */
static esp_err_t send_file_chunked(httpd_req_t *req, const char *full_path)
{
    storage_lfs_lock();
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        storage_lfs_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        fclose(f);
        storage_lfs_unlock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    size_t rd;
    while ((rd = fread(buf, 1, UPLOAD_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    free(buf);
    fclose(f);
    storage_lfs_unlock();
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);   /* lezáró chunk */
    }
    return ret;
}

/* Statikus fájl a /lfs/www alól (wildcard GET), vagy fallback HTML. */
static esp_err_t static_get_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }

    /* URI -> /lfs/www/<uri> ; "/" -> index.html */
    const char *uri = req->uri;
    char rel[160];
    if (strcmp(uri, "/") == 0) {
        snprintf(rel, sizeof(rel), "/www/index.html");
    } else {
        /* a query részt vágjuk le */
        const char *q = strchr(uri, '?');
        size_t ulen = q ? (size_t)(q - uri) : strlen(uri);
        if (ulen >= sizeof(rel) - 8) ulen = sizeof(rel) - 8;
        snprintf(rel, sizeof(rel), "/www%.*s", (int)ulen, uri);
    }

    char full[200];
    if (build_lfs_path(rel, full, sizeof(full)) == ESP_OK) {
        httpd_resp_set_type(req, mime_from_path(full));
        esp_err_t r = send_file_chunked(req, full);
        if (r == ESP_OK) return ESP_OK;
        /* ha nem létezett a fájl -> fallback (csak a gyökérnél van értelme) */
    }

    /* Fallback minimál UI (nincs UI-asset feltöltve). */
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, k_fallback_html, HTTPD_RESP_USE_STRLEN);
}

/* ===================================================================== */
/* REST: GET /api/files?dir=fw|cfg                                        */
/* ===================================================================== */

typedef struct {
    char  *json;     /* dinamikus buffer */
    size_t cap;
    size_t len;
    bool   first;
    bool   oom;
} files_ctx_t;

static void files_append(files_ctx_t *c, const char *s, size_t n)
{
    if (c->oom) return;
    if (c->len + n + 1 > c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 512;
        while (ncap < c->len + n + 1) ncap *= 2;
        char *nb = realloc(c->json, ncap);
        if (!nb) { c->oom = true; return; }
        c->json = nb;
        c->cap = ncap;
    }
    memcpy(c->json + c->len, s, n);
    c->len += n;
    c->json[c->len] = '\0';
}

/* storage_lfs_list callback: minden bejegyzést JSON objektumként fűz hozzá. */
static void files_list_cb(const char *name, size_t size, bool is_dir, void *ctx)
{
    files_ctx_t *c = ctx;
    if (is_dir) return;   /* csak fájlokat listázunk */
    char item[160];
    /* a név idézőjelét nem escape-eljük: a fájlnevekben nem várunk " jelet */
    int n = snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%u}",
                     c->first ? "" : ",", name, (unsigned)size);
    if (n > 0) files_append(c, item, (size_t)n);
    c->first = false;
}

static esp_err_t api_files_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    char dir[16] = {0};
    if (get_query_param(req, "dir", dir, sizeof(dir)) != ESP_OK ||
        (strcmp(dir, "fw") != 0 && strcmp(dir, "cfg") != 0)) {
        return send_json_error(req, "400 Bad Request", "dir must be fw or cfg");
    }

    char full[64];
    snprintf(full, sizeof(full), "%s/%s", STORAGE_LFS_BASE, dir);

    files_ctx_t c = { .json = NULL, .cap = 0, .len = 0, .first = true, .oom = false };
    files_append(&c, "[", 1);

    /* A listázás a storage lock alatt (a storage_lfs_list maga fogja). */
    esp_err_t lr = storage_lfs_list(full, files_list_cb, &c);
    files_append(&c, "]", 1);

    if (c.oom || !c.json) {
        free(c.json);
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    if (lr != ESP_OK) {
        ESP_LOGW(TAG, "list(%s) hiba: %s", full, esp_err_to_name(lr));
        /* üres tömböt adunk vissza, ha a könyvtár nem listázható */
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, c.json, c.len);
    free(c.json);
    return r;
}

/* ===================================================================== */
/* REST: POST /api/upload?path=/fw/x.bin  (STREAMELT)                     */
/* ===================================================================== */

static esp_err_t api_upload_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    char path[160] = {0};
    if (get_query_param(req, "path", path, sizeof(path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "missing path");
    }
    char full[200];
    if (build_lfs_path(path, full, sizeof(full)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid path");
    }

    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }

    /* A teljes feltöltés idejére fogjuk a storage lockot, és a body-t
     * chunkonként olvassuk -> nem töltjük a teljes fájlt heapbe. */
    storage_lfs_lock();
    FILE *f = fopen(full, "wb");
    if (!f) {
        storage_lfs_unlock();
        free(buf);
        return send_json_error(req, "500 Internal Server Error", "open failed");
    }

    int remaining = req->content_len;
    esp_err_t err = ESP_OK;
    size_t written = 0;
    while (remaining > 0) {
        int to_read = MIN(remaining, UPLOAD_CHUNK);
        int r = httpd_req_recv(req, buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;   /* timeout -> újrapróbál */
        }
        if (r <= 0) { err = ESP_FAIL; break; }
        if (fwrite(buf, 1, r, f) != (size_t)r) { err = ESP_FAIL; break; }
        written += r;
        remaining -= r;
    }
    fclose(f);
    storage_lfs_unlock();
    free(buf);

    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "write failed");
    }

    char ok[256];
    snprintf(ok, sizeof(ok), "{\"ok\":true,\"path\":\"%s\",\"size\":%u}",
             path, (unsigned)written);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok);
}

/* ===================================================================== */
/* REST: GET /api/download?path=/fw/x.bin                                 */
/* ===================================================================== */

static esp_err_t api_download_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    char path[160] = {0};
    if (get_query_param(req, "path", path, sizeof(path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "missing path");
    }
    char full[200];
    if (build_lfs_path(path, full, sizeof(full)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid path");
    }

    /* letöltésként ajánljuk fel (a fájlnévvel) */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char disp[200];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, mime_from_path(full));

    esp_err_t r = send_file_chunked(req, full);
    if (r == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "file not found");
    }
    return r;
}

/* ===================================================================== */
/* REST: DELETE /api/file?path=/fw/x.bin                                  */
/* ===================================================================== */

static esp_err_t api_delete_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    char path[160] = {0};
    if (get_query_param(req, "path", path, sizeof(path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "missing path");
    }
    char full[200];
    if (build_lfs_path(path, full, sizeof(full)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid path");
    }

    storage_lfs_lock();
    int rc = remove(full);
    storage_lfs_unlock();

    if (rc != 0) {
        return send_json_error(req, "404 Not Found", "delete failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ===================================================================== */
/* REST: cfg pull / push (soros hídon át)                                 */
/* ===================================================================== */

static esp_err_t api_cfg_pull_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    char name[64] = {0};
    if (get_query_param(req, "name", name, sizeof(name)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "missing name");
    }
    esp_err_t r = target_serial_cfg_pull(name);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "cfg_pull(%s) hiba: %s", name, esp_err_to_name(r));
        return send_json_error(req, "502 Bad Gateway", "cfg pull failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_cfg_push_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }
    char name[64] = {0};
    if (get_query_param(req, "name", name, sizeof(name)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "missing name");
    }
    esp_err_t r = target_serial_cfg_push(name);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "cfg_push(%s) hiba: %s", name, esp_err_to_name(r));
        return send_json_error(req, "502 Bad Gateway", "cfg push failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ===================================================================== */
/* REST: POST /api/program?file=/fw/x.bin  -> prog_session flash         */
/* ===================================================================== */

/* prog_phase_t -> rövid név a WS JSON-höz. */
static const char *prog_phase_name(prog_phase_t p)
{
    switch (p) {
    case PROG_IDLE:    return "idle";
    case PROG_CONNECT: return "connect";
    case PROG_ERASE:   return "erase";
    case PROG_PROGRAM: return "program";
    case PROG_VERIFY:  return "verify";
    case PROG_DONE:    return "done";
    case PROG_FAILED:  return "failed";
    default:           return "?";
    }
}

/* prog_session progress callback: a flash-taszk kontextusából hívódik.
   A státuszból JSON-t épít és a /ws klienseknek broadcastolja. */
static void web_flash_cb(const prog_status_t *st, void *ctx)
{
    (void)ctx;
    if (!st) return;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"prog\","
             "\"phase\":%d,"
             "\"phase_name\":\"%s\","
             "\"percent\":%d,"
             "\"dev_id\":%u,"
             "\"target_name\":\"%s\","
             "\"message\":\"%s\"}",
             (int)st->phase,
             prog_phase_name(st->phase),
             st->percent,
             (unsigned)st->dev_id,
             st->target_name,
             st->message);

    web_ui_ws_broadcast(json);
}

/* Külön FreeRTOS taszk: lefuttatja a szinkron flash-t, majd kilép.
   Az argumentum egy malloc-olt /lfs-abszolút útvonal (a taszk free-zi). */
static void web_flash_task(void *arg)
{
    char *full = (char *)arg;
    ESP_LOGI(TAG, "flash taszk indul: %s", full);

    esp_err_t err = prog_session_flash_file(full, 0, web_flash_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flash hiba: %s", esp_err_to_name(err));
        /* Záró hibaüzenet a klienseknek (a cb FAILED-et is küldhetett már). */
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"type\":\"prog\",\"phase\":%d,\"phase_name\":\"failed\","
                 "\"percent\":0,\"message\":\"%s\"}",
                 (int)PROG_FAILED, esp_err_to_name(err));
        web_ui_ws_broadcast(json);
    } else {
        ESP_LOGI(TAG, "flash kesz: %s", full);
    }

    free(full);
    vTaskDelete(NULL);
}

static esp_err_t api_program_handler(httpd_req_t *req)
{
    if (auth_check(req) != ESP_OK) {
        return send_json_error(req, "401 Unauthorized", "unauthorized");
    }

    /* Ha már fut egy flash, ne indítsunk másodikat. */
    if (prog_session_busy()) {
        return send_json_error(req, "409 Conflict", "busy");
    }

    /* 'file' query param (pl. "/fw/x.bin"), majd /lfs alá normalizálva.
       A prog_session a kapott utat közvetlenül a storage_lfs_read_all-nak
       adja, ami /lfs-abszolút utat vár -> a teljes "/lfs/fw/..." kell. */
    char file[160] = {0};
    if (get_query_param(req, "file", file, sizeof(file)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "missing file");
    }
    char full[200];
    if (build_lfs_path(file, full, sizeof(full)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid file");
    }

    /* A teljes utat a taszknak adjuk át (a taszk free-zi). */
    char *arg = strdup(full);
    if (!arg) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }

    /* Külön taszk -> a handler azonnal visszatér (202), a progress a /ws-en. */
    BaseType_t ok = xTaskCreate(web_flash_task, "web_flash", 8192, arg, 5, NULL);
    if (ok != pdPASS) {
        free(arg);
        return send_json_error(req, "500 Internal Server Error", "task create failed");
    }

    ESP_LOGI(TAG, "flash kerelem: %s", full);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"started\":true}");
}

/* ===================================================================== */
/* WebSocket /ws                                                          */
/* ===================================================================== */

static void ws_add_fd(int fd)
{
    xSemaphoreTake(s_ws_mux, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) { xSemaphoreGive(s_ws_mux); return; }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fds[i] < 0) { s_ws_fds[i] = fd; break; }
    }
    xSemaphoreGive(s_ws_mux);
}

static void ws_remove_fd(int fd)
{
    xSemaphoreTake(s_ws_mux, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) s_ws_fds[i] = -1;
    }
    xSemaphoreGive(s_ws_mux);
}

/* WS handshake + bejövő keretek. A handshake-kor (GET) regisztráljuk az fd-t. */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* handshake befejeződött, a kliens fel van véve */
        int fd = httpd_req_to_sockfd(req);
        ws_add_fd(fd);
        ESP_LOGI(TAG, "WS kliens csatlakozott, fd=%d", fd);
        return ESP_OK;
    }

    /* Bejövő keret beolvasása (a klienstől). Élő adatra nincs szükség
     * tartalomra, de a frame-et le kell olvasni; close-ra leiratkozunk. */
    httpd_ws_frame_t frame = { 0 };
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    if (frame.len > 0 && frame.len < 1024) {
        uint8_t *payload = calloc(1, frame.len + 1);
        if (payload) {
            frame.payload = payload;
            httpd_ws_recv_frame(req, &frame, frame.len);
            /* jelenleg nem dolgozunk fel kliens-üzenetet (jövőbeni parancsok) */
            free(payload);
        }
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_remove_fd(httpd_req_to_sockfd(req));
    }
    return ESP_OK;
}

/* Egy adott fd-re aszinkron WS textframe küldése (a httpd worker-en). */
typedef struct {
    httpd_handle_t hd;
    int            fd;
    char          *msg;     /* malloc-olt, a callback free-zi */
    size_t         len;
} ws_async_t;

static void ws_async_send(void *arg)
{
    ws_async_t *a = arg;
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)a->msg,
        .len = a->len,
    };
    esp_err_t r = httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    if (r != ESP_OK) {
        ws_remove_fd(a->fd);   /* küldés bukott -> kliens eltávolítása */
    }
    free(a->msg);
    free(a);
}

esp_err_t web_ui_ws_broadcast(const char *json)
{
    if (!s_server || !json) return ESP_ERR_INVALID_STATE;
    size_t len = strlen(json);

    /* Az aktuális fd-listát kimásoljuk a lock alatt, a küldést azon kívül
     * ütemezzük (httpd_queue_work). */
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    xSemaphoreTake(s_ws_mux, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fds[i] >= 0) fds[n++] = s_ws_fds[i];
    }
    xSemaphoreGive(s_ws_mux);

    for (int i = 0; i < n; i++) {
        ws_async_t *a = malloc(sizeof(*a));
        if (!a) continue;
        a->hd = s_server;
        a->fd = fds[i];
        a->msg = malloc(len + 1);
        if (!a->msg) { free(a); continue; }
        memcpy(a->msg, json, len + 1);
        a->len = len;
        if (httpd_queue_work(s_server, ws_async_send, a) != ESP_OK) {
            free(a->msg);
            free(a);
        }
    }
    return ESP_OK;
}

/* ~1 mp-es timer: target_state -> JSON -> broadcast. */
static void live_timer_cb(void *arg)
{
    (void)arg;
    target_state_t st;
    target_state_get(&st);

    /* values tömb felépítése */
    char vals[256];
    size_t vp = 0;
    vp += snprintf(vals + vp, sizeof(vals) - vp, "[");
    for (int i = 0; i < st.value_count && i < TARGET_STATE_MAX_VALUES; i++) {
        vp += snprintf(vals + vp, sizeof(vals) - vp, "%s%.4g",
                       i ? "," : "", st.values[i]);
        if (vp >= sizeof(vals) - 8) break;
    }
    vp += snprintf(vals + vp, sizeof(vals) - vp, "]");

    char json[512];
    snprintf(json, sizeof(json),
             "{\"type\":\"state\","
             "\"target_present\":%s,"
             "\"dev_id\":%u,"
             "\"target_name\":\"%s\","
             "\"serial_link\":%s,"
             "\"uptime_s\":%u,"
             "\"values\":%s}",
             st.target_present ? "true" : "false",
             (unsigned)st.dev_id,
             st.target_name,
             st.serial_link ? "true" : "false",
             (unsigned)st.uptime_s,
             vals);

    web_ui_ws_broadcast(json);
}

/* ===================================================================== */
/* Init                                                                  */
/* ===================================================================== */

/* URI-handler regisztráló segéd. */
static esp_err_t reg(httpd_handle_t s, const char *uri, httpd_method_t m,
                     esp_err_t (*h)(httpd_req_t *), bool is_ws)
{
    httpd_uri_t u = {
        .uri = uri,
        .method = m,
        .handler = h,
        .user_ctx = NULL,
        .is_websocket = is_ws,
    };
    return httpd_register_uri_handler(s, &u);
}

esp_err_t web_ui_init(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "már fut");
        return ESP_OK;
    }

    for (int i = 0; i < WS_MAX_CLIENTS; i++) s_ws_fds[i] = -1;
    s_ws_mux = xSemaphoreCreateMutex();
    if (!s_ws_mux) return ESP_ERR_NO_MEM;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;        /* megemelt handler-szám */
    config.lru_purge_enable = true;      /* régi kapcsolatok ürítése */
    config.recv_wait_timeout = 10;       /* s */
    config.send_wait_timeout = 10;       /* s */
    config.uri_match_fn = httpd_uri_match_wildcard;  /* wildcard GET miatt */

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start hiba: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_ws_mux);
        s_ws_mux = NULL;
        return err;
    }

    /* REST végpontok (a wildcard statikus handler ELÉ kell regisztrálni) */
    reg(s_server, "/api/files",    HTTP_GET,    api_files_handler,    false);
    reg(s_server, "/api/upload",   HTTP_POST,   api_upload_handler,   false);
    reg(s_server, "/api/download", HTTP_GET,    api_download_handler, false);
    reg(s_server, "/api/file",     HTTP_DELETE, api_delete_handler,   false);
    reg(s_server, "/api/program",  HTTP_POST,   api_program_handler,  false);
    reg(s_server, "/api/cfg/pull", HTTP_POST,   api_cfg_pull_handler, false);
    reg(s_server, "/api/cfg/push", HTTP_POST,   api_cfg_push_handler, false);

    /* WebSocket */
    reg(s_server, "/ws", HTTP_GET, ws_handler, true);

    /* Statikus UI — wildcard, mindent elkap, ami fent nem illeszkedett. */
    reg(s_server, "/*", HTTP_GET, static_get_handler, false);

    /* Élő-adat timer indítása (~1 mp). */
    const esp_timer_create_args_t targs = {
        .callback = live_timer_cb,
        .name = "web_ui_live",
    };
    if (esp_timer_create(&targs, &s_live_timer) == ESP_OK) {
        esp_timer_start_periodic(s_live_timer, LIVE_PERIOD_US);
    } else {
        ESP_LOGW(TAG, "élő-adat timer nem indult");
    }

    ESP_LOGI(TAG, "web UI elindult (httpd)");
    return ESP_OK;
}

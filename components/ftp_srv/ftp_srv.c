/* Minimális FTP szerver a LittleFS (/lfs) fölött. (terv 12.3)
 *
 * Passzív mód, plaintext (csak LAN). 1–2 egyidejű session. A fájlokat
 * közvetlen POSIX hívásokkal éri el a /lfs VFS-en (fopen/fread/fwrite/
 * opendir/readdir/stat/remove), a storage_lfs közös lockja alatt.
 *
 * Támogatott parancsok:
 *   USER PASS SYST FEAT PWD XPWD CWD CDUP TYPE PASV
 *   LIST NLST RETR STOR DELE SIZE NOOP QUIT
 *
 * Megjegyzések:
 *  - PASV: ephemeral data listener-t nyitunk, a 227-ben a control socket
 *    helyi címét hirdetjük (getsockname) — így a kliens a szerver tényleges
 *    IP-jét kapja, NAT/több-interfész nélkül megbízható.
 *  - Path-traversal védelem: minden virtuális utat a /lfs prefixre képezünk,
 *    a "."/".." komponenseket normalizáljuk, a /lfs gyökér alól kilépni nem
 *    lehet (a ".." a gyökéren megáll).
 *  - Lock-stratégia: a storage_lfs_lock()-ot a tényleges fájl-I/O köré fogjuk.
 *    Egyszerűsítés: RETR/STOR esetén a teljes transzfer a lock alatt fut
 *    (megnyitás + chunkolt olvasás/írás + zárás), így a LittleFS nem
 *    konkurrál a web_ui-val. A control/parancs-parsing és a hálózati
 *    accept() NEM tartja a lockot.
 */
#include "ftp_srv.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "storage_lfs.h"

static const char *TAG = "ftp_srv";

/* ---- Konfiguráció ---- */
#define FTP_CTRL_PORT     21
#define FTP_MAX_SESSIONS  2          /* egyidejű session-ök maximuma */
#define FTP_CMD_BUF       512        /* egy parancssor max hossza */
#define FTP_PATH_MAX      256        /* virtuális útvonal max */
#define FTP_XFER_BUF      2048       /* data csatorna chunk méret */
#define FTP_DATA_ACCEPT_TO 15        /* data accept timeout (s) */
#define FTP_CTRL_STACK    6144       /* session taszk stack (byte) */

/* Aktív session-számláló (egyszerű kliens-limit). */
static volatile int s_active_sessions = 0;
static SemaphoreHandle_t s_sess_mtx = NULL;

/* Egy session minden állapota. */
typedef struct {
    int  ctrl_sock;                  /* control kapcsolat */
    int  pasv_listen;                /* PASV data listener (-1 ha nincs) */
    bool type_binary;                /* TYPE I (true) / TYPE A (false) */
    char cwd[FTP_PATH_MAX];          /* virtuális CWD, "/"-vel kezdődik (a /lfs-hez relatív) */
} ftp_sess_t;

/* ============================ Útvonal-kezelés ============================ */

/* Virtuális út normalizálása helyben: "." kihagyás, ".." visszalépés,
 * dupla "/" összevonás. Az eredmény mindig "/"-vel kezdődik, a gyökér alól
 * nem lép ki (path-traversal védelem). 'base' az aktuális CWD (abszolút
 * virtuális), 'arg' a parancs argumentuma (lehet abszolút vagy relatív). */
static void ftp_resolve(const char *base, const char *arg, char *out, size_t out_sz)
{
    char tmp[FTP_PATH_MAX * 2];

    if (arg && arg[0] == '/') {
        /* abszolút virtuális út */
        snprintf(tmp, sizeof(tmp), "%s", arg);
    } else if (arg && arg[0] != '\0') {
        /* relatív a base-hez */
        snprintf(tmp, sizeof(tmp), "%s/%s", base, arg);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", base);
    }

    /* Komponensenként normalizálás egy stackbe. */
    const char *parts[64];
    int depth = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (depth > 0) depth--;     /* visszalépés, de a gyökeren megáll */
            continue;
        }
        if (depth < (int)(sizeof(parts) / sizeof(parts[0]))) {
            parts[depth++] = tok;
        }
    }

    /* Összefűzés "/"-vel. */
    size_t pos = 0;
    out[0] = '/';
    out[1] = '\0';
    pos = 1;
    for (int i = 0; i < depth; i++) {
        int n = snprintf(out + pos, out_sz - pos, "%s%s",
                         (i == 0 ? "" : "/"), parts[i]);
        if (n < 0 || (size_t)n >= out_sz - pos) break;
        pos += n;
    }
    /* Ha üres maradt (gyökér), az "/" már bent van. */
    if (pos == 1) {
        out[0] = '/';
        out[1] = '\0';
    }
}

/* Virtuális útból (/...) valódi POSIX út a /lfs alatt. */
static void ftp_to_fs(const char *vpath, char *out, size_t out_sz)
{
    if (strcmp(vpath, "/") == 0) {
        snprintf(out, out_sz, "%s", STORAGE_LFS_BASE);
    } else {
        snprintf(out, out_sz, "%s%s", STORAGE_LFS_BASE, vpath);
    }
}

/* ============================ Control I/O ============================ */

/* Válaszsor küldése a control csatornán (CRLF-fel). */
static int ftp_reply(ftp_sess_t *s, const char *line)
{
    char buf[FTP_CMD_BUF];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", line);
    if (n < 0) return -1;
    return send(s->ctrl_sock, buf, n, 0);
}

static int ftp_replyf(ftp_sess_t *s, const char *fmt, ...)
{
    char line[FTP_CMD_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return ftp_reply(s, line);
}

/* Egy parancssor beolvasása a control socketről CRLF-ig (vagy LF-ig).
 * A záró CR/LF-et levágja. Visszatérés: a sor hossza, 0 = lezárt/üres,
 * -1 = hiba/kapcsolat zárva. */
static int ftp_read_line(ftp_sess_t *s, char *buf, size_t buf_sz)
{
    size_t len = 0;
    while (len < buf_sz - 1) {
        char c;
        int r = recv(s->ctrl_sock, &c, 1, 0);
        if (r <= 0) {
            return (len > 0) ? (int)len : -1;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;                 /* CR-t eldobjuk */
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return (int)len;
}

/* ============================ PASV data csatorna ============================ */

/* PASV listener nyitása ephemeral porton. A control socket helyi IP-jét
 * hirdetjük a 227-ben (getsockname). 0 = ok, -1 = hiba. */
static int ftp_open_pasv(ftp_sess_t *s)
{
    /* Korábbi PASV listener bezárása, ha maradt. */
    if (s->pasv_listen >= 0) {
        close(s->pasv_listen);
        s->pasv_listen = -1;
    }

    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;                /* ephemeral port */

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(ls, 1) != 0) {
        close(ls);
        return -1;
    }

    /* A ténylegesen kiosztott port lekérése. */
    struct sockaddr_in la = {0};
    socklen_t la_len = sizeof(la);
    if (getsockname(ls, (struct sockaddr *)&la, &la_len) != 0) {
        close(ls);
        return -1;
    }
    uint16_t port = ntohs(la.sin_port);

    /* Szerver IP a control socket helyi címéből. */
    struct sockaddr_in ca = {0};
    socklen_t ca_len = sizeof(ca);
    if (getsockname(s->ctrl_sock, (struct sockaddr *)&ca, &ca_len) != 0) {
        close(ls);
        return -1;
    }
    uint32_t ip = ntohl(ca.sin_addr.s_addr);

    s->pasv_listen = ls;

    /* 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
    ftp_replyf(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8) & 0xFF, ip & 0xFF,
               (port >> 8) & 0xFF, port & 0xFF);
    return 0;
}

/* A PASV listenerre érkező data kapcsolat fogadása (timeouttal).
 * Visszaadja a data socketet, vagy -1-et. A listenert minden esetben
 * lezárja (egy data kapcsolat / PASV). */
static int ftp_accept_data(ftp_sess_t *s)
{
    if (s->pasv_listen < 0) return -1;

    struct timeval tv = { .tv_sec = FTP_DATA_ACCEPT_TO, .tv_usec = 0 };
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s->pasv_listen, &rfds);

    int ds = -1;
    int sel = select(s->pasv_listen + 1, &rfds, NULL, NULL, &tv);
    if (sel > 0 && FD_ISSET(s->pasv_listen, &rfds)) {
        ds = accept(s->pasv_listen, NULL, NULL);
    }

    close(s->pasv_listen);
    s->pasv_listen = -1;
    return ds;
}

/* ============================ LIST / NLST ============================ */

/* UNIX-szerű "ls -l" sor formázása egy bejegyzéshez. */
static void ftp_fmt_list_line(char *out, size_t out_sz,
                              const char *name, bool is_dir, long size)
{
    /* Fix dátum (az LFS nem feltétlen ad mtime-ot) — a kliensek elnézik. */
    const char type = is_dir ? 'd' : '-';
    snprintf(out, out_sz,
             "%crw-r--r-- 1 ftp ftp %10ld Jan  1 00:00 %s\r\n",
             type, size, name);
}

/* LIST (long=true) vagy NLST (long=false) a megadott valós könyvtárról,
 * a data socketre. A storage lockot a teljes listázás idejére fogjuk. */
static void ftp_do_list(int data_sock, const char *fs_dir, bool longfmt)
{
    char line[FTP_PATH_MAX + 96];

    storage_lfs_lock();
    DIR *d = opendir(fs_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            if (longfmt) {
                bool is_dir = false;
                long size = 0;
                char full[FTP_PATH_MAX * 2];
                snprintf(full, sizeof(full), "%s/%s", fs_dir, de->d_name);
                struct stat st;
                if (stat(full, &st) == 0) {
                    is_dir = S_ISDIR(st.st_mode);
                    size = (long)st.st_size;
                } else {
                    is_dir = (de->d_type == DT_DIR);
                }
                ftp_fmt_list_line(line, sizeof(line), de->d_name, is_dir, size);
            } else {
                snprintf(line, sizeof(line), "%s\r\n", de->d_name);
            }
            send(data_sock, line, strlen(line), 0);
        }
        closedir(d);
    }
    storage_lfs_unlock();
}

/* ============================ RETR / STOR ============================ */

/* Fájl letöltése (RETR) a data csatornára. 0 = ok, -1 = hiba.
 * A teljes transzfer a storage lock alatt fut (egyszerűsítés, lásd fent). */
static int ftp_do_retr(int data_sock, const char *fs_path)
{
    int rc = -1;
    char *buf = malloc(FTP_XFER_BUF);
    if (!buf) return -1;

    storage_lfs_lock();
    FILE *f = fopen(fs_path, "rb");
    if (f) {
        rc = 0;
        size_t n;
        while ((n = fread(buf, 1, FTP_XFER_BUF, f)) > 0) {
            size_t off = 0;
            while (off < n) {
                int w = send(data_sock, buf + off, n - off, 0);
                if (w <= 0) { rc = -1; break; }
                off += w;
            }
            if (rc != 0) break;
        }
        fclose(f);
    }
    storage_lfs_unlock();

    free(buf);
    return rc;
}

/* Fájl feltöltése (STOR) a data csatornáról. 0 = ok, -1 = hiba.
 * A teljes transzfer a storage lock alatt fut (egyszerűsítés, lásd fent). */
static int ftp_do_stor(int data_sock, const char *fs_path)
{
    int rc = -1;
    char *buf = malloc(FTP_XFER_BUF);
    if (!buf) return -1;

    storage_lfs_lock();
    FILE *f = fopen(fs_path, "wb");
    if (f) {
        rc = 0;
        int n;
        while ((n = recv(data_sock, buf, FTP_XFER_BUF, 0)) > 0) {
            size_t off = 0;
            while (off < (size_t)n) {
                size_t w = fwrite(buf + off, 1, n - off, f);
                if (w == 0) { rc = -1; break; }
                off += w;
            }
            if (rc != 0) break;
        }
        if (n < 0) rc = -1;          /* recv hiba */
        fclose(f);
    }
    storage_lfs_unlock();

    free(buf);
    return rc;
}

/* ============================ Session állapotgép ============================ */

/* Egy parancs feldolgozása. Visszatérés: 0 = folytatás, 1 = QUIT (zárás). */
static int ftp_handle_cmd(ftp_sess_t *s, char *line)
{
    /* Parancs és argumentum szétválasztása az első szóköznél. */
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    } else {
        arg = "";
    }

    /* Parancsnév nagybetűsítése (case-insensitive). */
    for (char *p = line; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }

    if (strcmp(line, "USER") == 0) {
        ftp_reply(s, "331 User name okay, need password.");
    } else if (strcmp(line, "PASS") == 0) {
        ftp_reply(s, "230 User logged in.");
    } else if (strcmp(line, "SYST") == 0) {
        ftp_reply(s, "215 UNIX Type: L8");
    } else if (strcmp(line, "FEAT") == 0) {
        ftp_reply(s, "211-Features:");
        ftp_reply(s, " SIZE");
        ftp_reply(s, "211 End");
    } else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
        ftp_replyf(s, "257 \"%s\" is current directory.", s->cwd);
    } else if (strcmp(line, "CWD") == 0) {
        char vp[FTP_PATH_MAX], fp[FTP_PATH_MAX * 2];
        ftp_resolve(s->cwd, arg, vp, sizeof(vp));
        ftp_to_fs(vp, fp, sizeof(fp));
        struct stat st;
        bool ok;
        storage_lfs_lock();
        ok = (stat(fp, &st) == 0 && S_ISDIR(st.st_mode));
        storage_lfs_unlock();
        if (ok) {
            snprintf(s->cwd, sizeof(s->cwd), "%s", vp);
            ftp_reply(s, "250 Directory changed.");
        } else {
            ftp_reply(s, "550 No such directory.");
        }
    } else if (strcmp(line, "CDUP") == 0) {
        char vp[FTP_PATH_MAX];
        ftp_resolve(s->cwd, "..", vp, sizeof(vp));
        snprintf(s->cwd, sizeof(s->cwd), "%s", vp);
        ftp_reply(s, "250 Directory changed.");
    } else if (strcmp(line, "TYPE") == 0) {
        if (arg[0] == 'I' || arg[0] == 'i') {
            s->type_binary = true;
            ftp_reply(s, "200 Type set to I.");
        } else if (arg[0] == 'A' || arg[0] == 'a') {
            s->type_binary = false;
            ftp_reply(s, "200 Type set to A.");
        } else {
            ftp_reply(s, "504 Type not supported.");
        }
    } else if (strcmp(line, "PASV") == 0) {
        if (ftp_open_pasv(s) != 0) {
            ftp_reply(s, "425 Can't open passive connection.");
        }
        /* siker esetén a 227-et az ftp_open_pasv küldi */
    } else if (strcmp(line, "LIST") == 0 || strcmp(line, "NLST") == 0) {
        /* Az argumentum (ha van) a listázandó könyvtár, különben a CWD.
         * Egyszerűsítés: az opciókat (pl. "-l") ignoráljuk. */
        const char *target = (arg[0] == '-' || arg[0] == '\0') ? NULL : arg;
        char vp[FTP_PATH_MAX], fp[FTP_PATH_MAX * 2];
        ftp_resolve(s->cwd, target ? target : "", vp, sizeof(vp));
        ftp_to_fs(vp, fp, sizeof(fp));

        ftp_reply(s, "150 Opening data connection for directory list.");
        int ds = ftp_accept_data(s);
        if (ds < 0) {
            ftp_reply(s, "425 Can't open data connection.");
        } else {
            ftp_do_list(ds, fp, (strcmp(line, "LIST") == 0));
            close(ds);
            ftp_reply(s, "226 Transfer complete.");
        }
    } else if (strcmp(line, "RETR") == 0) {
        char vp[FTP_PATH_MAX], fp[FTP_PATH_MAX * 2];
        ftp_resolve(s->cwd, arg, vp, sizeof(vp));
        ftp_to_fs(vp, fp, sizeof(fp));

        ftp_reply(s, "150 Opening data connection.");
        int ds = ftp_accept_data(s);
        if (ds < 0) {
            ftp_reply(s, "425 Can't open data connection.");
        } else {
            int rc = ftp_do_retr(ds, fp);
            close(ds);
            ftp_reply(s, rc == 0 ? "226 Transfer complete."
                                 : "550 Failed to open file.");
        }
    } else if (strcmp(line, "STOR") == 0) {
        char vp[FTP_PATH_MAX], fp[FTP_PATH_MAX * 2];
        ftp_resolve(s->cwd, arg, vp, sizeof(vp));
        ftp_to_fs(vp, fp, sizeof(fp));

        ftp_reply(s, "150 Opening data connection.");
        int ds = ftp_accept_data(s);
        if (ds < 0) {
            ftp_reply(s, "425 Can't open data connection.");
        } else {
            int rc = ftp_do_stor(ds, fp);
            close(ds);
            ftp_reply(s, rc == 0 ? "226 Transfer complete."
                                 : "550 Failed to store file.");
        }
    } else if (strcmp(line, "DELE") == 0) {
        char vp[FTP_PATH_MAX], fp[FTP_PATH_MAX * 2];
        ftp_resolve(s->cwd, arg, vp, sizeof(vp));
        ftp_to_fs(vp, fp, sizeof(fp));
        int rc;
        storage_lfs_lock();
        rc = remove(fp);
        storage_lfs_unlock();
        ftp_reply(s, rc == 0 ? "250 File deleted." : "550 Delete failed.");
    } else if (strcmp(line, "SIZE") == 0) {
        char vp[FTP_PATH_MAX], fp[FTP_PATH_MAX * 2];
        ftp_resolve(s->cwd, arg, vp, sizeof(vp));
        ftp_to_fs(vp, fp, sizeof(fp));
        struct stat st;
        int rc;
        storage_lfs_lock();
        rc = stat(fp, &st);
        storage_lfs_unlock();
        if (rc == 0 && S_ISREG(st.st_mode)) {
            ftp_replyf(s, "213 %ld", (long)st.st_size);
        } else {
            ftp_reply(s, "550 Could not get file size.");
        }
    } else if (strcmp(line, "NOOP") == 0) {
        ftp_reply(s, "200 OK.");
    } else if (strcmp(line, "QUIT") == 0) {
        ftp_reply(s, "221 Goodbye.");
        return 1;
    } else {
        ftp_reply(s, "502 Command not implemented.");
    }
    return 0;
}

/* Egy session taszk: a control kapcsolat teljes életciklusa. */
static void ftp_session_task(void *param)
{
    ftp_sess_t *s = (ftp_sess_t *)param;

    ftp_reply(s, "220 ESP32-S3 SWD FTP ready.");

    char line[FTP_CMD_BUF];
    for (;;) {
        int n = ftp_read_line(s, line, sizeof(line));
        if (n < 0) break;            /* kapcsolat zárva / hiba */
        if (n == 0) continue;        /* üres sor — átugorjuk */
        if (ftp_handle_cmd(s, line) == 1) break;   /* QUIT */
    }

    /* Takarítás. */
    if (s->pasv_listen >= 0) close(s->pasv_listen);
    if (s->ctrl_sock >= 0) close(s->ctrl_sock);
    free(s);

    xSemaphoreTake(s_sess_mtx, portMAX_DELAY);
    s_active_sessions--;
    xSemaphoreGive(s_sess_mtx);

    ESP_LOGI(TAG, "session vege (aktiv=%d)", s_active_sessions);
    vTaskDelete(NULL);
}

/* ============================ Control listener ============================ */

static void ftp_listener_task(void *param)
{
    (void)param;

    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) {
        ESP_LOGE(TAG, "control socket() hiba: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(FTP_CTRL_PORT);

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "control bind() hiba: %d", errno);
        close(ls);
        vTaskDelete(NULL);
        return;
    }
    if (listen(ls, FTP_MAX_SESSIONS) != 0) {
        ESP_LOGE(TAG, "control listen() hiba: %d", errno);
        close(ls);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "FTP listener fut, port %d", FTP_CTRL_PORT);

    for (;;) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) {
            ESP_LOGW(TAG, "accept() hiba: %d", errno);
            continue;
        }

        /* Kliens-limit ellenőrzése. */
        bool allow;
        xSemaphoreTake(s_sess_mtx, portMAX_DELAY);
        allow = (s_active_sessions < FTP_MAX_SESSIONS);
        if (allow) s_active_sessions++;
        xSemaphoreGive(s_sess_mtx);

        if (!allow) {
            const char *busy = "421 Too many connections.\r\n";
            send(cs, busy, strlen(busy), 0);
            close(cs);
            ESP_LOGW(TAG, "session elutasitva (limit)");
            continue;
        }

        ftp_sess_t *s = calloc(1, sizeof(ftp_sess_t));
        if (!s) {
            const char *err = "421 Out of memory.\r\n";
            send(cs, err, strlen(err), 0);
            close(cs);
            xSemaphoreTake(s_sess_mtx, portMAX_DELAY);
            s_active_sessions--;
            xSemaphoreGive(s_sess_mtx);
            continue;
        }
        s->ctrl_sock = cs;
        s->pasv_listen = -1;
        s->type_binary = true;       /* alapból bináris */
        strcpy(s->cwd, "/");         /* a /lfs gyökér */

        if (xTaskCreate(ftp_session_task, "ftp_sess", FTP_CTRL_STACK,
                        s, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "session taszk inditas hiba");
            close(cs);
            free(s);
            xSemaphoreTake(s_sess_mtx, portMAX_DELAY);
            s_active_sessions--;
            xSemaphoreGive(s_sess_mtx);
        }
    }

    /* nem éri el */
    close(ls);
    vTaskDelete(NULL);
}

/* ============================ Publikus API ============================ */

esp_err_t ftp_srv_init(void)
{
    if (s_sess_mtx == NULL) {
        s_sess_mtx = xSemaphoreCreateMutex();
        if (s_sess_mtx == NULL) {
            ESP_LOGE(TAG, "mutex letrehozas hiba");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(ftp_listener_task, "ftp_listen", 4096,
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "listener taszk inditas hiba");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ftp_srv_init kesz");
    return ESP_OK;
}

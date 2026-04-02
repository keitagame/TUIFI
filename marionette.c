/*
 * marionette.c — Firefox Marionette WebDriver protocol client
 *
 * Marionette is Firefox's built-in remote control protocol, accessible via
 * TCP on port 2828 by default when Firefox is launched with:
 *   firefox --marionette
 *
 * Protocol: line-framed JSON messages
 *   Client → Server: [0, id, command, params]
 *   Server → Client: [1, id, error, result]
 *
 * Reference: https://firefox-source-docs.mozilla.org/testing/marionette/
 */

#include "foxterm.h"
#include <sys/time.h>

#define RECV_BUF_SIZE  (1024 * 1024)  /* 1 MB */

/* ────────────────────────────────────────────────────────────────
 * Low-level TCP helpers
 * ──────────────────────────────────────────────────────────────── */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints = {0}, *res = NULL;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* Set connection timeout */
    struct timeval tv = { .tv_sec = MARIONETTE_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* Read exactly `n` bytes */
static bool recv_exact(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* Read until ':' and parse the length prefix (netstring protocol) */
static char *recv_message(int fd)
{
    /* Marionette uses netstring framing: "<len>:<json>," */
    char lenbuf[32];
    size_t li = 0;

    while (li < sizeof(lenbuf) - 1) {
        char c;
        if (recv(fd, &c, 1, 0) <= 0) return NULL;
        if (c == ':') break;
        if (!isdigit((unsigned char)c)) return NULL;
        lenbuf[li++] = c;
    }
    lenbuf[li] = '\0';

    long msglen = atol(lenbuf);
    if (msglen <= 0 || msglen > 10 * 1024 * 1024) return NULL;

    char *buf = malloc(msglen + 2);
    if (!buf) return NULL;

    if (!recv_exact(fd, buf, (size_t)msglen)) {
        free(buf);
        return NULL;
    }
    buf[msglen] = '\0';

    /* Consume trailing comma */
    char comma;
    recv(fd, &comma, 1, 0);

    return buf;
}

/* Send a netstring-framed message */
static bool send_message(int fd, const char *msg)
{
    size_t len = strlen(msg);
    char header[32];
    int hlen = snprintf(header, sizeof(header), "%zu:", len);

    if (send(fd, header, (size_t)hlen, 0) < 0) return false;
    if (send(fd, msg,    len,           0) < 0) return false;
    if (send(fd, ",",    1,             0) < 0) return false;
    return true;
}

/* ────────────────────────────────────────────────────────────────
 * Marionette command / response
 * ──────────────────────────────────────────────────────────────── */

static char *marionette_send_cmd(MarionetteConn *m,
                                  const char *cmd,
                                  json_object *params)
{
    if (!m->connected || m->fd < 0) return NULL;

    /* Build: [0, id, cmd, params] */
    json_object *req = json_object_new_array();
    json_object_array_add(req, json_object_new_int(0));
    json_object_array_add(req, json_object_new_int(++m->msg_id));
    json_object_array_add(req, json_object_new_string(cmd));
    json_object_array_add(req, params ? params : json_object_new_object());

    const char *msg = json_object_to_json_string(req);
    bool ok = send_message(m->fd, msg);
    json_object_put(req);

    if (!ok) return NULL;

    /* Receive response */
    char *resp = recv_message(m->fd);
    return resp;  /* caller must free */
}

/* Extract result field from response [1, id, error, result] */
static json_object *parse_response(const char *resp_str, bool *err_out)
{
    if (!resp_str) { if (err_out) *err_out = true; return NULL; }

    json_object *arr = json_tokener_parse(resp_str);
    if (!arr || !json_object_is_type(arr, json_type_array)) {
        if (arr) json_object_put(arr);
        if (err_out) *err_out = true;
        return NULL;
    }

    /* arr[2] is error (null if success), arr[3] is result */
    json_object *errobj = json_object_array_get_idx(arr, 2);
    if (errobj && !json_object_is_type(errobj, json_type_null)) {
        if (err_out) *err_out = true;
        json_object_put(arr);
        return NULL;
    }

    json_object *result = json_object_array_get_idx(arr, 3);
    /* Increment refcount so we can free the parent array */
    if (result) json_object_get(result);
    json_object_put(arr);

    if (err_out) *err_out = false;
    return result;
}

/* ────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────── */

bool marionette_connect(MarionetteConn *m, int port)
{
    m->fd        = -1;
    m->connected = false;
    m->msg_id    = 0;
    m->port      = port;

    int fd = tcp_connect("127.0.0.1", port);
    if (fd < 0) return false;

    /* Marionette sends a greeting first */
    char *greeting = recv_message(fd);
    if (!greeting) {
        close(fd);
        return false;
    }
    free(greeting);

    m->fd        = fd;
    m->connected = true;

    /* Send WebDriver:NewSession to start a session */
    json_object *caps = json_object_new_object();
    json_object *desired = json_object_new_object();
    json_object_object_add(caps, "capabilities", desired);

    char *resp = marionette_send_cmd(m, "WebDriver:NewSession", caps);
    bool err = false;
    json_object *result = parse_response(resp, &err);
    free(resp);
    if (result) json_object_put(result);

    if (err) {
        /* Try older Marionette session command */
        char *resp2 = marionette_send_cmd(m, "newSession", json_object_new_object());
        json_object *r2 = parse_response(resp2, &err);
        free(resp2);
        if (r2) json_object_put(r2);
    }

    return !err;
}

void marionette_disconnect(MarionetteConn *m)
{
    if (m->fd >= 0) {
        /* Try clean quit */
        char *resp = marionette_send_cmd(m, "Marionette:Quit", NULL);
        free(resp);
        close(m->fd);
        m->fd = -1;
    }
    m->connected = false;
}

bool marionette_navigate(MarionetteConn *m, const char *url)
{
    json_object *params = json_object_new_object();
    json_object_object_add(params, "url", json_object_new_string(url));

    char *resp = marionette_send_cmd(m, "WebDriver:Navigate", params);
    bool err = false;
    json_object *r = parse_response(resp, &err);
    free(resp);
    if (r) json_object_put(r);
    return !err;
}

char *marionette_get_title(MarionetteConn *m)
{
    char *resp = marionette_send_cmd(m, "WebDriver:GetTitle", NULL);
    bool err = false;
    json_object *r = parse_response(resp, &err);
    free(resp);

    if (!r || err) return NULL;

    char *title = NULL;
    json_object *val = NULL;
    if (json_object_object_get_ex(r, "value", &val)) {
        const char *s = json_object_get_string(val);
        if (s) title = strdup(s);
    }
    json_object_put(r);
    return title;
}

char *marionette_get_page_source(MarionetteConn *m)
{
    char *resp = marionette_send_cmd(m, "WebDriver:GetPageSource", NULL);
    bool err = false;
    json_object *r = parse_response(resp, &err);
    free(resp);

    if (!r || err) return NULL;

    char *src = NULL;
    json_object *val = NULL;
    if (json_object_object_get_ex(r, "value", &val)) {
        const char *s = json_object_get_string(val);
        if (s) src = strdup(s);
    }
    json_object_put(r);
    return src;
}

char *marionette_eval(MarionetteConn *m, const char *script)
{
    json_object *params = json_object_new_object();
    json_object_object_add(params, "script", json_object_new_string(script));
    json_object_object_add(params, "args",   json_object_new_array());

    char *resp = marionette_send_cmd(m, "WebDriver:ExecuteScript", params);
    bool err = false;
    json_object *r = parse_response(resp, &err);
    free(resp);

    if (!r || err) return NULL;

    char *result = NULL;
    json_object *val = NULL;
    if (json_object_object_get_ex(r, "value", &val)) {
        const char *s = json_object_to_json_string(val);
        if (s) result = strdup(s);
    }
    json_object_put(r);
    return result;
}

/* ── Try to find a running Firefox with Marionette enabled ── */
bool marionette_find_firefox(int *port_out)
{
    /* Check default port */
    int fd = tcp_connect("127.0.0.1", MARIONETTE_DEFAULT_PORT);
    if (fd >= 0) {
        close(fd);
        if (port_out) *port_out = MARIONETTE_DEFAULT_PORT;
        return true;
    }

    /* Check common alternative ports */
    int ports[] = { 2829, 2830, 4444, 9222, 0 };
    for (int i = 0; ports[i]; i++) {
        fd = tcp_connect("127.0.0.1", ports[i]);
        if (fd >= 0) {
            close(fd);
            if (port_out) *port_out = ports[i];
            return true;
        }
    }

    return false;
}
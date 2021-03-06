/*
 * Copyright (C) 2017 Jianhui Zhao <jianhuizhao329@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <glob.h>
#include <arpa/inet.h>
#include <libubox/usock.h>
#include <libubox/utils.h>

#include "uwsc.h"
#include "log.h"
#include "utils.h"

static void uwsc_free(struct uwsc_client *cl)
{
    uloop_timeout_cancel(&cl->timeout);
    ustream_free(&cl->sfd.stream);
    shutdown(cl->sfd.fd.fd, SHUT_RDWR);
    close(cl->sfd.fd.fd);
#if (UWSC_SSL_SUPPORT)
    if (cl->ssl_ops && cl->ssl_ctx)
        cl->ssl_ops->context_free(cl->ssl_ctx);
#endif
    free(cl);
}

static inline void uwsc_error(struct uwsc_client *cl, int error)
{
    cl->us->eof = true;
    cl->error = error;
    ustream_state_change(cl->us);
}

static void dispach_message(struct uwsc_client *cl)
{
    struct uwsc_frame *frame = &cl->frame;

    switch (frame->opcode) {
    case WEBSOCKET_OP_TEXT:
        if (cl->onmessage)
            cl->onmessage(cl, frame->payload, frame->payload_len, frame->opcode);
        break;
    case WEBSOCKET_OP_PING:
        cl->send(cl, frame->payload, frame->payload_len, WEBSOCKET_OP_PONG);
        break;
    case WEBSOCKET_OP_CLOSE:
        if (cl->onclose)
            cl->onclose(cl);
    default:
        break;
    }
}

static void parse_frame(struct uwsc_client *cl, char *data)
{
    struct uwsc_frame *frame = &cl->frame;
    uint8_t fin, opcode;
    uint64_t payload_len;
    char *payload;
    
    fin = (data[0] & 0x80) ? 1 : 0;
    opcode = data[0] & 0x0F;

    if (data[1] & 0x80) {
        uwsc_log_err("Masked error");
        cl->send(cl, NULL, 0, WEBSOCKET_OP_CLOSE);
        return;
    }

    payload_len = data[1] & 0x7F;
    payload = data + 2;

    switch (payload_len) {
    case 126:
        payload_len = ntohs(*(uint16_t *)&data[2]);
        payload += 2;
        break;
    case 127:
        payload_len = (((uint64_t)ntohl(*(uint32_t *)&data[2])) << 32) + ntohl(*(uint32_t *)&data[6]);
        payload += 8;
        break;
    default:
        break;
    }

    if (frame->fragmented) {
        int new_len = frame->payload_len + payload_len;

        if (fin && opcode == WEBSOCKET_OP_TEXT)
            new_len += 1;
        
        frame->payload = realloc(frame->payload, new_len);
        if (!frame->payload) {
            uwsc_log_err("No mem");
            cl->send(cl, NULL, 0, WEBSOCKET_OP_CLOSE);
            return;
        }

        memcpy(frame->payload + frame->payload_len, payload, payload_len);
        frame->payload[payload_len - 1] = 0;
        frame->payload_len = new_len;
    } else {
        frame->opcode = opcode;
        frame->payload_len = payload_len;
        frame->payload = payload;

        if (!fin) {
            frame->fragmented = true;
            frame->payload = malloc(payload_len);
            memcpy(frame->payload, payload, payload_len);
        }
    }

    if (fin) {
        dispach_message(cl);
        if (frame->fragmented) {
            frame->fragmented = false;
            free(frame->payload);
        }
    }
}

static int parse_header(struct uwsc_client *cl, char *data)
{
    char *k, *v;
    bool has_upgrade = false;
    bool has_connection = false;
    bool has_sec_webSocket_accept = false;

    while (1) {
        k = strtok(NULL, "\r\n");
        if (!k)
            break;
        
        v = strchr(k, ':');
        if (!v)
            break;

        *v++ = 0;

        while (*v == ' ')
            v++;

        if (!strcasecmp(k, "Upgrade") && !strcasecmp(v, "websocket"))
            has_upgrade = true;

        if (!strcasecmp(k, "Connection") && !strcasecmp(v, "upgrade"))
            has_connection = true;

        if (!strcasecmp(k, "Sec-WebSocket-Accept"))
            has_sec_webSocket_accept = true;

        /* TODO: verify the value of Sec-WebSocket-Accept */
    }

    if (!has_upgrade || !has_connection || !has_sec_webSocket_accept) {
        uwsc_log_err("Invalid header");
        return -1;
    }
    
    return 0;
}

static void uwsc_ping_cb(struct uloop_timeout *timeout)
{
    struct uwsc_client *cl = container_of(timeout, struct uwsc_client, timeout);

    if (cl->state > CLIENT_STATE_MESSAGE)
        return;

    cl->ping(cl);
    uloop_timeout_set(&cl->timeout, UWSC_PING_INTERVAL * 1000);
}

static void __uwsc_notify_read(struct uwsc_client *cl, struct ustream *s)
{
    char *data;
    int len;

    data = ustream_get_read_buf(s, &len);
    if (!data || !len)
        return;

    if (cl->state == CLIENT_STATE_HANDSHAKE) {
        char *p, *version, *status_code, *summary;

        p = strstr(data, "\r\n\r\n");
        if (!p)
            return;
        
        p[2] = 0;

        version = strtok(data, " ");
        status_code = strtok(NULL, " ");
        summary = strtok(NULL, "\r\n");

        if (!version || strcmp(version, "HTTP/1.1")) {
            uwsc_log_err("Invalid version");
            goto err;
        }

        if (!status_code || atoi(status_code) != 101) {
            uwsc_log_err("Invalid status code");
            goto err;
        }

        if (!summary) {
            uwsc_log_err("Invalid summary");
            goto err;
        }

        if (parse_header(cl, data))
            goto err;

        ustream_consume(cl->us, p + 4 - data);

        if (cl->onopen)
            cl->onopen(cl);

        cl->timeout.cb = uwsc_ping_cb;
        uloop_timeout_set(&cl->timeout, UWSC_PING_INTERVAL * 1000);
        
        cl->state = CLIENT_STATE_MESSAGE;
    } else if (cl->state == CLIENT_STATE_MESSAGE) {
        parse_frame(cl, data);
        ustream_consume(cl->us, len);
    }
    return;

err:
    uwsc_error(cl, UWSC_ERROR_INVALID_HEADER);
}

static inline void uwsc_notify_read(struct ustream *s, int bytes)
{
    struct uwsc_client *cl = container_of(s, struct uwsc_client, sfd.stream);
    __uwsc_notify_read(cl, s);
}

static void __uwsc_notify_state(struct uwsc_client *cl, struct ustream *s)
{

    if (!cl->error && s->write_error)
        cl->error = UWSC_ERROR_WRITE;

    if (!cl->error) {
        if (!s->eof || s->w.data_bytes)
            return;
    }

    if (cl->error && cl->onerror)
        cl->onerror(cl);

    if (cl->onclose)
        cl->onclose(cl);
}

static inline void uwsc_notify_state(struct ustream *s)
{
    struct uwsc_client *cl = container_of(s, struct uwsc_client, sfd.stream);
    __uwsc_notify_state(cl, s);
}

#if (UWSC_SSL_SUPPORT)
static inline void uwsc_ssl_notify_read(struct ustream *s, int bytes)
{
    struct uwsc_client *cl = container_of(s, struct uwsc_client, ussl.stream);
    __uwsc_notify_read(cl, s);
}

static inline void uwsc_ssl_notify_state(struct ustream *s)
{
    struct uwsc_client *cl = container_of(s, struct uwsc_client, ussl.stream);
    __uwsc_notify_state(cl, s);
}

static void uwsc_ssl_notify_error(struct ustream_ssl *ssl, int error, const char *str)
{
    struct uwsc_client *cl = container_of(ssl, struct uwsc_client, ussl);

    uwsc_error(cl, UWSC_ERROR_SSL);
    uwsc_log_err("ssl error:%d:%s", error, str);
}

static void uwsc_ssl_notify_verify_error(struct ustream_ssl *ssl, int error, const char *str)
{
    struct uwsc_client *cl = container_of(ssl, struct uwsc_client, ussl);

    if (!cl->ssl_require_validation)
        return;

    uwsc_error(cl, UWSC_ERROR_SSL_INVALID_CERT);
    uwsc_log_err("ssl error:%d:%s", error, str);
}

static void uwsc_ssl_notify_connected(struct ustream_ssl *ssl)
{
    struct uwsc_client *cl = container_of(ssl, struct uwsc_client, ussl);

    if (!cl->ssl_require_validation)
        return;

    if (!cl->ussl.valid_cn) {
        uwsc_error(cl, UWSC_ERROR_SSL_CN_MISMATCH);
        uwsc_log_err("ssl error: cn mismatch");
    }
}

#endif

static int uwsc_send(struct uwsc_client *cl, char *data, int len, enum websocket_op op)
{
    char *buf, *p;
    uint8_t mask_key[4];
    int i;
    int frame_size = 0;

    if (len > INT_MAX - 8) {
        uwsc_log_err("Payload too big");
        return -1;
    }

    buf = malloc(len + 8);
    if (!buf) {
        uwsc_log_err("NO mem");
        return -1;
    }

    p = buf;

    get_nonce(mask_key, 4);

    *p++ = 0x80 | op;   /* FIN and opcode */

    if (len < 126) {
        *p++ = 0x80 | len;
        frame_size = 6 + len;
    } else if (len < 0x10000) {
        *p++ = 0x80 | 126;
        *p++ = (len >> 8) & 0xFF;
        *p++ = len & 0xFF;
        frame_size = 8 + len;
    } else {
        *p++ = 0x80 | 127;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        *p++ = (len >> 24) & 0xFF;
        *p++ = (len >> 16) & 0xFF;
        *p++ = (len >> 8) & 0xFF;
        *p++ = len & 0xFF;
        frame_size = 14 + len;
    }

    memcpy(p, mask_key, 4);
    p += 4;
    memcpy(p, data, len);
    for (i = 0; i < len; i++) {
        p[i] ^= mask_key[i % 4];
    }

    ustream_write(cl->us, buf, frame_size, false);

    free(buf);

    return 0;
}

static inline void uwsc_ping(struct uwsc_client *cl)
{
    cl->send(cl, NULL, 0, WEBSOCKET_OP_PING);
}

static void uwsc_handshake(struct uwsc_client *cl, const char *host, int port, const char *path)
{
    uint8_t nonce[16];
    char websocket_key[256] = "";

    get_nonce(nonce, sizeof(nonce));
    
    b64_encode(nonce, sizeof(nonce), websocket_key, sizeof(websocket_key));

    ustream_printf(cl->us, "GET %s HTTP/1.1\r\n", path);
    ustream_printf(cl->us, "Upgrade: websocket\r\n");
    ustream_printf(cl->us, "Connection: Upgrade\r\n");
    ustream_printf(cl->us, "Sec-WebSocket-Key: %s\r\n", websocket_key);
    ustream_printf(cl->us, "Sec-WebSocket-Version: 13\r\n");

    ustream_printf(cl->us, "Host: %s", host);
    if (port == 80)
        ustream_printf(cl->us, "\r\n");
    else
        ustream_printf(cl->us, ":%d\r\n", port);
    
    ustream_printf(cl->us, "\r\n");

    cl->state = CLIENT_STATE_HANDSHAKE;
}

struct uwsc_client *uwsc_new_ssl(const char *url, const char *ca_crt_file, bool verify)
{
    struct uwsc_client *cl = NULL;
    char *host = NULL;
    const char *path = "/";
    int port;
    int sock;
    bool ssl;

    if (parse_url(url, &host, &port, &path, &ssl) < 0) {
        uwsc_log_err("Invalid url");
        return NULL;
    }

    sock = usock(USOCK_TCP | USOCK_NOCLOEXEC, host, usock_port(port));
    if (sock < 0) {
        uwsc_log_err("usock");
        goto err;
    }

    cl = calloc(1, sizeof(struct uwsc_client));
    if (!cl) {
        uwsc_log_err("calloc");
        goto err;
    }

    cl->free = uwsc_free;
    cl->send = uwsc_send;
    cl->ping = uwsc_ping;

    ustream_fd_init(&cl->sfd, sock);

    if (ssl) {
#if (UWSC_SSL_SUPPORT)
        cl->ssl_ops = init_ustream_ssl();
        if (!cl->ssl_ops) {
            uwsc_log_err("SSL support not available,please install one of the libustream-ssl-* libraries");
            goto err;
        }

        cl->ssl_ctx = cl->ssl_ops->context_new(false);
        if (!cl->ssl_ctx) {
            uwsc_log_err("ustream_ssl_context_new");
            goto err;
        }

        if (ca_crt_file) {
            if (cl->ssl_ops->context_add_ca_crt_file(cl->ssl_ctx, ca_crt_file)) {
                uwsc_log_err("Load CA certificates failed");
                goto err;
            }
        } else if (verify) {
            int i;
            glob_t gl;

            cl->ssl_require_validation = true;

            if (!glob("/etc/ssl/certs/*.crt", 0, NULL, &gl)) {
                for (i = 0; i < gl.gl_pathc; i++)
                    cl->ssl_ops->context_add_ca_crt_file(cl->ssl_ctx, gl.gl_pathv[i]);
                globfree(&gl);
            }
        }

        cl->us = &cl->ussl.stream;
        cl->us->string_data = true;
        cl->us->notify_read = uwsc_ssl_notify_read;
        cl->us->notify_state = uwsc_ssl_notify_state;
        cl->ussl.notify_error = uwsc_ssl_notify_error;
        cl->ussl.notify_verify_error = uwsc_ssl_notify_verify_error;
        cl->ussl.notify_connected = uwsc_ssl_notify_connected;
        cl->ussl.server_name = host;
        cl->ssl_ops->init(&cl->ussl, &cl->sfd.stream, cl->ssl_ctx, false);
        cl->ssl_ops->set_peer_cn(&cl->ussl, host);
#else
        uwsc_log_err("SSL support not available");
        return NULL;
#endif
    } else {
        cl->us = &cl->sfd.stream;
        cl->us->string_data = true;
        cl->us->notify_read = uwsc_notify_read;
        cl->us->notify_state = uwsc_notify_state;
    }

    uwsc_handshake(cl, host, port, path);
    free(host);
    
    return cl;

err:
    if (host)
        free(host);

    if (cl)
        cl->free(cl);

    return NULL;    
}
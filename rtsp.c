/*
 * RTSP protocol handler. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <memory.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <signal.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <openssl/md5.h>

#include "common.h"
#include "player.h"
#include "rtp.h"
#include "mdns.h"

// only one thread is allowed to use the player at once.
// it monitors the request variable (at least when interrupted)
static pthread_mutex_t playing_mutex = PTHREAD_MUTEX_INITIALIZER;
static int please_shutdown = 0;
static pthread_t playing_thread = 0;

typedef struct {
    stream_cfg stream;
    SOCKADDR remote;
} rtsp_conn_info;

static inline int rtsp_playing(void) {
    return playing_thread == pthread_self();
}

static void rtsp_take_player(void) {
    if (pthread_mutex_trylock(&playing_mutex)) {
        debug("shutting down playing thread\n");
        // XXX minor race condition between please_shutdown and signal delivery
        please_shutdown = 1;
        pthread_kill(playing_thread, SIGUSR1);
        pthread_mutex_lock(&playing_mutex);
    }
    playing_thread = pthread_self();
}

void rtsp_shutdown_stream(void) {
    rtsp_take_player();
    pthread_mutex_unlock(&playing_mutex);
}


// park a null at the line ending, and return the next line pointer
// accept \r, \n, or \r\n
static char *nextline(char *in, int inbuf) {
    char *out = NULL;
    while (inbuf) {
        if (*in == '\r') {
            *in++ = 0;
            out = in;
        }
        if (*in == '\n') {
            *in++ = 0;
            out = in;
        }

        if (out)
            break;

        in++;
        inbuf--;
    }
    return out;
}

typedef struct {
    int nheaders;
    char *name[16];
    char *value[16];

    int contentlength;
    char *content;

    // for requests
    char method[16];

    // for responses
    int respcode;
} rtsp_message;

static rtsp_message * msg_init(void) {
    rtsp_message *msg = malloc(sizeof(rtsp_message));
    memset(msg, 0, sizeof(rtsp_message)); 
    return msg;
}

static int msg_add_header(rtsp_message *msg, char *name, char *value) {
    if (msg->nheaders >= sizeof(msg->name)/sizeof(char*)) {
        warn("too many headers?!\n");
        return 1;
    }

    msg->name[msg->nheaders] = strdup(name);
    msg->value[msg->nheaders] = strdup(value);
    msg->nheaders++;

    return 0;
}

static char *msg_get_header(rtsp_message *msg, char *name) {
    int i;
    for (i=0; i<msg->nheaders; i++)
        if (!strcasecmp(msg->name[i], name))
            return msg->value[i];
    return NULL;
}

static void msg_free(rtsp_message *msg) {
    int i;
    for (i=0; i<msg->nheaders; i++) {
        free(msg->name[i]);
        free(msg->value[i]);
    }
    if (msg->content)
        free(msg->content);
    free(msg);
}


static int msg_handle_line(rtsp_message **pmsg, char *line) {
    rtsp_message *msg = *pmsg;

    if (!msg) {
        msg = msg_init();
        *pmsg = msg;
        char *sp, *p;

        debug("received request: %s\n", line);

        p = strtok_r(line, " ", &sp);
        if (!p)
            goto fail;
        strncpy(msg->method, p, sizeof(msg->method)-1);

        p = strtok_r(NULL, " ", &sp);
        if (!p)
            goto fail;
        
        p = strtok_r(NULL, " ", &sp);
        if (!p)
            goto fail;
        if (strcmp(p, "RTSP/1.0"))
            goto fail;

        return -1;
    }

    if (strlen(line)) {
        char *p;
        p = strstr(line, ": ");
        if (!p) {
            warn("bad header: >>%s<<\n", line);
            goto fail;
        }
        *p = 0;
        p += 2;
        msg_add_header(msg, line, p);
        debug("    %s: %s\n", line, p);
        return -1;
    } else {
        char *cl = msg_get_header(msg, "Content-Length");
        if (cl)
            return atoi(cl);
        else
            return 0;
    }

fail:
    *pmsg = NULL;
    msg_free(msg);
    return 0;
}

static rtsp_message * rtsp_read_request(int fd) {
    ssize_t buflen = 512;
    char *buf = malloc(buflen+1);

    rtsp_message *msg = NULL;

    ssize_t nread; 
    ssize_t inbuf = 0;
    int msg_size = -1;

    while (msg_size < 0) {
        if (please_shutdown) {
            debug("RTSP shutdown requested\n");
            goto shutdown;
        }
        nread = read(fd, buf+inbuf, buflen - inbuf);
        if (!nread) {
            debug("RTSP connection closed\n");
            goto shutdown;
        }
        if (nread < 0) {
            if (errno==EINTR)
                continue;
            perror("read failure");
            goto shutdown;
        }
        inbuf += nread;

        char *next;
        while (msg_size < 0 && (next = nextline(buf, inbuf))) {
            msg_size = msg_handle_line(&msg, buf);

            if (!msg) {
                warn("no RTSP header received\n");
                goto shutdown;
            }

            inbuf -= next-buf;
            if (inbuf)
                memmove(buf, next, inbuf);
        }
    }

    if (msg_size > buflen) {
        buf = realloc(buf, msg_size);
        if (!buf) {
            warn("too much content");
            goto shutdown;
        }
        buflen = msg_size;
    }

    while (inbuf < msg_size) {
        nread = read(fd, buf+inbuf, msg_size-inbuf);
        if (!nread)
            goto shutdown;
        if (nread==EINTR)
            continue;
        if (nread < 0) {
            perror("read failure");
            goto shutdown;
        }
        inbuf += nread;
    }

    msg->contentlength = inbuf;
    msg->content = buf;
    return msg;

shutdown:
    free(buf);
    if (msg) {
        msg_free(msg);
    }
    return NULL;
}

static void msg_write_response(int fd, rtsp_message *resp) {
    char rbuf[30];
    int nrbuf;
    nrbuf = snprintf(rbuf, sizeof(rbuf),
                     "RTSP/1.0 %d %s\r\n", resp->respcode,
                     resp->respcode==200 ? "OK" : "Error");
    write(fd, rbuf, nrbuf);
    debug("sending response: %s", rbuf);
    int i;
    for (i=0; i<resp->nheaders; i++) {
        debug("    %s: %s\n", resp->name[i], resp->value[i]);
        write(fd, resp->name[i], strlen(resp->name[i]));
        write(fd, ": ", 2);
        write(fd, resp->value[i], strlen(resp->value[i]));
        write(fd, "\r\n", 2);
    }
    write(fd, "\r\n", 2);
}

static void handle_options(rtsp_conn_info *conn,
                           rtsp_message *req, rtsp_message *resp) {
    resp->respcode = 200;
    msg_add_header(resp, "Public",
                   "ANNOUNCE, SETUP, RECORD, "
                   "PAUSE, FLUSH, TEARDOWN, "
                   "OPTIONS, GET_PARAMETER, SET_PARAMETER");
}

static void handle_teardown(rtsp_conn_info *conn,
                            rtsp_message *req, rtsp_message *resp) {
    if (!rtsp_playing())
        return;
    resp->respcode = 200;
    msg_add_header(resp, "Connection", "close");
    please_shutdown = 1;
}

static void handle_flush(rtsp_conn_info *conn,
                         rtsp_message *req, rtsp_message *resp) {
    if (!rtsp_playing())
        return;
    player_flush();
    resp->respcode = 200;
}

static void handle_setup(rtsp_conn_info *conn,
                         rtsp_message *req, rtsp_message *resp) {
    int cport, tport;
    char *hdr = msg_get_header(req, "Transport");
    if (!hdr)
        return;

    char *p;
    p = strstr(hdr, "control_port=");
    if (!p)
        return;
    p = strchr(p, '=') + 1;
    cport = atoi(p);

    p = strstr(hdr, "timing_port=");
    if (!p)
        return;
    p = strchr(p, '=') + 1;
    tport = atoi(p);

    rtsp_take_player();
    int sport = rtp_setup(&conn->remote, cport, tport);
    if (!sport)
        return;

    player_play(&conn->stream);

    char *resphdr = malloc(strlen(hdr) + 20);
    strcpy(resphdr, hdr);
    sprintf(resphdr + strlen(resphdr), ";server_port=%d", sport);
    msg_add_header(resp, "Transport", resphdr);

    resp->respcode = 200;
}

static void handle_ignore(rtsp_conn_info *conn,
                          rtsp_message *req, rtsp_message *resp) {
    resp->respcode = 200;
}

static void handle_announce(rtsp_conn_info *conn,
                            rtsp_message *req, rtsp_message *resp) {

    char *paesiv = NULL;
    char *prsaaeskey = NULL;
    char *pfmtp = NULL;
    char *cp = req->content;
    int cp_left = req->contentlength;
    char *next;
    while (cp) {
        next = nextline(cp, cp_left);
        cp_left -= next-cp;

        if (!strncmp(cp, "a=fmtp:", 7))
            pfmtp = cp+7;

        if (!strncmp(cp, "a=aesiv:", 8))
            paesiv = cp+8;

        if (!strncmp(cp, "a=rsaaeskey:", 12))
            prsaaeskey = cp+12;
        
        cp = next;
    }

    if (!paesiv || !prsaaeskey || !pfmtp) {
        warn("required params missing from announce\n");
        return;
    }

    int len, keylen;
    uint8_t *aesiv = base64_dec(paesiv, &len);
    if (len != 16) {
        warn("client announced aeskey of %d bytes, wanted 16\n", len);
        free(aesiv);
        return;
    }
    memcpy(conn->stream.aesiv, aesiv, 16);
    free(aesiv);

    uint8_t *rsaaeskey = base64_dec(prsaaeskey, &len);
    uint8_t *aeskey = rsa_apply(rsaaeskey, len, &keylen, RSA_MODE_KEY);
    free(rsaaeskey);
    if (keylen != 16) {
        warn("client announced rsaaeskey of %d bytes, wanted 16\n", keylen);
        free(aeskey);
        return;
    }
    memcpy(conn->stream.aeskey, aeskey, 16);
    free(aeskey);
    
    int i;
    for (i=0; i<sizeof(conn->stream.fmtp)/sizeof(conn->stream.fmtp[0]); i++)
        conn->stream.fmtp[i] = atoi(strsep(&pfmtp, " \t"));

    resp->respcode = 200;
}


static struct method_handler {
    char *method;
    void (*handler)(rtsp_conn_info *conn, rtsp_message *req,
                    rtsp_message *resp);
} method_handlers[] = {
    {"OPTIONS",         handle_options},
    {"ANNOUNCE",        handle_announce},
    {"FLUSH",           handle_flush},
    {"TEARDOWN",        handle_teardown},
    {"SETUP",           handle_setup},
    {"GET_PARAMETER",   handle_ignore},
    {"SET_PARAMETER",   handle_ignore}, // XXX
    {"RECORD",          handle_ignore},
    {NULL,              NULL}
};

static void apple_challenge(int fd, rtsp_message *req, rtsp_message *resp) {
    char *hdr = msg_get_header(req, "Apple-Challenge");
    if (!hdr)
        return;

    SOCKADDR fdsa;
    socklen_t sa_len = sizeof(fdsa);
    getsockname(fd, (struct sockaddr*)&fdsa, &sa_len);

    int chall_len;
    uint8_t *chall = base64_dec(hdr, &chall_len);
    uint8_t buf[48], *bp = buf;
    int i;
    memset(buf, 0, sizeof(buf));

    if (chall_len > 16) {
        warn("oversized Apple-Challenge!\n");
        free(chall);
        return;
    }
    memcpy(bp, chall, chall_len);
    free(chall);
    bp += chall_len;
    
#ifdef AF_INET6
    if (fdsa.SAFAMILY == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)(&fdsa);
        memcpy(bp, sa6->sin6_addr.s6_addr, 16);
        bp += 16;
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)(&fdsa);
        unsigned int ip = sa->sin_addr.s_addr;
        for (i=0; i<4; i++) {
            *bp++ = ip & 0xff;
            ip >>= 8;
        }
    }

    for (i=0; i<6; i++)
        *bp++ = config.hw_addr[i];

    int buflen, resplen;
    buflen = bp-buf;
    if (buflen < 0x20)
        buflen = 0x20;

    uint8_t *challresp = rsa_apply(buf, buflen, &resplen, RSA_MODE_AUTH);
    char *encoded = base64_enc(challresp, resplen);

    // strip the padding.
    char *padding = strchr(encoded, '=');
    if (padding)
        *padding = 0;

    msg_add_header(resp, "Apple-Response", encoded);
    free(challresp);
    free(encoded);
}

static char *make_nonce(void) {
    uint8_t random[8];
    int fd = open("/dev/random", O_RDONLY);
    if (fd < 0)
        die("could not open /dev/random!");
    read(fd, random, sizeof(random));
    close(fd);
    return base64_enc(random, 8);
}

static int rtsp_auth(char **nonce, rtsp_message *req, rtsp_message *resp) {

    if (!config.password)
        return 0;
    if (!*nonce) {
        *nonce = make_nonce();
        goto authenticate;
    }

    char *hdr = msg_get_header(req, "Authorization");
    if (!hdr || strncmp(hdr, "Digest ", 7))
        goto authenticate;
    
    char *realm = strstr(hdr, "realm=\"");
    char *username = strstr(hdr, "username=\"");
    char *response = strstr(hdr, "response=\"");
    char *uri = strstr(hdr, "uri=\"");

    if (!realm || !username || !response || !uri)
        goto authenticate;

    char *quote;
    realm = strchr(realm, '"') + 1;
    if (!(quote = strchr(realm, '"')))
        goto authenticate;
    *quote = 0;
    username = strchr(username, '"') + 1;
    if (!(quote = strchr(username, '"')))
        goto authenticate;
    *quote = 0;
    response = strchr(response, '"') + 1;
    if (!(quote = strchr(response, '"')))
        goto authenticate;
    *quote = 0;
    uri = strchr(uri, '"') + 1;
    if (!(quote = strchr(uri, '"')))
        goto authenticate;
    *quote = 0;

    uint8_t digest_urp[16], digest_mu[16], digest_total[16];
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, username, strlen(username));
    MD5_Update(&ctx, ":", 1);
    MD5_Update(&ctx, realm, strlen(realm));
    MD5_Update(&ctx, ":", 1);
    MD5_Update(&ctx, config.password, strlen(config.password));
    MD5_Final(digest_urp, &ctx);

    MD5_Init(&ctx);
    MD5_Update(&ctx, req->method, strlen(req->method));
    MD5_Update(&ctx, ":", 1);
    MD5_Update(&ctx, uri, strlen(uri));
    MD5_Final(digest_mu, &ctx);

    int i;
    char buf[33];
    for (i=0; i<16; i++)
        sprintf(buf + 2*i, "%02X", digest_urp[i]);
    MD5_Init(&ctx);
    MD5_Update(&ctx, buf, 32);
    MD5_Update(&ctx, ":", 1);
    MD5_Update(&ctx, *nonce, strlen(*nonce));
    MD5_Update(&ctx, ":", 1);
    for (i=0; i<16; i++)
        sprintf(buf + 2*i, "%02X", digest_mu[i]);
    MD5_Update(&ctx, buf, 32);
    MD5_Final(digest_total, &ctx);

    for (i=0; i<16; i++)
        sprintf(buf + 2*i, "%02X", digest_total[i]);

    if (!strcmp(response, buf))
        return 0;
    warn("auth failed\n");

authenticate:
    resp->respcode = 401;
    int hdrlen = strlen(*nonce) + 40;
    char *authhdr = malloc(hdrlen);
    snprintf(authhdr, hdrlen, "Digest realm=\"taco\", nonce=\"%s\"", *nonce);
    msg_add_header(resp, "WWW-Authenticate", authhdr);
    free(authhdr);
    return 1;
}

static void *rtsp_conversation_thread_func(void *vfd) {
    // SIGUSR1 is used to interrupt this thread if blocked for read
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    rtsp_conn_info conn;
    memset(&conn, 0, sizeof(conn));

    int fd = *(int*)vfd;
    socklen_t slen = sizeof(conn.remote);
    
    fd = accept(fd, (struct sockaddr *)&conn.remote, &slen);
    if (fd < 0) {
        perror("failed to accept connection");
        goto shutdown;
    }

    rtsp_message *req, *resp;
    char *hdr, *auth_nonce = NULL;
    while ((req = rtsp_read_request(fd))) {
        resp = msg_init();
        resp->respcode = 400;

        apple_challenge(fd, req, resp);
        hdr = msg_get_header(req, "CSeq");
        if (hdr)
            msg_add_header(resp, "CSeq", hdr);
        msg_add_header(resp, "Audio-Jack-Status", "connected; type=analog");

        if (rtsp_auth(&auth_nonce, req, resp))
            goto respond;
        
        struct method_handler *mh;
        for (mh=method_handlers; mh->method; mh++) {
            if (!strcmp(mh->method, req->method)) {
                mh->handler(&conn, req, resp);
                break;
            }
        }

respond:
        msg_write_response(fd, resp);
        msg_free(req);
        msg_free(resp);
    } 

shutdown:
    if (fd > 0)
        close(fd);
    if (rtsp_playing()) {
        rtp_shutdown();
        player_stop();
        please_shutdown = 0;
        pthread_mutex_unlock(&playing_mutex);
    }
    if (auth_nonce)
        free(auth_nonce);
    return NULL;
}

void rtsp_listen_loop(void) {
    struct addrinfo hints, *info, *p;
    char portstr[6];
    int sockfd[2];
    int nsock = 0;
    int ret;
    memset(sockfd, 0, sizeof(sockfd));

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;

    snprintf(portstr, 6, "%d", config.port);

    ret = getaddrinfo(NULL, portstr, &hints, &info);
    if (ret) {
        die("getaddrinfo failed: %s\n", gai_strerror(ret));
    }

    for (p=info; p; p=p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, IPPROTO_TCP);
        int yes = 1;

        ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

#ifdef AF_INET6
        // some systems don't support v4 access on v6 sockets, but some do.
        // since we need to account for two sockets we might as well always.
        if (p->ai_family == AF_INET6)
            ret |= setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif

        if (!ret)
            ret = bind(fd, p->ai_addr, p->ai_addrlen);

        if (ret) {
            perror("could not bind a listen socket");
            continue;
        }

        listen(fd, 5);
        sockfd[nsock++] = fd;
    }

    freeaddrinfo(info);

    if (!nsock)
        die("could not bind any listen sockets!");


    int maxfd = sockfd[0];
    if (sockfd[1]>maxfd)
        maxfd = sockfd[1];

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd[0], &fds);
    if (sockfd[1])
        FD_SET(sockfd[1], &fds);

    mdns_register();

    printf("Listening for connections.\n");

    int acceptfd;
    while (select(maxfd+1, &fds, 0, 0, 0) >= 0) {
        if (FD_ISSET(sockfd[0], &fds))
            acceptfd = sockfd[0];
        if (FD_ISSET(sockfd[1], &fds) && sockfd[1])
            acceptfd = sockfd[1];

        // for now, we do not track these and let them die of natural causes.
        // XXX: this leaks threads; they need to be culled with pthread_tryjoin_np.
        // XXX: acceptfd could change before the thread is up. which should never happen, but still.
        pthread_t rtsp_conversation_thread;
        pthread_create(&rtsp_conversation_thread, NULL, rtsp_conversation_thread_func, &acceptfd);

        FD_SET(sockfd[0], &fds);
        if (sockfd[1])
            FD_SET(sockfd[1], &fds);
    }
    perror("select");
    die("fell out of the RTSP select loop\n");
}

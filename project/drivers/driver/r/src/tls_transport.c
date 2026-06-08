// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sb_socket_t;
#define SB_INVALID_SOCKET INVALID_SOCKET
#define SB_SOCKET_ERROR SOCKET_ERROR
#else
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
typedef int sb_socket_t;
#define SB_INVALID_SOCKET (-1)
#define SB_SOCKET_ERROR (-1)
#endif

typedef struct {
    sb_socket_t sock;
    SSL_CTX* ctx;
    SSL* ssl;
    int closed;
} sb_tls_conn;

static void sb_close_socket(sb_socket_t sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

static const char* sb_ssl_error_string(void) {
    static char buffer[256];
    unsigned long err = ERR_get_error();
    if (err == 0) {
        snprintf(buffer, sizeof(buffer), "unknown SSL error");
        return buffer;
    }
    ERR_error_string_n(err, buffer, sizeof(buffer));
    return buffer;
}

static void sb_conn_close(sb_tls_conn* conn) {
    if (conn == NULL || conn->closed) {
        return;
    }
    conn->closed = 1;
    if (conn->ssl != NULL) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    if (conn->ctx != NULL) {
        SSL_CTX_free(conn->ctx);
        conn->ctx = NULL;
    }
    if (conn->sock != SB_INVALID_SOCKET) {
        sb_close_socket(conn->sock);
        conn->sock = SB_INVALID_SOCKET;
    }
}

static void sb_conn_finalizer(SEXP extptr) {
    sb_tls_conn* conn = (sb_tls_conn*) R_ExternalPtrAddr(extptr);
    if (conn != NULL) {
        sb_conn_close(conn);
        Free(conn);
        R_ClearExternalPtr(extptr);
    }
}

static sb_tls_conn* sb_get_conn(SEXP extptr) {
    if (TYPEOF(extptr) != EXTPTRSXP) {
        Rf_error("Invalid transport handle type");
    }
    sb_tls_conn* conn = (sb_tls_conn*) R_ExternalPtrAddr(extptr);
    if (conn == NULL || conn->closed || conn->sock == SB_INVALID_SOCKET) {
        Rf_error("Transport handle is closed");
    }
    return conn;
}

#ifdef _WIN32
static void sb_ensure_winsock(void) {
    static int initialized = 0;
    if (!initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            Rf_error("WSAStartup failed");
        }
        initialized = 1;
    }
}
#endif

static int sb_set_nonblocking(sb_socket_t sock, int nonblocking) {
#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(sock, F_SETFL, flags);
#endif
}

static int sb_set_socket_timeouts(sb_socket_t sock, int timeout_ms) {
    if (timeout_ms <= 0) {
        return 0;
    }
#ifdef _WIN32
    DWORD tv = (DWORD) timeout_ms;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv)) != 0) {
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*) &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
#endif
}

static sb_socket_t sb_connect_tcp(const char* host, const char* port_str, int connect_timeout_ms, int socket_timeout_ms) {
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* it = NULL;
    sb_socket_t sock = SB_INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int gai = getaddrinfo(host, port_str, &hints, &result);
    if (gai != 0) {
#ifdef _WIN32
        Rf_error("DNS lookup failed for host '%s'", host);
#else
        Rf_error("DNS lookup failed for host '%s': %s", host, gai_strerror(gai));
#endif
    }

    for (it = result; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == SB_INVALID_SOCKET) {
            continue;
        }

        int tcp_no_delay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*) &tcp_no_delay, sizeof(tcp_no_delay));

        if (connect_timeout_ms > 0) {
            if (sb_set_nonblocking(sock, 1) != 0) {
                sb_close_socket(sock);
                sock = SB_INVALID_SOCKET;
                continue;
            }
        }

        int rc = connect(sock, it->ai_addr, (socklen_t) it->ai_addrlen);
        if (rc == 0) {
            if (connect_timeout_ms > 0) {
                sb_set_nonblocking(sock, 0);
            }
            if (sb_set_socket_timeouts(sock, socket_timeout_ms) != 0) {
                sb_close_socket(sock);
                sock = SB_INVALID_SOCKET;
                continue;
            }
            break;
        }

        if (connect_timeout_ms > 0) {
            int in_progress = 0;
#ifdef _WIN32
            int wsa_err = WSAGetLastError();
            in_progress = (wsa_err == WSAEWOULDBLOCK || wsa_err == WSAEINPROGRESS);
#else
            in_progress = (errno == EINPROGRESS);
#endif
            if (in_progress) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(sock, &write_set);

                struct timeval tv;
                tv.tv_sec = connect_timeout_ms / 1000;
                tv.tv_usec = (connect_timeout_ms % 1000) * 1000;

                int sel = select((int) (sock + 1), NULL, &write_set, NULL, &tv);
                if (sel > 0 && FD_ISSET(sock, &write_set)) {
                    int so_error = 0;
                    socklen_t len = sizeof(so_error);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*) &so_error, &len);
                    if (so_error == 0) {
                        sb_set_nonblocking(sock, 0);
                        if (sb_set_socket_timeouts(sock, socket_timeout_ms) == 0) {
                            break;
                        }
                    }
                }
            }
        }

        sb_close_socket(sock);
        sock = SB_INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (sock == SB_INVALID_SOCKET) {
        Rf_error("TCP connection to %s:%s failed", host, port_str);
    }
    return sock;
}

typedef struct {
    const char* password;
} sb_pass_ctx;

static int sb_pem_password_cb(char* buf, int size, int rwflag, void* userdata) {
    (void) rwflag;
    sb_pass_ctx* ctx = (sb_pass_ctx*) userdata;
    if (ctx == NULL || ctx->password == NULL) {
        return 0;
    }
    int len = (int) strlen(ctx->password);
    if (len > size - 1) {
        len = size - 1;
    }
    if (len > 0) {
        memcpy(buf, ctx->password, (size_t) len);
    }
    buf[len] = '\0';
    return len;
}

static int sb_ssl_mode_is_strict(const char* mode) {
    return (strcmp(mode, "require") == 0 ||
            strcmp(mode, "verify-ca") == 0 ||
            strcmp(mode, "verify_ca") == 0 ||
            strcmp(mode, "verify-full") == 0 ||
            strcmp(mode, "verify_full") == 0);
}

static int sb_ssl_mode_verify_full(const char* mode) {
    return (strcmp(mode, "verify-full") == 0 || strcmp(mode, "verify_full") == 0);
}

static const char* sb_scalar_string(SEXP x) {
    if (x == R_NilValue || XLENGTH(x) == 0) {
        return "";
    }
    return CHAR(asChar(x));
}

SEXP C_sb_tls_connect(SEXP hostSEXP,
                      SEXP portSEXP,
                      SEXP sslmodeSEXP,
                      SEXP rootCertSEXP,
                      SEXP certSEXP,
                      SEXP keySEXP,
                      SEXP passwordSEXP,
                      SEXP connectTimeoutSEXP,
                      SEXP socketTimeoutSEXP) {
#ifdef _WIN32
    sb_ensure_winsock();
#endif
    OPENSSL_init_ssl(0, NULL);

    const char* host = sb_scalar_string(hostSEXP);
    const char* sslmode = sb_scalar_string(sslmodeSEXP);
    const char* root_cert = sb_scalar_string(rootCertSEXP);
    const char* cert_file = sb_scalar_string(certSEXP);
    const char* key_file = sb_scalar_string(keySEXP);
    const char* key_password = sb_scalar_string(passwordSEXP);

    int port = asInteger(portSEXP);
    if (port <= 0 || port > 65535) {
        Rf_error("Invalid TCP port");
    }
    int connect_timeout_ms = asInteger(connectTimeoutSEXP);
    int socket_timeout_ms = asInteger(socketTimeoutSEXP);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    sb_socket_t sock = sb_connect_tcp(host, port_str, connect_timeout_ms, socket_timeout_ms);

    if (strcmp(sslmode, "disable") == 0) {
        sb_tls_conn* conn = (sb_tls_conn*) Calloc(1, sb_tls_conn);
        conn->sock = sock;
        conn->ctx = NULL;
        conn->ssl = NULL;
        conn->closed = 0;

        SEXP extptr = PROTECT(R_MakeExternalPtr(conn, R_NilValue, R_NilValue));
        R_RegisterCFinalizerEx(extptr, sb_conn_finalizer, TRUE);
        UNPROTECT(1);
        return extptr;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        sb_close_socket(sock);
        Rf_error("SSL_CTX_new failed: %s", sb_ssl_error_string());
    }

#ifdef TLS1_3_VERSION
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
#endif

    const int strict_verify = sb_ssl_mode_is_strict(sslmode);
    const int verify_full = sb_ssl_mode_verify_full(sslmode);
    SSL_CTX_set_verify(ctx, strict_verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

    if (strict_verify) {
        if (root_cert != NULL && root_cert[0] != '\0') {
            if (SSL_CTX_load_verify_locations(ctx, root_cert, NULL) != 1) {
                SSL_CTX_free(ctx);
                sb_close_socket(sock);
                Rf_error("Failed to load root certificate: %s", sb_ssl_error_string());
            }
        } else {
            if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
                SSL_CTX_free(ctx);
                sb_close_socket(sock);
                Rf_error("Failed to load default trust store: %s", sb_ssl_error_string());
            }
        }
    } else if (root_cert != NULL && root_cert[0] != '\0') {
        if (SSL_CTX_load_verify_locations(ctx, root_cert, NULL) != 1) {
            SSL_CTX_free(ctx);
            sb_close_socket(sock);
            Rf_error("Failed to load root certificate: %s", sb_ssl_error_string());
        }
    }

    if (cert_file[0] != '\0' || key_file[0] != '\0') {
        if (cert_file[0] == '\0' || key_file[0] == '\0') {
            SSL_CTX_free(ctx);
            sb_close_socket(sock);
            Rf_error("Both sslcert and sslkey must be provided together");
        }
        sb_pass_ctx pass_ctx;
        pass_ctx.password = (key_password[0] != '\0') ? key_password : NULL;
        if (pass_ctx.password != NULL) {
            SSL_CTX_set_default_passwd_cb(ctx, sb_pem_password_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, &pass_ctx);
        }
        if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1) {
            SSL_CTX_free(ctx);
            sb_close_socket(sock);
            Rf_error("Failed to load client certificate: %s", sb_ssl_error_string());
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(ctx);
            sb_close_socket(sock);
            Rf_error("Failed to load client private key: %s", sb_ssl_error_string());
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            sb_close_socket(sock);
            Rf_error("Client private key does not match certificate");
        }
    }

    SSL* ssl = SSL_new(ctx);
    if (ssl == NULL) {
        SSL_CTX_free(ctx);
        sb_close_socket(sock);
        Rf_error("SSL_new failed: %s", sb_ssl_error_string());
    }

    SSL_set_fd(ssl, (int) sock);
    SSL_set_tlsext_host_name(ssl, host);
    if (verify_full) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set1_host(param, host, 0);
    }

    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        sb_close_socket(sock);
        Rf_error("TLS handshake failed: %s", sb_ssl_error_string());
    }

    if (strict_verify) {
        long verify_result = SSL_get_verify_result(ssl);
        if (verify_result != X509_V_OK) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            sb_close_socket(sock);
            Rf_error("TLS certificate verification failed: %ld", verify_result);
        }
    }

    sb_tls_conn* conn = (sb_tls_conn*) Calloc(1, sb_tls_conn);
    conn->sock = sock;
    conn->ctx = ctx;
    conn->ssl = ssl;
    conn->closed = 0;

    SEXP extptr = PROTECT(R_MakeExternalPtr(conn, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(extptr, sb_conn_finalizer, TRUE);
    UNPROTECT(1);
    return extptr;
}

SEXP C_sb_tls_write(SEXP extptr, SEXP payloadSEXP) {
    sb_tls_conn* conn = sb_get_conn(extptr);
    if (TYPEOF(payloadSEXP) != RAWSXP) {
        Rf_error("Payload must be raw vector");
    }
    R_xlen_t len = XLENGTH(payloadSEXP);
    const unsigned char* data = RAW(payloadSEXP);
    R_xlen_t offset = 0;

    if (conn->ssl == NULL) {
        while (offset < len) {
            int chunk = (len - offset > INT_MAX) ? INT_MAX : (int) (len - offset);
            int rc = (int) send(conn->sock, (const char*) data + offset, (size_t) chunk, 0);
            if (rc > 0) {
                offset += (R_xlen_t) rc;
                continue;
            }
            Rf_error("Socket write failed");
        }
        return ScalarInteger((int) len);
    }

    while (offset < len) {
        int chunk = (len - offset > INT_MAX) ? INT_MAX : (int) (len - offset);
        int rc = SSL_write(conn->ssl, data + offset, chunk);
        if (rc > 0) {
            offset += (R_xlen_t) rc;
            continue;
        }
        int ssl_err = SSL_get_error(conn->ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            continue;
        }
        Rf_error("TLS write failed: %s", sb_ssl_error_string());
    }
    return ScalarInteger((int) len);
}

SEXP C_sb_tls_read_exact(SEXP extptr, SEXP nSEXP) {
    sb_tls_conn* conn = sb_get_conn(extptr);
    int n = asInteger(nSEXP);
    if (n < 0) {
        Rf_error("Requested byte count must be non-negative");
    }
    SEXP out = PROTECT(Rf_allocVector(RAWSXP, n));
    unsigned char* buffer = RAW(out);
    int offset = 0;

    if (conn->ssl == NULL) {
        while (offset < n) {
            int rc = (int) recv(conn->sock, (char*) buffer + offset, (size_t) (n - offset), 0);
            if (rc > 0) {
                offset += rc;
                continue;
            }
            if (rc == 0) {
                UNPROTECT(1);
                Rf_error("Socket closed");
            }
            UNPROTECT(1);
            Rf_error("Socket read failed");
        }
        UNPROTECT(1);
        return out;
    }

    while (offset < n) {
        int rc = SSL_read(conn->ssl, buffer + offset, n - offset);
        if (rc > 0) {
            offset += rc;
            continue;
        }
        if (rc == 0) {
            UNPROTECT(1);
            Rf_error("TLS socket closed");
        }
        int ssl_err = SSL_get_error(conn->ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            continue;
        }
        UNPROTECT(1);
        Rf_error("TLS read failed: %s", sb_ssl_error_string());
    }
    UNPROTECT(1);
    return out;
}

SEXP C_sb_tls_close(SEXP extptr) {
    if (TYPEOF(extptr) == EXTPTRSXP) {
        sb_tls_conn* conn = (sb_tls_conn*) R_ExternalPtrAddr(extptr);
        if (conn != NULL) {
            sb_conn_close(conn);
            Free(conn);
            R_ClearExternalPtr(extptr);
        }
    }
    return R_NilValue;
}

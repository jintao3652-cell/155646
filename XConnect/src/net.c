#include "net.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
static int g_net_initd = 0;
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#endif

int net_init(void) {
#ifdef _WIN32
    if (!g_net_initd) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        g_net_initd = 1;
    }
    return 0;
#else
    signal(SIGPIPE, SIG_IGN); /* 防止对端关闭时发送导致进程退出 */
    return 0;
#endif
}

net_sock_t net_listen(int port) {
    net_sock_t lsock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    int on = 1;
    if (lsock == NET_INVALID) return NET_INVALID;

#ifdef _WIN32
    setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
#else
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(lsock, 4) != 0) {
        net_close(lsock);
        return NET_INVALID;
    }
    return lsock;
}

net_sock_t net_accept(net_sock_t lsock) {
    struct sockaddr_storage ss;
#ifdef _WIN32
    int len = (int)sizeof(ss);
#else
    socklen_t len = (socklen_t)sizeof(ss);
#endif
    return accept(lsock, (struct sockaddr*)&ss, &len);
}

int net_read_line(net_sock_t sock, char* buf, int size) {
    int pos = 0;
    char c;
    while (pos < size - 1) {
        int n = recv(sock, &c, 1, 0);
        if (n == 0) {
            return pos > 0 ? pos : 0; /* 关闭 */
        }
        if (n < 0) return -1;
        if (c == '\n') {
            /* 去掉尾部 \r */
            if (pos > 0 && buf[pos - 1] == '\r') pos--;
            buf[pos] = '\0';
            return pos;
        }
        if (c != '\r') buf[pos++] = c;
    }
    /* 行超出缓冲：丢弃本行剩余字节直到换行，防止数据流错位。
     * 返回 -2 表示"超长行被丢弃"，调用方应继续读取下一条。 */
    while (1) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return -2;
        if (c == '\n') return -2;
    }
}

int net_write_all(net_sock_t sock, const char* buf, int len) {
    int off = 0;
    while (off < len) {
        int n = send(sock, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

int net_send_line(net_sock_t sock, const char* line) {
    int len = (int)strlen(line);
    char nl = '\n';
    if (len > 0 && line[len - 1] != '\n') {
        if (net_write_all(sock, line, len) != 0) return -1;
        return net_write_all(sock, &nl, 1);
    }
    return net_write_all(sock, line, len);
}

void net_close(net_sock_t sock) {
    if (sock == NET_INVALID) return;
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
    closesocket(sock);
#else
    shutdown(sock, SHUT_RDWR);
    close(sock);
#endif
}

void net_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}
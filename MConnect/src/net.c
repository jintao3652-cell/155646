#include "net.h"
#include "mconnect.h"

#include <stdio.h>
#include <string.h>

static int g_net_initd = 0;

int net_init(void) {
    if (!g_net_initd) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        g_net_initd = 1;
    }
    return 0;
}

net_sock_t net_listen(int port) {
    net_sock_t lsock;
    struct sockaddr_in addr;
    int on = 1;
    BOOL exclusive = TRUE;

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == NET_INVALID) return NET_INVALID;

    /* SO_EXCLUSIVEADDRUSE：防止其他进程（例如残留的上一个实例）抢同一端口，
     * 抢到时立即失败，好让调用方切到备用端口。 */
    setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char*)&exclusive, sizeof(exclusive));
    (void)on;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 只监听本机 */
    addr.sin_port = htons((unsigned short)port);

    if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(lsock, 4) != 0) {
        closesocket(lsock);
        return NET_INVALID;
    }
    return lsock;
}

net_sock_t net_accept(net_sock_t lsock) {
    struct sockaddr_storage ss;
    int len = (int)sizeof(ss);
    return accept(lsock, (struct sockaddr*)&ss, &len);
}

int net_read_line(net_sock_t sock, char* buf, int size) {
    int pos = 0;
    char c;
    while (pos < size - 1) {
        int n = recv(sock, &c, 1, 0);
        if (n == 0) return pos > 0 ? pos : 0;
        if (n < 0) return -1;
        if (c == '\n') {
            if (pos > 0 && buf[pos - 1] == '\r') pos--;
            buf[pos] = '\0';
            return pos;
        }
        if (c != '\r') buf[pos++] = c;
    }
    /* 超长行：丢弃到换行为止，避免数据流错位 */
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

/* 首包嗅探：判断连接是否 HTTP 请求（同端口同时服务桥接协议与 HTTP 快照）。
 * MSG_PEEK 不消费数据，桥接路径完全不受影响。 */
int net_peek_is_http(net_sock_t sock) {
    static const char* kMethods[] = { "GET ", "HEAD", "POST", "PUT ", "OPTI", "DELE" };
    char buf[8];
    int i;
    int got = recv(sock, buf, sizeof(buf), MSG_PEEK);
    if (got < 4) return 0;
    for (i = 0; i < (int)(sizeof(kMethods) / sizeof(kMethods[0])); i++)
        if (memcmp(buf, kMethods[i], 4) == 0) return 1;
    return 0;
}

/* 消费掉 HTTP 请求头（读到 \r\n\r\n 为止）。GET/OPTIONS 无请求体。
 * 返回 0 成功；<0 = 对端关闭或头部长度异常。 */
int net_skip_http_head(net_sock_t sock) {
    char tail[4] = { 0, 0, 0, 0 };
    int i = 0;
    char c;
    while (i < 16 * 1024) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return -1;
        tail[i & 3] = c;
        i++;
        if (tail[0] == '\r' && tail[1] == '\n' &&
            tail[2] == '\r' && tail[3] == '\n') return 0;
    }
    return -1;
}

int net_send_line(net_sock_t sock, const char* line) {
    int len = (int)strlen(line);
    static const char nl = '\n';
    if (len > 0 && line[len - 1] != '\n') {
        if (net_write_all(sock, line, len) != 0) return -1;
        return net_write_all(sock, &nl, 1);
    }
    return net_write_all(sock, line, len);
}

void net_close(net_sock_t sock) {
    if (sock == NET_INVALID) return;
    shutdown(sock, SD_BOTH);
    closesocket(sock);
}

void net_shutdown(net_sock_t sock) {
    if (sock == NET_INVALID) return;
    shutdown(sock, SD_BOTH);
}

void net_shutdown_listen(net_sock_t lsock) {
    if (lsock == NET_INVALID) return;
    /* 关闭 socket 后阻塞的 accept 会返回错误，线程得以退出 */
    shutdown(lsock, SD_BOTH);
    closesocket(lsock);
}
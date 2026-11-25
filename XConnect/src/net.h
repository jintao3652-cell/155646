/* TCP 跨平台封装（WinSock2 / POSIX）。 */
#ifndef XCONNECT_NET_H
#define XCONNECT_NET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>   /* Sleep 等（winsock2 在前避免冲突） */
typedef SOCKET net_sock_t;
#define NET_INVALID INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int net_sock_t;
#define NET_INVALID (-1)
#endif

/* 一次性初始化网络库（线程安全可多次调用）。返回 0 成功。 */
int net_init(void);

/* 创建并绑定监听 socket。返回 NET_INVALID 失败。 */
net_sock_t net_listen(int port);

/* 阻塞等待一个连接（取消：返回 NET_INVALID 且 errno 为 EINTR）。 */
net_sock_t net_accept(net_sock_t lsock);

/* 读取一行（以 \n 结尾），去掉尾部 \r\n。返回行长度；0=对方关闭；<0 错误。 */
int net_read_line(net_sock_t sock, char* buf, int size);

/* 发送全部字节。返回 0 成功，<0 失败。 */
int net_write_all(net_sock_t sock, const char* buf, int len);

/* 发送一行文本（自动追加 \n，若缺失）。 */
int net_send_line(net_sock_t sock, const char* line);

void net_close(net_sock_t sock);

/* 只 shutdown 不 close：用于唤醒其它线程中阻塞的 recv/accept，
 * 由该线程自己 close（避免 double-close）。 */
void net_shutdown(net_sock_t sock);

void net_sleep_ms(int ms);

#endif

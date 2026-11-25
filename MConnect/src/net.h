/* TCP 封装（WinSock2）。协议与 XConnect 完全一致（行分隔 JSON）。 */
#ifndef MCONNECT_NET_H
#define MCONNECT_NET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>   /* winsock2 必须在前，否则 winsock.h 冲突 */
typedef SOCKET net_sock_t;
#define NET_INVALID INVALID_SOCKET
#endif

/* 一次性初始化网络库（可多次调用）。返回 0 成功。 */
int net_init(void);

/* 创建并绑定监听 socket（仅 127.0.0.1）。失败返回 NET_INVALID。 */
net_sock_t net_listen(int port);

/* 阻塞等待一个连接。 */
net_sock_t net_accept(net_sock_t lsock);

/* 读取一行（\n 结尾），去掉尾部 \r\n。返回行长；0=对端关闭；<0 错误；
 * -2 = 超长行已丢弃（调用方继续读下一条）。 */
int net_read_line(net_sock_t sock, char* buf, int size);

/* 发送全部字节。返回 0 成功。 */
int net_write_all(net_sock_t sock, const char* buf, int len);

/* 发送一行文本（自动补 \n）。 */
int net_send_line(net_sock_t sock, const char* line);

/* 主动关闭并唤醒阻塞中的 accept/recv。 */
void net_close(net_sock_t sock);

/* 只 shutdown 不 close：唤醒其它线程中阻塞的 recv，由该线程自己关闭。 */
void net_shutdown(net_sock_t sock);

/* 让阻塞中的 accept 立刻返回（模拟器退出时用）。 */
void net_shutdown_listen(net_sock_t lsock);

#endif
#include "co_struct.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

void func1(void *arg) {
    int port = *(int*)arg;
    int sock_tcp_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(port);
    bind(sock_tcp_fd, (sockaddr*)&addr, sizeof(addr));
    listen(sock_tcp_fd, 1024);
    // 监听连接，做一个echo服务器
    int nfd = 1;
    pollfd pf[1024];
    bzero(pf, sizeof(pf));
    pf[0].events = POLLIN;
    pf[0].fd = sock_tcp_fd;

    for(;;) {
        int n = co_poll(pf, nfd, 6000000); // 让出cpu给loop
        // printf("co_poll return %d, tot %d\n", n, nfd);
        if (n > 0) {
            for (int i = 1; i < nfd; ++i) {
                if (pf[i].revents & POLLIN) {
                    char buf[1024];
                    // printf("read from fd %d\n", pf[i].fd);
                    int n = read(pf[i].fd, buf, sizeof(buf));
                    buf[n] = '\0';
                    if (n > 0) {
                        printf("read %s from fd %d\n", buf, pf[i].fd);
                        write(pf[i].fd, buf, n);
                    } else {
                        close(pf[i].fd);
                        nfd--;
                    }
                }
            }
            if (pf[0].revents & POLLIN) {
                int fd = accept(sock_tcp_fd, NULL, NULL);
                pf[nfd].fd = fd;
                pf[nfd].events = POLLIN;
                nfd++;
                printf("accept %d\n", fd);
            }
        }
    }
}

int main() {
    co_global_init();

    // 创建100个服务器
    co_struct *cos[100];
    for (int i = 0; i < 100; ++i) {
        int *port = new int;
        *port = 10000 + i; // 将会listen这些端口 尝试连接
        co_create(&cos[i], func1, port);
        co_resume(cos[i]);
    }

    co_eventloop();
    co_global_release();
}
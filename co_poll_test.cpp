#include "co_struct.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

void func1(void *) {
    int sock_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(8055);
    bind(sock_tcp_fd, (sockaddr*)&addr, sizeof(addr));
    listen(sock_tcp_fd, 1024);
    pollfd pf;
    pf.events = POLLIN | POLLOUT;
    pf.fd = sock_tcp_fd;

    for(int i = 0; ; i++) {
        int n = co_blocked_poll(&pf, 1, 2000); // 让出cpu给loop
        if (n > 0) {
            printf("有反应了(n=%d)\n", n); // 当使用netcat连接时print
        }
    }
}

int main() {
    co_global_init();

    struct co_struct *co1 = NULL;
    co_struct_init(&co1, func1, NULL);
    co_resume(co1);

    co_eventloop();

    co_global_release();
}
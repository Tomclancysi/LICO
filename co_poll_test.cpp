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
    while(true) {

    }
}

void func2(void *) {
    // connect to server
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(8055);
    connect(sock_fd, (sockaddr*)&addr, sizeof(addr));
    printf("connect sock_fd %d\n", sock_fd);
}

int main() {
    co_global_init();

    struct co_struct *co1 = NULL;
    co_struct_init(&co1, func1, NULL);
    struct co_struct *co2 = NULL;
    co_struct_init(&co2, func2, NULL);
    co_resume(co1);
    co_resume(co2);

    co_eventloop();

    co_global_release();
}
#include "co_struct.h"
#include <stdio.h>

struct obj
{
    int x;
};


void callee(void *o) {
    // fib
    struct obj *out = (struct obj*)o;

    int a = 1, b = 1;
    for (;;) { // 这就体现了协程的好处，能够实现懒计算
        out->x = a;
        int c = a + b;
        a = b;
        b = c;
        printf("callee will yield %d\n", out->x);
        co_yield();
    }
    printf("callee is ended\n");
}

void caller() {
    struct obj o;
    co_struct *subrt = NULL;
    co_struct_init(&subrt, callee, &o);
    for (int i = 0; i < 10; i++) { // 如果循环次数大于callee的生成次数就会出错，callee可以设置成无限循环
        co_resume(subrt);
        printf("caller get %d\n", o.x);
    }
    co_struct_release(subrt);
}

int main() {
    co_global_init();
    caller();
    co_global_release();
}
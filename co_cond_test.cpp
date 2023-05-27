#include "co_struct.h"
#include <queue>
#include <string>

std::queue<std::string> queue;
co_condition_variable cv;

void Consumer (void*) {
    const int N = 3; // 每天都要吃早中晚饭
    while(true) {
        int sig = cv.wait_for([]() {
            return queue.size() > 3;
        }, 2000);
        if (sig == 0) {
            printf("********吃，爽！*********\n");
            for (int i = 0; i < N; ++i) {
                std::string s = queue.front();
                queue.pop();
                printf("Consumer eat %s\n", s.c_str());
            }
            printf("-----------------------\n");
            // flush printf
            fflush(stdout);
        } else {
            printf("Consumer快要饿死了！\n");
        }
    }
}

void Producer (void*) {
    std::string menu[] = {"apple", "banana", "orange", "pear", "watermelon", "grape", "strawberry", "cherry", "mango"};
    int n = sizeof(menu) / sizeof(menu[0]);
    while(true) {
        queue.push(menu[rand() % n]);
        cv.notify_one();
        co_sleep(1000);
    }
}

int main() {
    co_global_init();
    struct co_struct *co1 = NULL;
    co_create(&co1, Consumer, NULL);
    struct co_struct *co2 = NULL;
    co_create(&co2, Producer, NULL);

    co_resume(co1);
    co_resume(co2);

    co_eventloop();
    co_global_release();
}

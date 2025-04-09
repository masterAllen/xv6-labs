#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// prime_start 是父进程传入的第一个质数
void make_filter(int fd_by_parent[2], int prime_start) {
    // ! 关键句：先关闭写端，否则后续 read 会阻塞，read 退出要求父子进程的写都关闭
    close(fd_by_parent[1]);

    int fd_by_myself[2];
    pipe(fd_by_myself);

    printf("prime %d\n", prime_start);
    // int nowid = getpid();
    // printf("Thread %d, prime %d, id: %d, %d\n", nowid, prime_start, fd_by_myself[0], fd_by_myself[1]);

    int pid = -1;
    int num = 0;
    while (read(fd_by_parent[0], &num, sizeof(num)) > 0) {
        if (num % prime_start != 0) {
            write(fd_by_myself[1], &num, sizeof(num));
            // 如果是第一次写，那么就 fork 一个进程
            if (pid == -1) {
                pid = fork();
                if (pid == 0) {
                    // ! 关键句：关闭不需要的管道，否则资源会耗尽
                    close(fd_by_parent[0]);
                    make_filter(fd_by_myself, num);
                }
            }
        }
    }

    // 因为上面循环结束之后，已经不需要再往管道里写了，所以这里顺序没有关系
    close(fd_by_myself[0]);
    close(fd_by_myself[1]);
    close(fd_by_parent[0]);

    // printf("Thread %d done\n", nowid);
}

int main(int argc, char *argv[])
{
    if (argc != 1) {
        fprintf(2, "Usage: primes\n");
        exit(1);
    }

    const int MAX_PRIME = 280;

    int primes[MAX_PRIME];
    // 2, 3, 4, 5 ....
    for (int i = 0; i < MAX_PRIME; i++) {
        primes[i] = i + 2;
    }

    int fd_in_main[2];
    pipe(fd_in_main);

    if (fork() == 0) {
        make_filter(fd_in_main, 2);
    } else {
        // 发送 2, 3, 4...
        close(fd_in_main[0]);
        for (int i = 0; i < MAX_PRIME; i++) {
            write(fd_in_main[1], &primes[i], sizeof(primes[i]));
        }
        close(fd_in_main[1]);
    }
    wait(0);
}
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    if (argc != 1) {
        fprintf(2, "Usage: pingpong\n");
        exit(1);
    }

    char buf[1];

    int pipe_fd[2];
    pipe(pipe_fd); // 创建管道

    if (fork() == 0) {
        close(pipe_fd[1]); // 关闭写端
        read(pipe_fd[0], buf, 1);
        close(pipe_fd[0]);

        int myid = getpid();
        printf("%d: received ping\n", myid);

    } else {
        close(pipe_fd[0]); // 关闭读端
        write(pipe_fd[1], buf, 1);
        close(pipe_fd[1]);

        int myid = getpid();
        printf("%d: received pong\n", myid);
    }
}
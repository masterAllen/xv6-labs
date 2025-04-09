#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"


int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(2, "Usage: xargs <input>\n");
        exit(1);
    }

    char* cmd = argv[1];
    char** cmd_argv = argv;
    for (int i = 0; i < argc-1; i++) {
        cmd_argv[i] = argv[i + 1];
    }

    char buf[512];
    read(0, buf, sizeof(buf));

    char line[512];
    for (int i = 0, j = 0; i < strlen(buf); i++) {
        if (buf[i] != '\n' && buf[i] != '\0') {
            line[j++] = buf[i];
        } else {
            line[j] = '\0';
            j = 0;

            cmd_argv[argc-1] = line;
            cmd_argv[argc] = 0;

            if (fork() == 0) {
                // printf("exec: %s\n", cmd);
                // for (int i = 0; i < argc; i++) {
                //     printf("argv[%d]: %s\n", i, cmd_argv[i]);
                // }
                exec(cmd, cmd_argv);
            } else {
                wait((int*)0);
            }
        }
    }

}
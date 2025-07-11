中文 | [English](./../en/lab01_util.md)

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## sleep & pingpong

比较简单，没什么值得记录的。

## find
1. `ls.c` 中的 `fmtnames` 会添加一些空格（用于格式化显示），在 `find.c` 中**不要**这样做。

## xargs
1. `exec(cmd, cmd_argvs)`：注意 `cmd_argvs[0]` 应该是命令本身！例如，在 `grep hello a/b` 中，`argvs[0]` 是 grep。

## primes
1. 每个进程记录第一个数字，从其父进程接收输入，并创建一个子进程来处理不能被第一个数字整除的数字。比如我创建了这个函数：`make_primes(int fd_by_parent, int first_num)`。
2. 仔细考虑：对于进程，哪些管道可以安全移除：
    - 不需要向父进程写入和从子进程读取。
    - **不需要父进程从爷爷进程（父亲的父亲）读取数据。（不考虑这个过不了 2024 fall）** 
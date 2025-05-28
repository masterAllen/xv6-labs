
> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## sleep & pingpong

Too Easy, nothing worth to record.

## find
1. `fmtnames` in `ls.c` append some spaces(for formatting purposes), **DO NOT** do this in `find.c`.

## xargs
1. `exec(cmd, cmd_argvs)`: Note that `cmd_argvs[0]` should be cmd itself! For example, in `grep hello a/b`, `argvs[0]` is grep.

## primes
1. Each process records the first number, receives input from its parent, and creates a child process to handle numbers not divisible by the first. I create this function: `make_primes(int fd_by_parent, int first_num)`.
2. Consider carefully: which pipes can be safely removed for a given process:
    - Dont need write to parent and read from child.
    - **Dont need the pipe for the parent to read data from its own parent.**
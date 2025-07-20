中文 | [English](./../en/lab08_filesystem.md)

# lab08_filesystem

> [!IMPORTANT]  
> 以下文章可能会对您的解决有决定性的帮助。如果您希望自己独立解决，**请勿**查看。如果您觉得有帮助，请考虑 Star 这个仓库！

## Large files

解决这个问题的关键在于**理解 xv6 的文件系统数据结构**。对于中文读者，[这篇博客文章](https://www.cnblogs.com/yinheyi/p/16464407.html)很好。

方法很直接：将 `dinode` 和 `inode` 中的 `addrs` 数组大小增加一个，然后找到并更新使用这些 `addrs` 的所有地方。

代码修改按照现有代码的方式改就行了。需要更新的主要函数是 `fs.c` 中的 `bmap` 和 `itrunc`。

最后，网页上紫色框所说的，我们需要将 `NDIRECT` 从 12 改为 11。

## Symbolic links

这个问题其实总体也很直接，我记录一些关键点。

### 1. 处理循环

就像提示所说：

> *"If the links form a cycle, you must return an error code. You may approximate this by returning an error code if the depth of links reaches some threshold (e.g., 10)."*

需要在 `sys_open` 函数中处理一下：

```cpp
void sys_open(void) {
    // ...
    if (!(omode & O_NOFOLLOW)) {
        // 设置深度限制为 10 以防止无限循环
        int depth = 10;

        struct inode *ip2;
        while (ip->type == T_SYMLINK) {
            // 将内容读入 path 以找到链接的文件
            if ((readi(ip, 0, (uint64)&path, 0, MAXPATH)) < 0) {
                iunlockput(ip);
                end_op();
                return -1;
            }

            // 根据 path 找到相应的 inode
            if((ip2 = namei(path)) == 0){
                iunlockput(ip);
                end_op();
                return -1;
            }
            iunlockput(ip);
            ip = ip2;
            // 添加锁以防止并发干扰
            ilock(ip);
            depth--;
        }

        if (depth == 0) {
            iunlockput(ip);
            end_op();
            return -1;
        }
    }
```

### 2. 我解决 symlink 时的历程

我们可以将 `sys_symlink` 分成两个主要部分：

1. 创建文件
2. 将 `srcpath` 写入文件

对于第一步，可以遵循 `sys_open` 的模式：

```cpp
void sys_open() {
    // ...
    begin_op();

    if(omode & O_CREATE){
        // 可以参考
        ip = create(path, T_FILE, 0, 0);
        if(ip == 0){
            end_op();
            return -1;
        }
    } else {
        // ...

uint64 sys_symlink(void) {
  char srcpath[MAXPATH], targetpath[MAXPATH];
  struct inode *ip;

  if(argstr(0, srcpath, MAXPATH) < 0 || argstr(1, targetpath, MAXPATH) < 0)
    return -1;

  // 遵循 sys_open 的日志操作模式
  begin_op();
  if((ip = create(targetpath, T_SYMLINK, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  // ...
```

第二步，写入 `srcpath` 到文件里面，更有挑战，花费的时间也多一点。

#### 2.1 效仿 `sys_write`

最初，我尝试遵循 `sys_write` 的模式：
```cpp
uint64 sys_write(void) {
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}
```

但是，我们在 `sys_symlink` 中只有 `inode* ip`，而不是 `file* f`。

#### 2.2 创建 `file` 结构

然后我就参考 `sys_open`，尝试创建一个 `file` 结构，然后将 `inode` 和它绑定起来：

```cpp
void sys_open() {
    // ...
    begin_op();
    // ...

    // 创建一个文件结构并将其分配给 fd
    if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
        if(f)
        fileclose(f);
        iunlockput(ip);
        end_op();
        return -1;
    }
    // ...

    // 在 sys_open 中组合 inode 和 file*
    if(ip->type == T_DEVICE){
        f->type = FD_DEVICE;
        f->major = ip->major;
    } else {
        f->type = FD_INODE;
        f->off = 0;
    }
    f->ip = ip;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

    if((omode & O_TRUNC) && ip->type == T_FILE){
        itrunc(ip);
    }

    iunlock(ip);
    end_op();

uint64 sys_symlink(void) {
    char srcpath[MAXPATH], targetpath[MAXPATH];
    struct inode *ip;

    // ...

    // 创建一个文件结构并设置 ip
    struct file *f;
    if((f = filealloc()) == 0){
        return -1;
    }
    f->type = FD_INODE;
    f->off = 0;
    f->ip = ip;
    f->readable = O_WRONLY || O_RDWR;
    f->writable = O_WRONLY || O_RDWR;

    // 像 sys_write 一样写入
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);

    return 0;
}
```

#### 2.3 调整日志操作

结果程序 hang 了，所以我检查了 `filewrite`，发现它在写入前调用了 `begin_op()`。

原来在 `sys_symlink` 中创建 inode 时，我们已经调用了 `begin_op()`，我们有了冲突。所以在创建后添加 `end_op()`：

```cpp
uint64 sys_symlink(void) {
    // ...

    // 创建
    begin_op();
    if((ip = create(targetpath, T_SYMLINK, 0, 0)) == 0) {
        end_op();
        return -1;
    }
    // 在创建后添加 end_op
    iunlock(ip);
    end_op();

    // FileWrite
    // 1. 创建一个文件结构
    // 2. filewrite
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);

    // ...
}
```

#### 2.4 添加 fileclose

仍然错误，我首先推测是需要在写入后关闭文件。检查 `sys_close` 后，在 `sys_symlink` 中添加了相应的代码：

```cpp
uint64 sys_symlink(void) {
    // ...

    // 创建

    // FileWrite
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);

    // 关闭 <--- 新增
    fileclose(f);
```

#### 2.5 调查 writei

结果... 仍然无法将内容写入目标文件！

没办法，所以就进 `filewrite` 里面看看，明明就调用了它为啥不对嘞，发现它也是调用了 `writei` 函数：

```cpp
int filewrite(struct file *f, uint64 addr, int n) {
    // ...
    while (...) {
        // ..

        // 关键部分
        begin_op();
        ilock(f->ip);
        if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
            f->off += r;
        iunlock(f->ip);
        end_op();

        // ..
    }
```

`writei` 中的注释特别有帮助：

```cpp
// Write data to inode. Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address; <-- 关键点！！
// otherwise, src is a kernel address.
// ...
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
```

看到了吗？**当 `user_src` 为 1 时，`src` 是用户虚拟地址**。但我传给 `filewrite` 的 `srcpath` 是内核地址！

```cpp
uint64 sys_symlink(void) {
    char srcpath[MAXPATH], targetpath[MAXPATH];
    if(argstr(0, srcpath, MAXPATH) < 0 || argstr(1, targetpath, MAXPATH) < 0)
        return -1;

    // ...
    // FileWrite --> srcpath 是内核地址，因为我们是在内核中定义的 srcpath！
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);
    //...
```

所以应该计算 srcpath 的用户地址？？想啥呢，这就是 `sys_symlink` 里面定义的一个临时变量。那其实我们可以直接使用 `writei`！这样甚至根本不需要创建 `file` 了，更简单了：

```cpp
uint64 sys_symlink(void) {
    begin_op();
    // 创建...

    // 直接使用 writei，无需创建 file！
    int ret = writei(ip, 0, (uint64)srcpath, 0, strlen(srcpath)+1);
    if(ret < 0) {
        iunlockput(ip);
        end_op();
        return -1;
    }
    iunlockput(ip);
    end_op();
}
``` 
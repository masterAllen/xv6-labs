[中文](./../zh/lab08_filesystem_zh.md) | English

# lab08_filesystem

> [!IMPORTANT]  
> The following hint may contain a decisive solution. If you wish to solve the problem by yourself, **DO NOT** view it. If you find it helpful, consider staring this repository!

## Large files

The key to solving this problem lies in understanding the file system data structures in xv6. For Chinese readers, [this blog post](https://www.cnblogs.com/yinheyi/p/16464407.html) provides excellent insights.

The solution is straightforward: we need to increase the size of the `addrs` array in both `dinode` and `inode` by one, then trace and update all the places where these `addrs` are used.

The code modifications are relatively simple - just follow the existing patterns in the codebase. The main functions that require updates are `bmap` and `itrunc` in `fs.c`.

Finally, as indicated in the purple box on the webpage, we need to change `NDIRECT` from 12 to 11.

## Symbolic links

This problem is relatively straightforward, so I'll focus on highlighting the key points.

### 1. Handling Cycles

As suggested in the hint:

> *"If the links form a cycle, you must return an error code. You may approximate this by returning an error code if the depth of links reaches some threshold (e.g., 10)."*

This situation needs to be handled in the `sys_open` function. Here's how:

```cpp
void sys_open(void) {
    // ...
    if (!(omode & O_NOFOLLOW)) {
        // Set a depth limit of 10 to prevent infinite cycles
        int depth = 10;

        struct inode *ip2;
        while (ip->type == T_SYMLINK) {
            // Read the content into path to find the linked file
            if ((readi(ip, 0, (uint64)&path, 0, MAXPATH)) < 0) {
                iunlockput(ip);
                end_op();
                return -1;
            }

            // Find the corresponding inode based on path
            if((ip2 = namei(path)) == 0){
                iunlockput(ip);
                end_op();
                return -1;
            }
            iunlockput(ip);
            ip = ip2;
            // Add lock to prevent concurrent interference
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

### 2. The path for me to solve symlink


We can break down `sys_symlink` into two main parts:

1. Create the file
2. Write the `srcpath` into the file

For the first step, we can follow the pattern from `sys_open`:

```cpp
void sys_open() {
    // ...
    begin_op();

    if(omode & O_CREATE){
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

  // Following sys_open's pattern for log operations
  begin_op();
  if((ip = create(targetpath, T_SYMLINK, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  // ...
```

The second step proved more challenging.

#### 2.1 Examining `sys_write`

Initially, I tried to follow the pattern from `sys_write`:
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

However, we only have `inode* ip` in `sys_symlink`, not a `file* f`.

#### 2.2 Creating a `file` Structure

I then attempted to create a `file` structure similar to `sys_open`:

```cpp
void sys_open() {
    // ...
    begin_op();
    // ...

    // Create a file struct and allocate it to a fd
    if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
        if(f)
        fileclose(f);
        iunlockput(ip);
        end_op();
        return -1;
    }
    // ...

    // Combine inode and file* in sys_open
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

    // Create a file structure and set the ip
    struct file *f;
    if((f = filealloc()) == 0){
        return -1;
    }
    f->type = FD_INODE;
    f->off = 0;
    f->ip = ip;
    f->readable = O_WRONLY || O_RDWR;
    f->writable = O_WRONLY || O_RDWR;

    // Write as sys_write does
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);

    return 0;
}
```

#### 2.3 Adjusting Log Operations

The program hung, so I examined `filewrite` and found it calls `begin_op()` before writing.

Since we already called `begin_op()` when creating the inode in `sys_symlink`, we had a conflict. We needed to add `end_op()` after creation:

```cpp
uint64 sys_symlink(void) {
    // ...

    // Create
    begin_op();
    if((ip = create(targetpath, T_SYMLINK, 0, 0)) == 0) {
        end_op();
        return -1;
    }
    // Add end_op after creation
    iunlock(ip);
    end_op();

    // FileWrite
    // 1. Create a file structure
    // 2. filewrite
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);

    // ...
}
```

#### 2.4 Adding fileclose

Still encountering errors, I realized we needed to close the file after writing. After checking `sys_close`, I added the corresponding code to `sys_symlink`:

```cpp
uint64 sys_symlink(void) {
    // ...

    // Create

    // FileWrite
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);

    // Close <--- New
    fileclose(f);
```

#### 2.5 Investigating writei

Unfortunately, I still couldn't write the content to the destination file!

I had to examine `filewrite` more closely, focusing on the `writei` function:

```cpp
int filewrite(struct file *f, uint64 addr, int n) {
    // ...
    while (...) {
        // ..

        // Critical section
        begin_op();
        ilock(f->ip);
        if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
            f->off += r;
        iunlock(f->ip);
        end_op();

        // ..
    }
```

The comments in `writei` were particularly enlightening:

```cpp
// Write data to inode. Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address; <-- Key Point!!
// otherwise, src is a kernel address.
// ...
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
```

See? When `user_src` is 1, `src` is a user virtual address. However, the `srcpath` I passed to `filewrite` was a kernel address!

```cpp
uint64 sys_symlink(void) {
    char srcpath[MAXPATH], targetpath[MAXPATH];
    if(argstr(0, srcpath, MAXPATH) < 0 || argstr(1, targetpath, MAXPATH) < 0)
        return -1;

    // ...
    // FileWrite --> srcpath is a kernel address because we declared it in kernel!
    filewrite(f, (uint64)&srcpath, strlen(srcpath)+1);
    //...
```

After understanding `writei`'s behavior, I realized we could use it directly! We didn't need to create a file structure at all, making everything much simpler:

```cpp
uint64 sys_symlink(void) {
    begin_op();
    // Create...

    // Use writei directly without creating a file structure!
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
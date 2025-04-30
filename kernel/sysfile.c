//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

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

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  // mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
  uint64 addr;
  size_t len;
  int prot;
  int flags;
  int fd;
  int offset;
  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  // 题目为了简单，addr 固定是 0，offset 也是 0
  if (addr != 0 || offset != 0) {
    printf("addr: %p, offset: %d\n", (void *)addr, offset);
    return -1;
  }

  struct proc *p = myproc();

  // 如果文件只读、但是 mmap 中：MAP_SHARED 且 PROT_WRITE，则报错
  if (p->ofile[fd]->writable == 0 && (flags & MAP_SHARED) && (prot & PROT_WRITE)) {
    printf("mmap: file is read-only, but mmap with MAP_SHARED and PROT_WRITE\n");
    return -1;
  }

  acquire(&p->lock);
  // 寻找一个空的 VMA
  for (int i = 0; i < MAX_VMA; i++) {
    if (p->vmas[i].valid == 0) {

      // 寻找虚拟内存，参考 sys_sbrk，这样其实有问题，因为如果有回收那块虚拟内存就再也不会利用
      // 但为了方便，就这样做了。这也是 xv6 的宗旨，为了方便牺牲性能。实际上可以用 bitmap 等进行虚拟空间管理
      uint64 va = PGROUNDUP(p->sz);
      // 物理地址，从文件指针开始找: file->inode->bread->bdata->phyaddr
      // 但先不需要了，因为可以映射一个假的物理地址，到时候缺页了，直接通过文件指针找
      uint64 pa = 0;

      // TODO: 如果 len 不是 PGSIZE 的整数倍？
      uint64 append_sz = len + len%PGSIZE;
      // 开始映射
      for (int j = 0; j < append_sz; j += PGSIZE) {
        // printf("mmap: va = %p, pa = %p, f = %p\n", (void*)(va+j), (void*)pa, p->ofile[fd]);

        // 注意这里不要根据 prot 设置 flags
        // 以 PROT_READ 为例，如果此时把 PTE_R 标记为 1，当程序想要访问这个页表，发现标记为是 PTE_V|PTE_R
        // 此时程序认为这个页表非常合理，所以 usertrap 就不是读取页错误了，而是正常访问物理地址，显然出错
        // 正确做法是：先不标记为 PTE_R，这样就会触发读取页错误（想要读取页，但页没有权限），此时在里面使用记录的 vmas[i].prot 来检测
        uint flags = PTE_V | PTE_U | PTE_M;

        if(mappages(p->pagetable, va+j, PGSIZE, pa, flags) != 0){
          release(&p->lock);
          return -1;
        }
      }
      p->sz += append_sz;

      p->vmas[i].valid = 1;
      p->vmas[i].prot = prot;
      p->vmas[i].flags = flags;
      p->vmas[i].f = p->ofile[fd];
      p->vmas[i].offset = offset;
      p->vmas[i].start = va;
      p->vmas[i].end = va + len;
      p->vmas[i].mapped_start = va;
      p->vmas[i].mapped_end = va + len;

      // 添加文件引用
      filedup(p->vmas[i].f);
      release(&p->lock);
      return va;
    }
  }
  release(&p->lock);
  return -1;
}

uint64
sys_munmap(void)
{
  uint64 start;
  size_t len;
  argaddr(0, &start);
  argaddr(1, &len);

  // // 临时调试：用户程序指向 munmap(0,0) 可以打印进程的虚拟地址及 flags 信息
  // if (start == 0 && len == 0) {
  //   struct proc *p = myproc();

  //   for(uint64 i = 0; i < p->sz; i += PGSIZE){
  //     pte_t *pte = walk(p->pagetable, i, 0);
  //     if(pte == 0)
  //       panic("uvmcopy: pte should exist");

  //     uint64 pa = PTE2PA(*pte);
  //     uint flags = PTE_FLAGS(*pte);

  //     printf("va = %p, pa = %p, ", (void*)i, (void*)pa);
  //     printf("PTE_V = %d, ", (flags & PTE_V) ? 1 : 0);
  //     printf("PTE_M = %d, ", (flags & PTE_M) ? 1 : 0);
  //     printf("PTE_W = %d, ", (flags & PTE_W) ? 1 : 0);
  //     printf("PTE_R = %d, ", (flags & PTE_R) ? 1 : 0);
  //     printf("PTE_U = %d\n", (flags & PTE_U) ? 1 : 0);
  //   }
  //   printf("----------------------------------\n\n");
  //   return 0;
  // }

  uint64 end = start + len;

  if (start % PGSIZE != 0 || end % PGSIZE != 0) {
    printf("munmap: start is not page-aligned, or end is not page-aligned\n");
    return -1;
  }

  struct proc* p = myproc();

  // 这里坑了很久，如果加锁，那么后面会 panic，为什么这里不应该 acquire ??
  // acquire(&p->lock);

  // 遍历 vmas，找到对应的 vma
  for (int i = 0; i < MAX_VMA; i++) {
    // printf("munmap: vmas[%d].valid = %d, vmas[%d].start = %p, vmas[%d].end = %p\n", i, p->vmas[i].valid, i, (void*)p->vmas[i].start, i, (void*)p->vmas[i].end);
    if (p->vmas[i].valid == 1 && p->vmas[i].start <= start && p->vmas[i].end >= end) {
      // 找到对应的 vma，遍历每一页，解除映射

      // 题目进行了精简，不可能有空洞，所以我们这里可以比较简单的调整参数
      if (p->vmas[i].mapped_start == start) {
        p->vmas[i].mapped_start = end;
      } else if (p->vmas[i].mapped_end == end) {
        p->vmas[i].mapped_end = start;
      }

      // printf("munmap: vmas[%d].mapped_start = %p, vmas[%d].mapped_end = %p\n", i, (void*)p->vmas[i].mapped_start, i, (void*)p->vmas[i].mapped_end);

      // 如果是 MAP_SHARED，并且有过改变，那么就写回文件
      if ((p->vmas[i].flags & MAP_SHARED)) {
        for (int j = start; j < end; j += PGSIZE) {
          uint64 va = j;
          pte_t *pte = walk(p->pagetable, va, 0);
          uint flags = PTE_FLAGS(*pte);

          if((pte==0) || (flags & PTE_V) == 0)
            panic("mmap_writeback: walk failed");

          // 写回这个文件
          if((flags & PTE_D)){ 
            // 如果 PTE_W 没有设置，则报错
            if (!(flags & PTE_W)) {
              panic("PTE_W is not set, but page is changed!");
            }

            // printf("PTE_D is set, write back to file\n");
            uint offset = va - p->vmas[i].start;
            if (file_from_pa(p->vmas[i].f, PTE2PA(*pte), offset, PGSIZE) == -1) {
            // if (file_from_pa(p->vmas[i].f, va, offset, PGSIZE) == -1) {
              panic("write back to file failed!");
            }
            // filewrite(p->vmas[i].f, va, PGSIZE);
          }
        }
      }

      // 这里本来直接调用 uvmunmap 的，但是后来发现：对于那些实际还没有申请的内存，释放他们就会有问题
      // uvmunmap(p->pagetable, start, len/PGSIZE, 1);
      for (int j = start; j < end; j += PGSIZE) {
        uint64 va = j;
        pte_t *pte = walk(p->pagetable, va, 0);
        uint flags = PTE_FLAGS(*pte);

        // 没有 PTE_M，才是实际申请的内存
        if ((flags & PTE_M) == 0) {
          uint64 pa = PTE2PA(*pte);
          kfree((void*)pa);
        }

        // 很重要：这里要标记为 PTE_M，然后 uvmcopy 和 uvmfree 会调整
        *pte = 0 | PTE_M;
      }

      // 如果解除了全部，需要减小对应文件的指针，而且把这个 valid 变为 0
      // if (p->vmas[i].mapped_start == start && p->vmas[i].mapped_end == end) {
      if(p->vmas[i].mapped_start == p->vmas[i].mapped_end) {
        // printf("munmap: vmas[%d].valid = 0\n", i);
        // printf("munmap: vmas[%d].mapped_start = %p, vmas[%d].mapped_end = %p\n", i, (void*)p->vmas[i].mapped_start, i, (void*)p->vmas[i].mapped_end);
        // printf("munmap: vmas[%d].start = %p, vmas[%d].end = %p\n", i, (void*)p->vmas[i].start, i, (void*)p->vmas[i].end);
        // printf("munmap: start = %p, end = %p\n", (void*)start, (void*)end);
        p->vmas[i].valid = 0;
        fileclose(p->vmas[i].f);
      }

      // release(&p->lock);
      return 0;
    }
  }

  // release(&p->lock);
  return -1;
}
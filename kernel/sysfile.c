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


#define min(a, b) ((a) < (b) ? (a) : (b))

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
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

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
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

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
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

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
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

// void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
uint64 sys_mmap(void){
  struct file* file;
  int len, fd;
  int prot, flags;
  struct proc *p = myproc();
  struct vma *vma = 0;

  if(argint(1, &len) < 0 || argint(2, &prot) < 0|| argint(3, &flags) < 0|| argfd(4, &fd, &file) < 0){
    return -1;
  }

  if((prot & PROT_WRITE) && !file->writable && (flags == MAP_SHARED)){
    // panic("Wrting an readonly file on disk.");
    return -1;
  }

  // Find a valid (idle) vma.
  for(int i = 0; i < NUMVMA; i++){
    if(p->vmas[i].valid){
      vma = &p->vmas[i];
      break;
    }
  }
  if(vma == 0){
    panic("No Valid VMA");
    return -1;
  }
  // Reference Increment
  file = filedup(file);

  vma->valid = 0;
  vma->file = file;
  vma->prot = prot;
  vma->flags = flags;
  vma->addr = PGROUNDUP(p->sz);
  vma->len = len;
  vma->end = PGROUNDUP(vma->addr+len);    // PGSIZE Alignment
  // printf("%d ", p->sz);
  p->sz = vma->end;
  // printf("%d\n", p->sz);

  return vma->addr;
}

struct vma *findvma(uint64 addr){
  struct proc *p = myproc();
  for(int i = 0; i < NUMVMA; i++){
    if(!p->vmas[i].valid && p->vmas[i].addr <= addr && p->vmas[i].end > addr){
      return &p->vmas[i];
    }
  }
  return 0;
}

int mmaphandler(uint64 addr){
  struct vma *vma = findvma(addr);
  if(vma == 0){   // Page fault not caused by mmap
    // panic("Unexpected page fault!");
    return -1;
  }
  uint64 mem = (uint64)kalloc();
  if(mem == 0){
    panic("No memory available for mmap kalloc.");
  }
  memset((void *)mem, 0, PGSIZE);

  struct inode *ip = vma->file->ip;
  uint64 offset = PGROUNDDOWN(addr-vma->addr);   // PGSIZE Alignment
  int perm = PTE_U;
  if(vma->prot | PROT_READ) perm |= PTE_R;
  if(vma->prot | PROT_WRITE) perm |= PTE_W;

  begin_op();
  ilock(ip);
  readi(ip, 0, mem, offset, min(PGSIZE, vma->len-offset));
  iunlock(ip);
  end_op();
  
  struct proc *p = myproc();
  if(mappages(p->pagetable, vma->addr+offset, PGSIZE, mem, perm) < 0){
    panic("Map vma failed.");
    return -1;
  }
  return 0;
}

uint64 sys_munmap(void){
  uint64 addr;
  int len;
  if(argaddr(0, &addr) < 0 || argint(1, &len) < 0) return -1;

  struct vma *vma = findvma(addr);
  if(vma == 0){
    panic("Cannot find vma corresponding to the address.");
    return -1;
  }

  uint64 start = PGROUNDUP(addr), end = PGROUNDDOWN(addr+len);
  int npages = (end-start)/PGSIZE;
  struct proc *p = myproc();
  pte_t *pte;
  struct inode *ip = vma->file->ip;
  if(vma->flags & MAP_SHARED){
    begin_op();
    ilock(ip);
    for(int i = PGROUNDDOWN(addr); i < PGROUNDUP(addr+len); i += PGSIZE){
      pte = walk(p->pagetable, i, 0);
      if(*pte & PTE_D){    // Dirty page
        writei(ip, 1, i, i-vma->addr, PGSIZE);
      }
    }
    iunlock(ip);
    end_op();
  }
  
  // Set the unfreed (still mapped because of part of the block is not munmapped) part to 0
  int offset = addr-PGROUNDDOWN(addr);
  char *mem = (char *)walkaddr(p->pagetable, PGROUNDDOWN(addr));
  memset(mem+offset, 0, start-addr);

  mem = (char *)walkaddr(p->pagetable, end);
  memset(mem, 0, addr+len-end);

  uvmunmap(p->pagetable, start, npages, 1);

  if(addr == vma->addr && len == vma->len){     // munmap the whole block
    fileclose(vma->file);       // Decrement file ref count.
    vma->valid = 1;             // Make the vma valid for new mmaps.
  }
  else if(addr == vma->addr){                   // munmap from the start
    vma->addr = end;
    vma->len -= len;
  }
  else if(addr+len == vma->addr+vma->len){      // munmap from tail
    vma->end = PGROUNDUP(addr);
    vma->len -= len;
  }
  else{
    panic("munmap in the middle!");
    return -1;
  }
  
  return 0;
}

void freeallvma(){
  struct proc *p = myproc();
  struct vma *vma;
  struct inode *ip;
  pte_t *pte;
  for(int i = 0; i < NUMVMA; i++){
    vma = &p->vmas[i];
    if(vma->valid) continue;
    ip = vma->file->ip;
    if(vma->flags & MAP_SHARED){
      begin_op();
      ilock(ip);
      for(int i = vma->addr; i < vma->end; i += PGSIZE){
        pte = walk(p->pagetable, i, 0);
        if(*pte & PTE_D){    // Dirty page
          writei(ip, 1, i, i-vma->addr, PGSIZE);
        }
      }
      iunlock(ip);
      end_op();
    }
    fileclose(vma->file);
    vma->valid = 1;
  }
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
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
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

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
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

  if(argaddr(0, &fdarray) < 0)
    return -1;
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
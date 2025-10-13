#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"




uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


//Exp3   sysproc.c
uint64
sys_interpose(void)
{
    int mask;
    uint64 path; // 未使用参数
    argint(0, &mask);
    argaddr(1, &path);
    struct proc *p = myproc();
    p->sandbox_mask = mask;
    printf("Process %d sandbox set: mask = %d\n", p->pid, mask);
    return 0;
}



// ---------------------------------------------------
// sysinfo 系统调用实现
// ---------------------------------------------------

// 结构体定义（内核中也定义一份，和 user.h 中一致）
struct sysinfo {
  uint64 freemem;   // 空闲字节数
  uint nproc;       // 进程数
};

// 手动声明 kmem 结构体（来自 kalloc.c）
struct run {
  struct run *next;
};
extern struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// === 手动声明来自 proc.c 的进程表 ===
extern struct proc proc[NPROC];

uint64
sys_sysinfo(void)
{
  uint64 user_dst;            // 用户传入的 sysinfo 结构地址
  struct sysinfo info;        // 内核中临时结构

  // ------------------ 1. 解析参数 ------------------
  argaddr(0, &user_dst);

  // ------------------ 2. 统计空闲内存 ------------------
  info.freemem = 0;
  struct run *r;
  acquire(&kmem.lock);
  for (r = kmem.freelist; r; r = r->next)
    info.freemem += PGSIZE;
  release(&kmem.lock);

  // ------------------ 3. 统计进程数 ------------------
  info.nproc = 0;
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state != UNUSED)
      info.nproc++;
  }

  // ------------------ 4. 拷贝回用户空间 ------------------
  if (copyout(myproc()->pagetable, user_dst, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}


uint64
sys_trace(void)
{
  int mask;
  // 解析第一个参数
  argint(0, &mask);
  // 设置当前进程的 trace mask
  myproc()->trace_mask = mask;
  return 0;
}


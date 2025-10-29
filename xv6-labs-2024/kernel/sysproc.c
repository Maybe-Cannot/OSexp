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
  // ---- 读者进入区 ----
  sem_P(&rw.r);
  sem_P(&rw.mutexReadCount);
  rw.readcount++;
  if (rw.readcount == 1)
    sem_P(&rw.w);   // 第一个读者阻止写者
  sem_V(&rw.mutexReadCount);
  sem_V(&rw.r);

  // ---- 临界区（读取 ticks） ----
  xticks = ticks;

  // ---- 读者退出区 ----
  sem_P(&rw.mutexReadCount);
  rw.readcount--;
  if (rw.readcount == 0)
    sem_V(&rw.w);   // 最后一个读者唤醒写者
  sem_V(&rw.mutexReadCount);
  return xticks;
}


uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
   // 使用读锁读取当前 ticks
  sem_P(&rw.r);
  sem_P(&rw.mutexReadCount);
  rw.readcount++;
  if (rw.readcount == 1)
    sem_P(&rw.w);
  sem_V(&rw.mutexReadCount);
  sem_V(&rw.r);

  ticks0 = ticks; // 读一次

  // 释放读锁
  sem_P(&rw.mutexReadCount);
  rw.readcount--;
  if (rw.readcount == 0)
    sem_V(&rw.w);
  sem_V(&rw.mutexReadCount);

  // 然后用原有 tickslock 进入 sleep 等待被唤醒
  acquire(&tickslock);
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
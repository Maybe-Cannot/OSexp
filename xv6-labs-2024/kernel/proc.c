#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// MLFQ 调度器相关数据结构
#define NQUEUE 3
struct proc_queue {
  struct proc *procs[NPROC];
  int head, tail;
  int size;
};

struct proc_queue mlfq[NQUEUE];
struct spinlock mlfq_lock;

// CPU 调度统计 (用于观察锁竞争分布)
uint64 cpu_schedule_count[NCPU];  // 每个 CPU 的调度次数

// 全局协调机制
int current_priority_level = -1;     // 当前时间片允许运行的优先级 (-1表示可以选择新优先级)
int running_procs_count = 0;        // 当前优先级正在运行的进程数
int pending_requeue_count = 0;      // 等待重新入队的进程数
struct proc *pending_requeue[NCPU]; // 等待重新入队的进程列表
struct spinlock global_coord_lock;  // 全局协调锁

// 各队列的时间片配置
// Q0, Q1: FCFS with time slices 1,2
// Q2: Round Robin with time slice 4
int time_slice[NQUEUE] = {1, 2, 4};

// MLFQ 队列操作函数声明
void enqueue(struct proc_queue *q, struct proc *p);
struct proc* dequeue(struct proc_queue *q);
int queue_empty(struct proc_queue *q);
int remove_from_queue(struct proc_queue *q, struct proc *target);
void init_mlfq(void);
void check_aging(void);

// 新增全局变量声明
extern int current_priority_level;
extern int running_procs_count;
extern int pending_requeue_count;
extern struct proc *pending_requeue[NCPU];
extern struct spinlock global_coord_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&mlfq_lock, "mlfq");
  initlock(&global_coord_lock, "global_coord");
  
  // 初始化 MLFQ 队列
  init_mlfq();
  
  // 初始化 CPU 统计
  for(int i = 0; i < NCPU; i++) {
    cpu_schedule_count[i] = 0;
  }
  
  // 初始化全局协调变量
  current_priority_level = -1;
  running_procs_count = 0;
  pending_requeue_count = 0;
  
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
      // 初始化 MLFQ 相关字段
      p->qlevel = 0;
      p->ticks_used = 0;
      p->last_run = 0;
      p->in_mlfq_queue = 0;
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // 先获取全局锁，再设置状态和入队
  acquire(&mlfq_lock);
  p->state = RUNNABLE;
  // 第一个用户进程加入最高优先级队列 Q0
  enqueue(&mlfq[0], p);
  release(&mlfq_lock);

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  // 先获取全局锁，再获取进程锁
  acquire(&mlfq_lock);
  acquire(&np->lock);
  
  np->state = RUNNABLE;
  // 新进程加入最高优先级队列 Q0
  enqueue(&mlfq[0], np);
  
  release(&np->lock);
  release(&mlfq_lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}


// 每个 CPU 的进程调度器。
// 每个 CPU 在完成自身设置后调用 scheduler()。
// scheduler 永不返回。它循环执行以下操作：
//  - 选择一个进程运行。
//  - 通过 swtch 切换到该进程开始运行。
//  - 最终该进程通过 swtch 将控制权交还给调度器。
void
scheduler(void)
{
  // 注意：不再需要局部变量 p，避免未使用警告
  struct cpu *c = mycpu();
  extern uint ticks; // 引用全局时钟计数用于 last_run 记录

  c->proc = 0;
  for(;;){
    // 最近运行的进程可能已经关闭了中断；
    // 重新打开中断以避免所有进程都在等待时发生死锁。
    intr_on();

    acquire(&global_coord_lock);
    acquire(&mlfq_lock);
    
    // 统计当前 CPU 获得锁的次数
    cpu_schedule_count[cpuid()]++;
    
    // 定期检查老化机制
    check_aging();
    
    struct proc *selected = 0;
    
    // 如果当前没有指定优先级，或者该优先级已无进程运行，选择新优先级
    if(current_priority_level == -1 || running_procs_count == 0) {
      // 批量处理等待重新入队的进程（保持顺序）
      if(pending_requeue_count > 0) {
        for(int i = 0; i < pending_requeue_count; i++) {
          struct proc *p = pending_requeue[i];
          if(p->ticks_used >= time_slice[p->qlevel]) {
            // 时间片用完 - 降级到下一级队列队尾
            if(p->qlevel < NQUEUE - 1) {
              p->qlevel++;
            }
            p->ticks_used = 0;
          }
          enqueue(&mlfq[p->qlevel], p);  // 按原顺序重新入队
        }
        pending_requeue_count = 0;
      }
      
      // 选择新的优先级
      for(int lvl = 0; lvl < NQUEUE; lvl++) {
        if(!queue_empty(&mlfq[lvl])) {
          current_priority_level = lvl;
          break;
        }
      }
    }
    
    // 从当前优先级队列中取进程
    if(current_priority_level != -1) {
      if(!queue_empty(&mlfq[current_priority_level])) {
        // 该优先级还有进程，可以调度
        selected = dequeue(&mlfq[current_priority_level]);
        if(selected) {
          acquire(&selected->lock);
          if(selected->state == RUNNABLE) {
            selected->state = RUNNING;
            running_procs_count++;  // 增加正在运行的进程计数
            // 注意：不要在这里释放 selected->lock，
            // 必须按照 xv6 约定在 swtch 前保持 p->lock 持有，
            // 以匹配 forkret()/sched() 的锁语义。
          } else {
            release(&selected->lock);
            selected = 0;
          }
        }
      } else if(running_procs_count > 0) {
        // 该优先级队列为空，但还有进程在运行
        // 当前CPU必须等待，不能调度其他优先级的进程
        selected = 0;  // 保持空闲状态
      }
    }
    
    release(&mlfq_lock);
    release(&global_coord_lock);

    if(selected) {
      // 进程已经在上面被标记为 RUNNING，直接运行
      selected->last_run = ticks;
      c->proc = selected;
      
      swtch(&c->context, &selected->context);
      
      // 进程返回后，处理重新入队
      c->proc = 0;
      
      // 此时仍然持有 selected->lock（遵循 xv6 语义），无需再次 acquire
      if(selected->state == RUNNABLE) {
        // 将进程加入等待重新入队的列表，而不是立即入队
        acquire(&global_coord_lock);
        pending_requeue[pending_requeue_count++] = selected;
        running_procs_count--;  // 减少正在运行的进程计数
        
        // 如果这是该优先级的最后一个进程，重置当前优先级
        if(running_procs_count == 0) {
          current_priority_level = -1;  // 允许下次选择新优先级
        }
        release(&global_coord_lock);
      } else {
        // 进程结束或进入睡眠状态
        acquire(&global_coord_lock);
        running_procs_count--;
        if(running_procs_count == 0) {
          current_priority_level = -1;
        }
        release(&global_coord_lock);
      }
      release(&selected->lock);
    }
    
    if(!selected) {
      // 没有可运行的进程，等待中断
      intr_on();
      asm volatile("wfi");
    }
  }
}

// 切换到调度器。此时只能持有 p->lock，并且已修改 proc->state。
// 保存和恢复 intena，因为 intena 是该内核线程的属性，而不是 CPU 的属性。
// 理论上应该用 proc->intena 和 proc->noff，但在某些持锁但没有进程的场景下会出错。
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  
  // 时间片中断时，增加时间片计数
  p->ticks_used++;
  p->state = RUNNABLE;
  
  // 注意：不在这里入队，由调度器统一处理重新入队逻辑
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;
  struct proc *to_wakeup[NPROC];
  int wakeup_count = 0;

  // 第一阶段：收集需要唤醒的进程
  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        to_wakeup[wakeup_count++] = p;
      }
      release(&p->lock);
    }
  }

  // 第二阶段：按正确的锁顺序将进程加入队列
  if(wakeup_count > 0) {
    acquire(&mlfq_lock);
    for(int i = 0; i < wakeup_count; i++) {
      p = to_wakeup[i];
      acquire(&p->lock);
      if(p->state == RUNNABLE && !p->in_mlfq_queue) {
        enqueue(&mlfq[p->qlevel], p);
      }
      release(&p->lock);
    }
    release(&mlfq_lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// MLFQ 队列操作函数实现

// 初始化所有 MLFQ 队列
void
init_mlfq(void)
{
  for(int i = 0; i < NQUEUE; i++) {
    mlfq[i].head = 0;
    mlfq[i].tail = 0;
    mlfq[i].size = 0;
  }
}

// 将进程加入队列尾部
void
enqueue(struct proc_queue *q, struct proc *p)
{
  if(q->size >= NPROC) {
    panic("queue overflow");
  }
  if(p->in_mlfq_queue) {
    return; // 已在队列中，避免重复入队
  }
  q->procs[q->tail] = p;
  q->tail = (q->tail + 1) % NPROC;
  q->size++;
  p->in_mlfq_queue = 1;
}

// 从队列头部取出进程
struct proc*
dequeue(struct proc_queue *q)
{
  if(q->size == 0) {
    return 0;
  }
  struct proc *p = q->procs[q->head];
  q->head = (q->head + 1) % NPROC;
  q->size--;
  p->in_mlfq_queue = 0;
  return p;
}

// 检查队列是否为空
int
queue_empty(struct proc_queue *q)
{
  return q->size == 0;
}

// 从队列中移除特定进程
int
remove_from_queue(struct proc_queue *q, struct proc *target)
{
  for(int i = 0; i < q->size; i++) {
    int idx = (q->head + i) % NPROC;
    if(q->procs[idx] == target) {
      // 找到目标进程，将后面的元素前移
      for(int j = i; j < q->size - 1; j++) {
        int curr_idx = (q->head + j) % NPROC;
        int next_idx = (q->head + j + 1) % NPROC;
        q->procs[curr_idx] = q->procs[next_idx];
      }
      q->size--;
      q->tail = (q->tail - 1 + NPROC) % NPROC;
      target->in_mlfq_queue = 0;
      return 1; // 成功移除
    }
  }
  return 0; // 未找到
}

// 检查老化机制，将在 Q2 中等待过久的进程提升到 Q0
void
check_aging(void)
{
  extern uint ticks;
  struct proc *p;
  
  // 临时数组存储需要提升的进程
  struct proc *to_promote[NPROC];
  int promote_count = 0;
  
  // 检查 Q2 中的进程
  for(int i = 0; i < mlfq[2].size; i++) {
    int idx = (mlfq[2].head + i) % NPROC;
    p = mlfq[2].procs[idx];
    
    // 检查是否超过10个时间片未被服务
    if(p->state == RUNNABLE && (ticks - p->last_run >= 10)) {
      to_promote[promote_count++] = p;
    }
  }
  
  // 执行提升操作
  for(int i = 0; i < promote_count; i++) {
    p = to_promote[i];
    
    // 从 Q2 中移除
    if(remove_from_queue(&mlfq[2], p)) {
      // 提升到 Q0 队尾
      p->qlevel = 0;
      p->ticks_used = 0;
      enqueue(&mlfq[0], p);
    }
  }
}

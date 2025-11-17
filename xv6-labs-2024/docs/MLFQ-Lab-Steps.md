# xv6 全局 MLFQ 实验步骤（实现与验证指南）

本实验旨在在 xv6 上实现一个全局多级反馈队列（MLFQ）调度器，并满足多核环境下“单一时间片仅运行一个优先级”和“多核批量按原顺序回队”的约束。我们将以连贯的叙述，从整体设计到关键实现细节展开，同时在讲解中穿插展示关联代码，帮助你在源码层面把握实现脉络与可验证的关键点。

为获得完整背景、改动面与对照代码，可参阅同目录中的《MLFQ-Design.md》。本文将以实现流程为主线，强调“为什么这样做”与“如何保证并发正确性”。

---

从全局设计出发，我们首先确立调度器遵循三层队列结构 Q0/Q1/Q2，并统一由全局 mlfq_lock 管理，所有 CPU 共享同一套队列，从而在宏观上形成一致的优先级视图。每一层分配固定时间片 {1,2,4}，短作业倾向于在高优先级队列获得更多抢占机会，而长作业随时间片耗尽向低优先级队列沉降；为避免饥饿，我们对最低优先级队列引入基于全局 ticks 的老化提升策略。此外，为满足“同一时间片只运行一种优先级”的强约束，我们引入一个全局的 current_priority_level 与 running_procs_count，配合 pending_requeue 缓冲区，使得多核在同一轮次结束后统一批量回队，保持同优先级进程的相对顺序稳定。

```c
// proc.c 片段：全局结构与时间片配置
#define NQUEUE 3
struct proc_queue {
  struct proc *procs[NPROC];
  int head, tail;
  int size;
};

struct proc_queue mlfq[NQUEUE];
struct spinlock mlfq_lock;           // 队列全局锁
int time_slice[NQUEUE] = {1, 2, 4};   // Q0/Q1/Q2 时间片

// 全局协调：同一时间片仅运行一个优先级 + 批量回队
int current_priority_level;           // -1 表示未选定
int running_procs_count;              // 当前优先级正在运行的进程数
struct proc *pending_requeue[NCPU];   // 批量回队缓冲
int pending_requeue_count;
struct spinlock global_coord_lock;    // 与 mlfq_lock 配合
```

在进程控制块层面，我们为每个进程附加 MLFQ 元信息，包括所处队列层级 qlevel、在该层级内已消耗时间片 ticks_used、最近一次实际运行时刻 last_run 以及避免重复入队的 in_mlfq_queue 标志。这些字段确保调度器能够精确地进行时间片统计、层级降级与老化提升，并在多核条件下维持状态一致性。

```c
// proc.h 片段：struct proc 增补的 MLFQ 元数据
// MLFQ 调度相关字段
int qlevel;                  // 当前所在队列层级 (0, 1, 2)
int ticks_used;              // 当前队列内已使用时间片
uint last_run;               // 上次运行时的全局 ticks
int in_mlfq_queue;           // 是否已在 MLFQ 队列中 (避免重复入队)
```

为了保证从系统启动到第一个用户进程以及随后的子进程都能够以一致的策略进入调度域，我们在 procinit、userinit 与 fork 等生命周期关键路径上进行初始化与入队处理。在 procinit 中设置全局锁、初始化队列、清零各类统计变量，并为每个进程初始化 MLFQ 字段；在 userinit 与 fork 中，统一采用“先获取 mlfq_lock，再获取 p->lock”的锁序，将新就绪的进程放入 Q0 队尾，以确保从系统第一批可运行进程开始，队列顺序与全局约束就得到遵守。

```c
// proc.c 片段：procinit 初始化全局结构与每进程字段
void
procinit(void)
{
  struct proc *p;
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&mlfq_lock, "mlfq");
  initlock(&global_coord_lock, "global_coord");

  init_mlfq();
  for(int i = 0; i < NCPU; i++) cpu_schedule_count[i] = 0;
  current_priority_level = -1;
  running_procs_count = 0;
  pending_requeue_count = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int) (p - proc));
    p->qlevel = 0;
    p->ticks_used = 0;
    p->last_run = 0;
    p->in_mlfq_queue = 0;
  }
}
```

在用户进程初始化与进程复制路径上，我们将新产生、即将进入 RUNNABLE 状态的进程放入 Q0 的队尾。这里遵循固定的锁序 mlfq_lock → p->lock，以避免与调度器或唤醒路径上的锁顺序产生反转，从而消除潜在的死锁和顺序竞争。在 userinit 的末尾，我们在持有 mlfq_lock 的情况下入队；在 fork 的返回路径，我们同样在设置 RUNNABLE 后立即将子进程入 Q0。

```c
// userinit 末尾片段：首个用户进程入 Q0 队尾
acquire(&mlfq_lock);
p->state = RUNNABLE;
enqueue(&mlfq[0], p);
release(&mlfq_lock);
```

```c
// fork 末尾片段：子进程入 Q0 队尾（锁序：mlfq_lock → np->lock）
acquire(&mlfq_lock);
acquire(&np->lock);
np->state = RUNNABLE;
enqueue(&mlfq[0], np);
release(&np->lock);
release(&mlfq_lock);
```

时钟中断是时间片的唯一权威来源，因此我们将时间片统计的递增放置在 yield 中，这样每次由时钟中断驱动的抢占都会使当前进程的 ticks_used 精确反映真实运行时长。与之相配合，我们不在 yield 中直接进行重新入队，而是将回队动作延后到调度器返回路径，通过 pending_requeue 实现同一优先级批量回队的顺序稳定性。

```c
// proc.c：yield 将计时放在时钟驱动的让出点，回队延后到调度器统一处理
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->ticks_used++;
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}
```

调度循环是实现整体策略与约束的核心所在。每次进入调度器，我们先开启中断以避免系统进入全体睡眠的死锁情形，然后进入全局协调与队列的临界区。若当前还未确定系统本轮次允许运行的优先级，或者该优先级在当前时刻已经没有运行中的进程，我们就先处理上一个轮次积累的 pending_requeue：对于耗尽时间片的进程进行降级并清零时间片计数，否则维持原级别，最后按 pending 的顺序统一入队。只有在完成这一步之后，我们才从 Q0 到 Q2 重新选择新的 current_priority_level。随后的取进程阶段严格只从当前选定优先级的队列中出队；如果队列已经被取空但仍有同级进程在其他 CPU 上运行，我们不跨级寻找工作，而是让当前 CPU 进入等待，确保“同一时间片仅运行一个优先级”的系统不变量。取到进程后，立即在其自旋锁保护下将其状态设置为 RUNNING 并递增 running_procs_count，然后释放全局锁并切换到进程上下文。待该进程在时钟驱动下让出后回到调度器，我们检查其状态，若仍为 RUNNABLE，则将其放入 pending_requeue（此时不入队，保持批量回队的顺序语义）；若不是 RUNNABLE，则仅减少计数。当 running_procs_count 归零时，重置 current_priority_level，使系统得以在下一轮重新选择优先级。

```c
// proc.c：scheduler 的关键逻辑（节选，去除与讲解无关的细节）
void
scheduler(void)
{
  struct proc *selected;
  struct cpu *c = mycpu();
  extern uint ticks;

  c->proc = 0;
  for(;;){
    intr_on();

    acquire(&global_coord_lock);
    acquire(&mlfq_lock);

    check_aging();

    if(current_priority_level == -1 || running_procs_count == 0) {
      if(pending_requeue_count > 0) {
        for(int i = 0; i < pending_requeue_count; i++) {
          struct proc *p = pending_requeue[i];
          if(p->ticks_used >= time_slice[p->qlevel]) {
            if(p->qlevel < NQUEUE - 1)
              p->qlevel++;
            p->ticks_used = 0;
          }
          enqueue(&mlfq[p->qlevel], p);
        }
        pending_requeue_count = 0;
      }
      for(int lvl = 0; lvl < NQUEUE; lvl++) {
        if(!queue_empty(&mlfq[lvl])) { current_priority_level = lvl; break; }
      }
    }

    selected = 0;
    if(current_priority_level != -1) {
      if(!queue_empty(&mlfq[current_priority_level])) {
        selected = dequeue(&mlfq[current_priority_level]);
        if(selected) {
          acquire(&selected->lock);
          if(selected->state == RUNNABLE) {
            selected->state = RUNNING;
            running_procs_count++;
            release(&selected->lock);
          } else {
            release(&selected->lock);
            selected = 0;
          }
        }
      }
    }

    release(&mlfq_lock);
    release(&global_coord_lock);

    if(selected) {
      selected->last_run = ticks;
      c->proc = selected;
      swtch(&c->context, &selected->context);
      c->proc = 0;

      acquire(&selected->lock);
      if(selected->state == RUNNABLE) {
        acquire(&global_coord_lock);
        pending_requeue[pending_requeue_count++] = selected;
        running_procs_count--;
        if(running_procs_count == 0)
          current_priority_level = -1;
        release(&global_coord_lock);
      } else {
        acquire(&global_coord_lock);
        running_procs_count--;
        if(running_procs_count == 0)
          current_priority_level = -1;
        release(&global_coord_lock);
      }
      release(&selected->lock);
    } else {
      intr_on();
      asm volatile("wfi");
    }
  }
}
```

值得注意的是，唤醒路径需要在保持锁序正确的情况下批量地将进程放回其对应的队列，以免与调度器争抢同一资源并破坏 FIFO 顺序。我们使用“两阶段”策略：遍历进程数组时仅在持有 p->lock 的情况下将满足条件的进程标记为 RUNNABLE 并收集；随后在持有 mlfq_lock 的临界区内按收集的顺序统一入队。这样的处理既保持了锁序的单调性，也让同一批次的唤醒具有顺序稳定性。

```c
// proc.c：wakeup 的两阶段批量入队
void
wakeup(void *chan)
{
  struct proc *p;
  struct proc *to_wakeup[NPROC];
  int wakeup_count = 0;

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
```

当上述所有逻辑完成后，实验的实现部分基本成型，接下来需要进行系统性的构建与验证。首先进行常规的编译与启动，确认系统可以正常引导至 shell。在此基础上，我们通过精心设计的进程组合来验证全局约束与行为特征：例如在多核环境下制造“同级进程数量小于 CPU 数量”的场景，观察空闲 CPU 是否确实等待而非跨级运行；再例如在运行过程中引入新的高优先级作业，验证其不会抢占当前时间片而是在下一轮调度时统一执行；最后在 Q2 中放置持续运行的长作业，观察其在经历足够的等待后被老化提升至 Q0。结合 ^P 触发的 procdump 输出与自定义的统计变量（如 cpu_schedule_count），我们可以对调度器行为进行定量与定性的双重判定。

```text
可选验证步骤（命令供参考）：
- 构建并运行：make; make qemu
- 自测用例：usertests; stressfs
- 调试观察：运行过程中按 ^P 查看 procdump，关注 RUNNABLE/RUNNING 的分布与切换节奏
```

至此，我们完成了从整体设计到关键路径实现的连贯说明与代码穿插。若希望进一步扩展实验，例如将 pending_requeue 扩展到支持超过 NCPU 的并发批量、在 procdump 中打印 MLFQ 元信息，或者引入更加精细的老化策略与队列锁粒度，都可以在不破坏“单一时间片单优先级”的全局约束前提下循序推进。本文档的叙述与代码片段与《MLFQ-Design.md》保持一致，你可以在两者间交叉阅读以加深理解与验证实现。
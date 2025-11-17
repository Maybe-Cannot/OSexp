# xv6 全局 MLFQ 调度实现与改动说明

本文件总结本次在 xv6 上实现“全局多级反馈队列（MLFQ）调度器”的主要改动：改了哪些代码、为什么要改、设计取舍与并发考虑，以及如何验证。

## 背景与目标

在不大改 xv6 原框架的前提下实现全局 MLFQ 调度，满足：
- 三层队列（Q0/Q1/Q2），时间片分别为 1/2/4；
- 抢占、时间片耗尽降级、低级队列老化提升；
- 多 CPU 共享同一套全局队列；
- 所有计时基于全局 ticks；
- 约束扩展：
  1) 同一时间片内，只允许同一优先级的进程在各 CPU 上运行；
  2) 多 CPU 同时运行同一优先级进程后，统一批量“按原顺序”重新入队（保持顺序一致）。

---

## 代码改动总览（What changed）

### 1) `kernel/proc.h`
新增每个进程的 MLFQ 元数据：
- `int qlevel;` 当前所在队列层级（0/1/2）。
- `int ticks_used;` 当前层级下已消费的时间片数（由时钟中断驱动，在 `yield()` 中递增）。
- `uint last_run;` 最近一次实际运行时的全局 ticks（用于老化判断）。
- `int in_mlfq_queue;` 是否已在任一 MLFQ 队列中，避免重复入队。

理由：为调度器提供必要的进程级状态，支持时间片、降级与老化。

---

### 2) `kernel/proc.c`

新增全局数据结构与工具函数：
- 队列与配置
  - `#define NQUEUE 3`
  - `struct proc_queue { struct proc *procs[NPROC]; int head, tail, size; };`
  - `struct proc_queue mlfq[NQUEUE];` 全局三层队列。
  - `struct spinlock mlfq_lock;` 保护所有队列的全局自旋锁。
  - `int time_slice[NQUEUE] = {1, 2, 4};` 三层队列时间片配置。
- 队列操作
  - `void enqueue(...)` 入队（尾插），并设置 `in_mlfq_queue=1`。
  - `struct proc* dequeue(...)` 出队（头取），并清 `in_mlfq_queue=0`。
  - `int queue_empty(...)` 判空。
  - `int remove_from_queue(...)` 从队列中移除指定进程（老化提升时使用）。
  - `void init_mlfq(void)` 初始化各队列。
  - `void check_aging(void)` 老化检测：Q2 中 `RUNNABLE && ticks - last_run >= 10` 的进程提升到 Q0 队尾、重置统计。

理由：构建最小而完整的队列抽象，便于集中维护并发与顺序语义。

新增多 CPU 全局协调（满足“同一时间片仅同一优先级在运行”“批量按顺序归队”）：
- 统计与协调变量
  - `uint64 cpu_schedule_count[NCPU];`（可选）统计各 CPU 获得调度机会次数。
  - `int current_priority_level;` 当前整个系统“本时间片允许运行的优先级”，-1 表示未选定。
  - `int running_procs_count;` 当前优先级正在运行的进程数（跨 CPU 计数）。
  - `int pending_requeue_count; struct proc *pending_requeue[NCPU];` 批量重新入队的暂存区（保持顺序）。
  - `struct spinlock global_coord_lock;` 全局协调锁，配合 `mlfq_lock` 使用。

理由：
- 保证“同一时间片内只运行一种优先级”的系统级约束；
- 让多 CPU 在一个优先级耗尽后，**统一**批量将该批次进程按“完成顺序 → 入队顺序”放回，维持队列顺序稳定。

对关键函数的修改：
- `procinit()`：初始化 `mlfq_lock`、`global_coord_lock`、`mlfq` 队列、各 CPU 统计，设置每个 `proc` 的 MLFQ 字段初值。
- `userinit()` 与 `fork()`：
  - 设置 `RUNNABLE` 时，遵循锁顺序“先全局（mlfq_lock），后进程（p->lock）”，并将新进程入 Q0 队尾。
  - 理由：统一的锁顺序可避免死锁，且新进程优先从高优先级开始。
- `wakeup()`：分两阶段处理（先收集、后批量入队），并统一在 `mlfq_lock` 保护下将 `RUNNABLE` 的进程按当前 `qlevel` 入队；保持锁顺序要求。
- `yield()`：将时间片计数递增移动到这里（由时钟中断驱动调用 `yield()`），保证时间片统计反映“真实运行时间”而非“被调度次数”。不直接入队，改由调度器返回后统一处理。
- `scheduler()`：
  - 选择逻辑：
    1) 进入调度循环后加 `global_coord_lock` 与 `mlfq_lock`；
    2) 若 `current_priority_level==-1` 或该级别 `running_procs_count==0`，先批量处理 `pending_requeue[]`（保持顺序），然后从 `Q0→Q1→Q2` 选择本周期的 `current_priority_level`；
    3) 仅从 `current_priority_level` 对应队列取进程；若队列空但仍有本级别进程在运行，则当前 CPU 空转等待（不去拿更低/更高优先级），满足“同一时间片单一优先级”。
  - 运行与返回：
    - 取到进程后立即锁住进程并标记为 `RUNNING`，递增 `running_procs_count`，然后释放队列锁再 `swtch()`；
    - 进程返回调度器时：
      * 若仍为 `RUNNABLE`，放入 `pending_requeue[]`，不立即入队（等待本优先级所有 CPU 跑完这一轮后“批量按顺序”统一入队）；
      * 若不再 `RUNNABLE`（如睡眠/退出），仅 `running_procs_count--`；
      * 当 `running_procs_count` 归零时，重置 `current_priority_level=-1`，下一轮可选择新的优先级。
  - 重新入队策略（在下一次选择优先级前的批量阶段执行）：
    - 若 `ticks_used >= time_slice[qlevel]` 则降级（不低于 Q2），清零 `ticks_used`；
    - 否则保留原层级；
    - 最终按 `pending_requeue[]` 的顺序逐个 `enqueue()`，实现顺序稳定。

理由：
- 避免多个 CPU 同时调度到同一进程（出队后立刻加 `p->lock` 并置为 `RUNNING`）。
- 避免“同一时间片混跑不同优先级”的违规行为。
- 批量重排保证同优先级内的相对顺序稳定（A/B/C 在多 CPU 上运行后回到队列时，顺序仍然是 A/B/C）。

---

### 3) 注释与小改动
- 将部分英文注释改为中文，便于阅读与课程实验记录。
- `kernel/param.h`：保留配置，仅微调注释（不影响功能）。

---

## 设计理由（Why changed）

1) 全局队列 + 单一 `mlfq_lock`
- 简化实现，保证多核共享统一的优先级视图；
- 全局时间与全局优先级有利于可解释性与实验性；
- 代价是锁竞争偏大，但易于验证正确性。

2) 锁顺序：先全局、后进程（`mlfq_lock` → `p->lock`）
- 在 `fork()`、`userinit()`、`wakeup()`、`scheduler()` 中统一遵循该顺序，避免死锁；
- 进程状态修改与队列操作分离，确保原子性与可推理性。

3) 时间片统计在 `yield()` 中递增
- xv6 的时钟中断会触发 `yield()`，以此为“真实时间片刻度”；
- 避免把“被调度次数”误当作“运行时长”。

4) 老化（aging）
- 仅对 Q2 生效，`ticks - last_run >= 10` 时提升到 Q0；
- 防止长期饥饿，结合全局时间保证多核一致性。

5) 多 CPU 约束的全局协调
- 通过 `current_priority_level`、`running_procs_count`、`pending_requeue[]` 与 `global_coord_lock`，
  实现：
  - 同一时间片只运行一个优先级；
  - 多 CPU 运行完成后，按原顺序批量入队，保持队列稳定性。

---

## 并发与正确性

- 所有队列操作在 `mlfq_lock` 下完成；
- 进程状态修改在 `p->lock` 下完成；
- 统一锁顺序 `mlfq_lock` → `p->lock`，规避死锁；
- 取出进程后立即置为 `RUNNING` 并放锁，确保不会被其他 CPU 再次选中。

---

## 关键时序（简化）

1) 选择优先级：
```
if current_priority_level == -1 or running_procs_count == 0:
  批量把 pending_requeue 按顺序放回队列（必要时降级）
  从 Q0→Q1→Q2 选择新的 current_priority_level
```
2) 运行：
```
只从 current_priority_level 的队列取进程；
若队列空但仍有本级在运行，当前 CPU 等待，不去运行其它级别。
```
3) 返回：
```
RUNNABLE → 加入 pending_requeue[]（不立即入队）
其它状态 → 仅 running_procs_count--
若 running_procs_count == 0 → 允许切换到新优先级
```

---

## 已知取舍与后续改进

- 全局锁可能成为瓶颈：可在不破坏“同一时间片单一优先级”的前提下，引入分级锁或 per-queue 锁；
- 当前 pending_requeue[] 容量按 NCPU 估计，若某优先级并发数超 NCPU，可扩展为动态数组或临时环形缓冲；
- 可增加调试接口（如在 `procdump()` 输出 `cpu_schedule_count[]` 与当前 `current_priority_level`）。

---

## 验证建议（How to verify）

- 构建运行：
  - `make` / `make qemu` 观察系统是否正常启动；
- 行为验证：
  - 准备短/长混合作业，验证 Q0/Q1/Q2 依次被完全消化后才切换优先级；
  - 在多 CPU 下制造“同级进程 < CPU 数”的场景，验证空闲 CPU 不会去运行其他级别；
  - 在 Q2 长时间等待后，应被提升到 Q0；
  - 使用 `^P` 触发 `procdump()`，观察进程状态与队列变化趋势；
- 稳定性：
  - 跑 `usertests` 与 `stressfs`，观察是否有死锁/崩溃。

---

## 变更清单（Quick diff by intent）
- `proc.h`
  - 新增：`qlevel`, `ticks_used`, `last_run`, `in_mlfq_queue`。
- `proc.c`
  - 新增结构与锁：`mlfq[NQUEUE]`, `mlfq_lock`, `time_slice[]`。
  - 新增方法：`enqueue/dequeue/queue_empty/remove_from_queue/init_mlfq/check_aging`。
  - 新增协调：`current_priority_level`, `running_procs_count`, `pending_requeue[]`, `global_coord_lock`。
  - 修改：`procinit/userinit/fork/wakeup/yield/scheduler` 按上述逻辑重写/增强。
- `param.h`
  - 注释调整（不影响行为）。

---

如需我再补一份“代码走读注释版”或“可视化时序图”，告诉我你希望关注的路径（比如 fork→调度→yield→wakeup）。

---

## 关键函数改动对照（Before/After by function）

以下展示原始 xv6 代码与本次修改后的实现，便于逐函数对照。注意：为清晰起见，仅展示与本次 MLFQ 实现强相关的函数主体；未改动的辅助代码在此省略。

### 1) `scheduler()`

原始（xv6 vanilla）：

```c
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
      asm volatile("wfi");
    }
  }
}
```

修改后（全局 MLFQ + 单时间片单优先级 + 批量按顺序回队）：

```c
void
scheduler(void)
{
  struct proc *selected;
  struct cpu *c = mycpu();
  extern uint ticks;

  c->proc = 0;
  for(;;){
    intr_on();

    // 进入全局协调与队列临界区
    acquire(&global_coord_lock);
    acquire(&mlfq_lock);

    cpu_schedule_count[cpuid()]++;
    check_aging();

    // 若当前无指定优先级或本级运行数为 0，则先批量回队并选择新优先级
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
      } else if(running_procs_count > 0) {
        // 同一时间片内仅允许运行同一优先级：队列空但仍有本级在跑，则当前 CPU 空转等待
        selected = 0;
      }
    }

    release(&mlfq_lock);
    release(&global_coord_lock);

    if(selected) {
      selected->last_run = ticks;
      c->proc = selected;
      swtch(&c->context, &selected->context);
      c->proc = 0;

      // 返回后统一处理回队/降级，保持顺序
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
      // 无可运行任务或需等待同级完成时间片
      intr_on();
      asm volatile("wfi");
    }
  }
}
```

---

### 2) `yield()`（时间片计数改由时钟中断驱动）

原始：

```c
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}
```

修改后（递增 `ticks_used`，不直接入队，回队统一由调度器批量处理）：

```c
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

---

### 3) `wakeup()`（两阶段：收集 → 批量入队，满足锁顺序与顺序稳定）

原始：

```c
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}
```

修改后（先收集 RUNNABLE，再在 `mlfq_lock` 下统一入队）：

```c
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

---

### 4) `fork()`（新进程入 Q0，锁顺序 mlfq_lock → p->lock）

原始片段（结尾处）：

```c
  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);
  return pid;
```

修改后：

```c
  // 先获取全局锁，再获取进程锁，避免锁序反转
  acquire(&mlfq_lock);
  acquire(&np->lock);

  np->state = RUNNABLE;
  enqueue(&mlfq[0], np); // 新进程进入 Q0 队尾

  release(&np->lock);
  release(&mlfq_lock);
  return pid;
```

---

### 5) `userinit()`（首个用户进程入 Q0，保持锁序）

原始片段（结尾处）：

```c
  p->state = RUNNABLE;
  release(&p->lock);
```

修改后：

```c
  acquire(&mlfq_lock);
  p->state = RUNNABLE;
  enqueue(&mlfq[0], p); // 首个用户进程进入 Q0 队尾
  release(&mlfq_lock);
  release(&p->lock);
```

---

### 6) `procinit()`（初始化全局结构与 per-proc 字段）

原始：

```c
void
procinit(void)
{
  struct proc *p;
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int) (p - proc));
  }
}
```

修改后：

```c
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

---

### 7) `struct proc`（位于 `proc.h`）

原始结尾片段：

```c
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};
```

修改后（新增 MLFQ 元信息）：

```c
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // MLFQ 调度相关字段
  int qlevel;                  // 当前所在队列层级 (0, 1, 2)
  int ticks_used;              // 当前队列内已使用时间片
  uint last_run;               // 上次运行时的全局 ticks
  int in_mlfq_queue;           // 是否已在 MLFQ 队列中 (避免重复入队)
};
```

---

### 8) 新增：队列与老化工具函数（`proc.c`）

新增（节选）：

```c
#define NQUEUE 3
struct proc_queue {
  struct proc *procs[NPROC];
  int head, tail;
  int size;
};

struct proc_queue mlfq[NQUEUE];
struct spinlock mlfq_lock;
int time_slice[NQUEUE] = {1, 2, 4};

void init_mlfq(void) { ... }
void enqueue(struct proc_queue *q, struct proc *p) { ... }
struct proc* dequeue(struct proc_queue *q) { ... }
int queue_empty(struct proc_queue *q) { ... }
int remove_from_queue(struct proc_queue *q, struct proc *target) { ... }
void check_aging(void) { ... }
```

理由：提供 MLFQ 的核心数据结构与操作原语。


# 写时复制 (Copy-On-Write) 实现说明

## 目标
通过在 `fork()` 时延迟物理页复制，实现高效的进程创建：父子初始共享物理页，首次写入才真正复制，减少内存与时间开销，并保证引用计数正确回收物理页。

## 修改总览
| 文件 | 修改内容 | 目的 |
|------|----------|------|
| `kernel/riscv.h` | 新增 `PTE_COW` 标志位(bit8) | 标记页为写时复制，只读共享 |
| `kernel/kalloc.c` | 增加 `refcnt[]`、`incref/decref/refcount`、在 `kalloc/kfree` 维护引用计数 | 物理页多引用管理 |
| `kernel/defs.h` | 声明新接口 | 让其它模块调用 |
| `kernel/vm.c` | 重写 `uvmcopy` 为 COW、`uvmunmap` 使用 `decref`、新增 `cowfault`、修改 `copyout` 支持COW写 | 核心COW逻辑与缺页处理 |
| `kernel/trap.c` | 在 `usertrap()` 中处理 `scause==15` (store page fault) 调用 `cowfault` | 捕获用户写入触发复制 |

## 数据结构与关键标志
- `PTE_COW`：页表项标记页为写时复制；与 `PTE_W` 互斥（共享阶段去掉写权限）。
- `refcnt[(PHYSTOP-KERNBASE)/PGSIZE]`：每个可分配物理页一个引用计数。
- 引用计数原则：
  - `kalloc()` 新页：计数设为 1。
  - `fork()` 共享页：每共享一次 `incref()`。
  - 进程释放地址空间或复制时旧页使用 `decref()`；计数归零才 `kfree()`。

## fork() 流程 (修改后)
1. 子进程页表创建（陷阱页、trampoline 与旧逻辑一致）。
2. 遍历父进程地址空间页：
   - 若页可写 (`PTE_W`)，转换为只读 + `PTE_COW`（父页表）。
   - 子页表建立指向相同物理页的映射（同样去写加 `PTE_COW`）。
   - `incref(pa)` 增加物理页引用计数。
3. 不分配/复制物理内存；大幅减少 fork 成本。

## 写时复制触发路径
### 用户态写入产生 store page fault
- RISC-V `scause==15` 表示 store/AMO page fault。
- `trap.c:usertrap()` 捕获后：
  1. 检查 `va` 是否越界 (`va < p->sz`)。
  2. 调用 `cowfault(p->pagetable, PGROUNDDOWN(va))`：
     - 若引用计数 `>1`：分配新页 -> 拷贝旧内容 -> 更新 PTE 指向新物理页，置可写，清除 `PTE_COW` -> `decref(old)`。
     - 若引用计数 `==1`：直接恢复 `PTE_W` 并清除 `PTE_COW`（无需复制）。
  3. 成功则继续执行用户指令；失败则 kill 进程。

### 内核 `copyout()` 写入用户页
- 原实现因页不可写会直接失败。现在：
  - 检测到 `!PTE_W && PTE_COW` 时调用 `cowfault()` 预先分裂，再继续写入。
  - 支持 `pipe/write` 等场景在共享只读页上第一次内核写入。

## 释放流程
- `exit()` 或 `sbrk` 收缩触发 `uvmunmap()`：
  - 对每个叶子页调用 `decref(pa)`。
  - `decref()` 内部：引用计数减一；若归零调用 `kfree()` 真正释放页。
- 页表页仍直接 `kfree()`（在 `kfree` 中把其 refcnt 重置为 0，避免残留）。

## cowfault() 逻辑摘要
```
if (!PTE_COW) return -1;
rc = refcount(pa);
if (rc == 1) { // 独占
  置回 PTE_W, 去掉 PTE_COW;
} else {
  new = kalloc();
  memcpy(new, old, PGSIZE);
  更新PTE指向new (PTE_W, 去COW);
  decref(old);
}
sfence_vma();
```

## 正确性与边界情况
- 并发：xv6 简化内核，单页故障处理在内核态串行执行；使用锁保护 `refcnt`，避免竞争。
- 非法访问：越界写或非 COW 页故障直接 `setkilled()`。
- 多重共享：任意链式 fork() 引用计数增长，首次写入只分裂当前进程需要修改的那一页。
- 内核写入（`copyout`）不会绕过权限导致错误修改共享页内容。

## 测试覆盖 (来源 `user/cowtest.c`)
| 测试 | 场景 | 通过标准 |
|------|------|----------|
| `simpletest` | 大量分配 + fork 不爆内存 | 子进程快速退出，父继续运行，内存归还后可再次分配 |
| `threetest` | 多进程写不同范围，验证内容与释放 | 写入后父内容保持，重复执行不泄漏 |
| `filetest` | `copyout` 向 COW 页写入 | 管道传输正确，父缓冲区首字节保持原值 |
| `forkforktest` | 多层次 fork + 并发写触发 race | 父页内容未被错误改写，引用计数稳定 |

## 可能的改进方向
- 延迟 `sfence_vma()`：当前每次复制后全局刷新，可优化为按页刷新或利用 `satp` 重新加载。
- 将 `refcnt` 镶嵌到页头减少数组索引计算。
- 增加调试接口：打印每个进程的 COW 页数量。

## 运行与验证
若已安装 RISC-V 工具链：
```bash
make qemu
user/cowtest
```
输出应包含：`ALL COW TESTS PASSED`。

## 总结
通过引入 `PTE_COW` 与引用计数，将原来的物理页全量复制改为按需复制：
- fork 时间复杂度从 O(内存页数拷贝) 降为 O(页表遍历)；
- 写入时仅复制被修改的页；
- 引用计数确保物理内存安全回收，无重复释放或泄漏。

以上修改完成后即可满足 `cowtest.c` 中的所有测试场景。

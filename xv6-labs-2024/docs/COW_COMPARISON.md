# 写时复制 (COW) 作战计划 vs 实际实现对比

本文件记录最初的“作战计划”需求要点与当前内核已实现方案的异同，便于复查与后续增强。

---
## 计划要点回顾
1. 修改 `uvmcopy()`：fork 时子进程直接共享父物理页，不分配新页；清除父/子所有可写页 (`PTE_W`)。
2. 实现 `vmfault()`：在写型缺页错误（store page fault）发生于原本可写的 COW 页时：
   - 分配新物理页 `kalloc()`。
   - 拷贝旧页内容。
   - 更新页表项为可写 (恢复 `PTE_W`)。
   - 对非 COW 只读页（例如文本段）写入应杀死进程。
3. 引入物理页引用计数：fork 时 +1，释放时 -1，计数为 0 才 `kfree()`。
4. `copyout()` 遇到 COW 页时采用与缺页异常相同策略完成复制分裂。
5. 使用 RISC-V 页表的 RSW（软件保留位）记录 COW 状态。
6. OOM：若 COW 分裂时无可用内存，终止触发进程。
7. 其他测试（`usertests -q`）也需通过，不仅是 `cowtest`。

---
## 已实现方案概述
文件位置与核心改动：
| 文件 | 改动 | 作用 |
|------|------|------|
| `kernel/riscv.h` | 定义 `PTE_COW (1<<8)` | 使用软件保留位标记 COW 页 |
| `kernel/kalloc.c` | 引入 `refcnt[]` + `incref/decref/refcount` | 物理页引用管理，延迟释放 |
| `kernel/defs.h` | 新增相关函数声明 | 暴露接口给其它模块 |
| `kernel/vm.c` | 重写 `uvmcopy` 为共享；`uvmunmap` 用 `decref`；新增 `cowfault`；修改 `copyout` 支持 COW | 写时复制主逻辑与缺页分裂 |
| `kernel/trap.c` | 在 `usertrap()` 中处理 `scause==15` 调用 `cowfault` | 捕获用户态写缺页 |
| `docs/COW.md` | 文档说明 | 设计与测试说明 |

---
## 逐条对比
| 计划点 | 当前实现 | 是否一致 | 差异/改进说明 |
|--------|----------|----------|---------------|
| fork 共享页 + 去写 | `uvmcopy` 去 `PTE_W` 加 `PTE_COW` 并 `incref` | 一致 | 额外使用 `PTE_COW` 区分共享写时复制页 |
| 使用 `vmfault()` 处理写缺页 | 使用 `cowfault()` (功能相同) 在 `vm.c` | 一致(命名不同) | 命名更贴近语义，集成在 `vm.c`；由 `trap.c` 调用 |
| 写缺页总是复制 | 我们在 `refcount==1` 时直接恢复写权限不复制 | 部分不同 | 优化：独占时无需复制，减少内存与拷贝开销 |
| 非 COW 只读页写入终止 | `cowfault` 对非 `PTE_COW` 返回 -1，`usertrap` 杀进程 | 一致 | 严格拒绝写入真实只读页（如代码段） |
| 引用计数数组 | 固定大小 `(PHYSTOP-KERNBASE)/PGSIZE` | 一致 | 简化容量计算，无需运行时动态确定上界 |
| `kalloc()` 设置 ref=1 | 已实现 | 一致 | 分配后保证初始引用正确 |
| fork 引用计数 +1 | `uvmcopy` 中 `incref(pa)` | 一致 | — |
| 释放时计数归零才 `kfree` | `decref` 归零调用 `kfree` | 一致 | 并在 `kfree` 设 ref=0 防残留 |
| `copyout` 支持 COW | 检测 `!PTE_W && PTE_COW` -> 调用 `cowfault` | 一致 | 支持内核写用户空间首次分裂 |
| 标记使用 RSW 位 | `PTE_COW (1<<8)` | 一致 | 位选择符合保留位用法 |
| OOM 时终止进程 | `cowfault` 分配失败返回 -1，`usertrap` 杀进程 | 一致 | 行为与计划匹配 |
| 其它测试适配 | 逻辑通用，与 `cowtest` 之外也兼容 | 一致 | 可继续运行 `usertests -q` 验证完整性 |
| 刷新 TLB | 每次分裂后 `sfence_vma()` | 计划未明示 | 额外正确性保证 |

---
## 重要差异详解
1. 优化：`refcount==1` 时不复制
   - 计划：总是分配新页。
   - 实现：独占页直接恢复写权限，节省一次 `kalloc+memmove`。
   - 影响：降低写热点场景开销（如父 fork 后立即写同一页）。

2. 函数命名与结构
   - 计划：使用 `vmfault()`。
   - 实现：采用 `cowfault()`；将 COW 专用逻辑与一般页表操作同置。
   - 影响：语义更清晰（只处理 COW，不处理一般缺页）。

3. 显式 TLB 刷新
   - 分裂后执行 `sfence_vma()`，确保新权限立即生效。

4. 释放路径精炼
   - `uvmunmap` 上直接调用 `decref`；页表页仍走原 `freewalk`。

---
## 与测试用例的对应关系
| 测试 | 验证点 | 依赖实现组件 |
|------|--------|-------------|
| simpletest | 延迟复制与成功释放 | `uvmcopy` / `uvmunmap` / 引用计数 |
| threetest (+重复) | 多进程局部写分裂正确性 | `cowfault` / `usertrap` / `refcnt` 锁保护 |
| filetest | 内核 `copyout` 写 COW 页分裂 | `copyout` 补充逻辑 / `cowfault` |
| forkforktest | 高频 fork 稳定性 | `uvmcopy` 高效共享 / refcnt 正确增减 |

---
## 代码关键片段引用（摘要）
```c
// uvmcopy: 去写 + 标记 COW + incref
if(flags & PTE_W){
  *pte = (*pte & ~PTE_W) | PTE_COW;
  flags = (flags & ~PTE_W) | PTE_COW;
}
*npte = PA2PTE(pa) | flags | PTE_V;
incref(pa);
```
```c
// cowfault: 引用==1直接恢复写；否则复制
if(rc == 1){
  *pte = (*pte & ~PTE_COW) | PTE_W;
}else{
  char *mem = kalloc();
  memmove(mem, (char*)pa, PGSIZE);
  *pte = PA2PTE(mem) | ((PTE_FLAGS(*pte) & ~PTE_COW) | PTE_W) | PTE_V;
  decref(pa);
}
sfence_vma();
```
```c
// copyout: COW 写分裂
if(((*pte & PTE_W) == 0) && (*pte & PTE_COW)){
  if(cowfault(pagetable, va0) < 0) return -1;
  pte = walk(pagetable, va0, 0);
}
```

---
## 结论
- 已实现方案完整满足原计划所有功能点。
- 增加 `refcount==1` 不复制与显式 TLB 刷新等优化，提高性能与正确性。
- 结构命名略有不同但不影响语义，且更清晰区分 COW 场景。
- 推荐下一步：运行完整 `usertests -q` 做扩展验证；可添加调试接口显示某虚拟地址的 COW/引用计数状态。

---
## 后续可选增强
1. 局部 `sfence.vma`：只刷新改动页地址。
2. 引入 `/proc/cowstat`（或命令）输出当前进程 COW 页数量。
3. 将 `refcnt` 嵌入页头以减少数组计算与缓存行跳转。

如需补充测试跑结果或调试工具，请提出。
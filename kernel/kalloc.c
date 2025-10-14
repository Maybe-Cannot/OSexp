// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
#ifdef LAB_PGTBL
  struct run *superfreelist;  // 超级页链表
#endif
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
#ifdef LAB_PGTBL
  kmem.superfreelist = 0;
#endif
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  
#ifdef LAB_PGTBL
  // 预留前 10 个 2MB 对齐的超级页
  int super_count = 0;
  char *super_start = (char*)SUPERPGROUNDUP((uint64)p);
  
  // 先释放对齐前的页到普通链表
  for(; p < super_start && p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    kfree(p);
  }
  
  // 预留 10 个超级页
  for(; super_count < 10 && p + SUPERPGSIZE <= (char*)pa_end; 
      p += SUPERPGSIZE, super_count++) {
    struct run *r = (struct run*)p;
    r->next = kmem.superfreelist;
    kmem.superfreelist = r;
  }
#endif
  
  // 剩余的页释放到普通链表
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

#ifdef LAB_PGTBL
// 从超级页池分配一个 2MB 超级页
void* 
superalloc(void) {
  struct run *r;
  
  acquire(&kmem.lock);
  r = kmem.superfreelist;
  if(r) {
    kmem.superfreelist = r->next;
  }
  release(&kmem.lock);
  
  if(r) {
    memset((char*)r, 5, SUPERPGSIZE);
  }
  
  return (void*)r;
}

void
superfree(void *pa)
{
  struct run *r;
  
  if(((uint64)pa % SUPERPGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("superfree");
  
  // 填充垃圾值
  memset(pa, 1, SUPERPGSIZE);
  
  r = (struct run*)pa;
  
  acquire(&kmem.lock);
  r->next = kmem.superfreelist;
  kmem.superfreelist = r;
  release(&kmem.lock);
}

#endif
struct stat;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);

// umalloc.c
void* malloc(uint);
void free(void*);


// EXp3 sandbox
int interpose(int mask, const char *path);

struct sysinfo {
  uint freemem;  // 空闲内存
  uint nproc;    // 当前进程数
};

int sysinfo(struct sysinfo *);
int trace(int);

// System call numbers (copied from kernel/syscall.h)
#define SYS_fork        1
#define SYS_exit        2
#define SYS_wait        3
#define SYS_pipe        4
#define SYS_read        5
#define SYS_kill        6
#define SYS_exec        7
#define SYS_fstat       8
#define SYS_chdir       9
#define SYS_dup         10
#define SYS_getpid      11
#define SYS_sbrk        12
#define SYS_sleep       13
#define SYS_uptime      14
#define SYS_open        15
#define SYS_write       16
#define SYS_mknod       17
#define SYS_unlink      18
#define SYS_link        19
#define SYS_mkdir       20
#define SYS_close       21
#define SYS_interpose   22   // 你新增的系统调用号
#define SYS_sysinfo     23
#define SYS_trace       24

// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

// 直接块数量调整为 11，预留出一个位置给一级间接和一个位置给二级间接块。
#define NDIRECT 11
// 一个一级间接块可引用的块数
#define NINDIRECT (BSIZE / sizeof(uint))
// 二级间接块可引用的总数据块数：其本身包含 NINDIRECT 个一级间接块指针，每个一级间接块再引用 NINDIRECT 个数据块。
#define N2INDIRECT (NINDIRECT * NINDIRECT)
// 最大文件块数：11 个直接块 + 1 个一级间接块提供 NINDIRECT 数据块 + 1 个二级间接块提供 N2INDIRECT 数据块。
#define MAXFILE (NDIRECT + NINDIRECT + N2INDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  // 数据块地址：
  // [0..NDIRECT-1] 直接块
  // [NDIRECT] 一级间接块
  // [NDIRECT+1] 二级间接块（指向存放一级间接块指针的块）
  uint addrs[NDIRECT+2];   // Data block addresses (direct + single indirect + double indirect)
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};


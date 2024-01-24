#ifndef _PARAM_H_
#define _PARAM_H_

// System parameters

#define NPROC        64  // maximum number of processes
#define KSTACKSIZE 4096  // size of per-process kernel stack
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NBUF         10  // size of disk block cache
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define USERTOP  0xA0000 // end of user address space
#define PHYSTOP  0x1000000 // use phys mem up to here as free pool
#define MAXARG       32  // max exec arguments

#define PRIORITY_HIGH   3
#define PRIORITY_MEDIUM 2
#define PRIORITY_LOW    1
#define PRIORITY_BOTTOM 0

#define TIME_SLICE_3 8
#define TIME_SLICE_2 16
#define TIME_SLICE_1 32
#define TIME_SLICE_0 50


#endif // _PARAM_H_

#ifndef _SHM_HEADER_H
#define _SHM_HEADER_H

// #define SHM_KEY 19942017
#define MAX_BUF_SIZE 1024

typedef struct 
{
	long long seconds;
	long long nanoseconds;
	int sigNotReceived;
} shared_oss_struct;

typedef struct resAllocation {
  int qty[20];
} resAlloc;

typedef struct PCB {
  pid_t processID;
  int request;
  int release;
  int deadlocked;
  int terminate;
  resAlloc ralloc;
  long long totalTimeRan;
  long long createTime;
} PCB;

typedef struct resource {
	int qty;
	int availQty;
} resource;

typedef struct msgbuf {
  long mType;
  char mText[80];
} msgbuf;

const int MAX_FUTURE_SPAWN = 280000001;

#endif
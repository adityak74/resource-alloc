#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/msg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "shm_header.h"

#define PERM 0666
#define PERMS (mode_t)(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define FLAGS (O_CREAT | O_EXCL)

pid_t myPid;
shared_oss_struct *shpinfo;
PCB *pcbArray;
resource *resourceArray;
const int QUIT_TIMEOUT = 10;
volatile sig_atomic_t sigNotReceived = 1;
int processNumber = 0;
key_t masterKey = 14641;
key_t slaveKey = 120314;
int masterQueueId;
void sendMessage(int, int);

// wait for semaphore
pid_t r_wait(int *stat_loc) {
  pid_t retval;
  while (((retval = wait(stat_loc)) == -1) && (errno == EINTR)) ;
  return retval;
}

// get the semaphore
int getnamed(char *name, sem_t **sem, int val) {
   while (((*sem = sem_open(name, FLAGS , PERMS, val)) == SEM_FAILED) &&
           (errno == EINTR)) ;
   if (*sem != SEM_FAILED)
       return 0;
   if (errno != EEXIST)
      return -1;
   while (((*sem = sem_open(name, 0)) == SEM_FAILED) && (errno == EINTR)) ;
   if (*sem != SEM_FAILED)
       return 0;
   return -1;
}

void intHandler(int);
void zombieKiller(int);

int main(int argc, char const *argv[])
{

	int shmid = 0, shmMsgID = 0, pcbShmid = 0;
	long start_seconds, start_nanoseconds;
	long current_seconds, current_nanoseconds;
	long long currentTime;
	myPid = getpid();
	char *short_options = "i:s:k:p:";
	int maxproc;
	char c;
	sem_t *semlockp;

	//get options from parent process
	opterr = 0;
	while((c = getopt(argc, argv, short_options)) != -1)
		switch (c) {
			case 'i':
				processNumber = atoi(optarg);
				break;
			case 's':
				maxproc = atoi(optarg);
				break;
			case 'k':
				shmid = atoi(optarg);
				break;
			case 'p':
				pcbShmid = atoi(optarg);
				break;
			case '?':
				fprintf(stderr, "    Arguments were not passed correctly to slave %d. Terminating.", myPid);
				exit(-1);
		}

 	srand(time(NULL) + processNumber);

 	//Trying to attach to shared memory
	if((shpinfo = (shared_oss_struct *)shmat(shmid, NULL, 0)) == (void *) -1) {
		perror("    Slave could not attach shared mem shpinfo");
		exit(1);
	}

	if((pcbArray = (PCB *)shmat(pcbShmid, NULL, 0)) == (void *) -1) {
	    perror("    Slave could not attach to shared memory array");
	    exit(1);
	  }

	//Ignore SIGINT so that it can be handled below
	signal(SIGINT, intHandler);

	//Set the sigquitHandler for the SIGQUIT signal
	signal(SIGQUIT, intHandler);

	//Set the alarm handler
	signal(SIGALRM, zombieKiller);

	//Set the default alarm time
	alarm(QUIT_TIMEOUT);

	// master queue for message
	if((masterQueueId = msgget(masterKey, IPC_CREAT | 0777)) == -1) {
		perror("    Slave msgget for master queue");
		exit(-1);
	}

	int i=0, j;

	long long duration = 1 + rand() % 1000000;
	printf("    Slave %d got duration %llu\n", processNumber, duration);

  	int notFinished = 1;

	start_seconds = shpinfo -> seconds;
	start_nanoseconds = shpinfo -> nanoseconds;
  	long long startTime = start_seconds*1000000000 + start_nanoseconds;

  	// SEMAPHORE EXCLUSION

  	if (getnamed("tesemn", &semlockp, 1) == -1) {
	  perror("Failed to create named semaphore");
	  return 1;
	}

	while (sem_wait(semlockp) == -1)                         /* entry section */ 
       if (errno != EINTR) { 
          perror("Failed to lock semlock");
          return 1;
       }

    // CRITICAL SECTION

    // fprintf(stderr, "USER PROCNUM# :%d CLOCK READ : %lld %lld\n", getpid() ,shpinfo -> seconds, shpinfo -> nanoseconds);

    do {
  
	    //If this process' request flag is -1, it is not waiting on anything
	    //go ahead and determine an action
	    if(pcbArray[processNumber].request == -1 && pcbArray[processNumber].release == -1) {
	      //printf("    Slave %d has no request\n", processNumber);
	      //Check to see if process will terminate
	      if(willTerminate()) {
	        notFinished = 0;
	      }
	      //if not, see what other action it will take
	      else {
	        if(takeRandomAction()) {
	          int choice = rand() % 2;
	          //Request a resource
	          if(choice) {
	           pcbArray[processNumber].request = chooseRandomResource(); 
	           //sendMessage(masterQueueId, 3);
	          }
	          //Release a resource
	          else {
	            int i;
	            for(i = 0; i < 20; i++) {
	              if(pcbArray[processNumber].ralloc.qty[i] > 0) {
	                pcbArray[processNumber].release = i;
	                break;
	              }
	            }
	            //sendMessage(masterQueueId, 3);
	          }
	        }
	      }
	    }
	} while (notFinished && shpinfo->sigNotReceived && !pcbArray[processNumber].terminate);

	// wait if any other process data in the sharedMessage
	// while( ossShmMsg -> procID != -1 ) {
	// 	break;
	// }

	// ossShmMsg -> procID = getpid();
	// ossShmMsg -> seconds = shpinfo -> seconds;
	// ossShmMsg -> nanoseconds = shpinfo -> nanoseconds;
    // CRITICAL ENDS
	
	if (sem_post(semlockp) == -1) {                           /* exit section */
	  perror("Failed to unlock semlock");
	  return 1; 
	}

	if(!pcbArray[processNumber].terminate) {
		pcbArray[processNumber].processID = -1;
		//sendMessage(masterQueueId, 3);
	}

	// REMAINDER
	if (r_wait(NULL) == -1)                              /* remainder section */
	  return 1;

	if(shmdt(shpinfo) == -1) {
		perror("    Slave could not detach shared memory");
	}

	if(shmdt(pcbArray) == -1) {
		perror("    Slave could not detach shared memory");
	}

  	// END
  	printf("    Slave %d exiting\n", processNumber);
	kill(myPid, SIGTERM);
	sleep(1);
	kill(myPid, SIGKILL);
	printf("    Slave error\n");

	return 0;
}

int chooseRandomResource(void) {
  int choice = rand() % 20;
  return choice;
}

int takeRandomAction(void) {
  int choice = rand() % 2;
  return choice;
}

int willTerminate(void) {
  if( (shpinfo->seconds*1000000000 + shpinfo->nanoseconds) - pcbArray[processNumber].createTime >= 1000000000) {
    int choice = 1 + rand() % 5;
    return choice == 1 ? 1 : 0;
  }
  return 0;
}

void sendMessage(int qid, int msgtype) {
  struct msgbuf msg;

  msg.mType = msgtype;
  sprintf(msg.mText, "%d", processNumber);

  if(msgsnd(qid, (void *) &msg, sizeof(msg.mText), IPC_NOWAIT) == -1) {
    perror("    Slave msgsnd error");
  }
}

//This handles SIGQUIT being sent from parent process
//It sets the volatile int to 0 so that it will not enter in the CS.
void intHandler(int sig) {
  printf("    Slave %d has received signal %s (%d)\n", processNumber, strsignal(sig), sig);
  sigNotReceived = 0;

  if(shmdt(shpinfo) == -1) {
    perror("    Slave could not detach shared memory");
  }

  kill(myPid, SIGKILL);

  //The slaves have at most 5 more seconds to exit gracefully or they will be SIGTERM'd
  alarm(5);
}

//function to kill itself if the alarm goes off,
//signaling that the parent could not kill it off
void zombieKiller(int sig) {
  printf("    Slave %d is killing itself due to slave timeout override%s\n", processNumber);
  kill(myPid, SIGTERM);
  sleep(1);
  kill(myPid, SIGKILL);
}

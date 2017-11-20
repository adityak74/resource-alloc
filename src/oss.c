#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "shm_header.h"

#define PERM 0666
#define MAXCHILD 20

const int TOTAL_SLAVES = 100;
const int MAXSLAVES = 20;
const long long INCREMENTER = 4000;

void showHelpMessage();
void intHandler(int);
void spawnChildren(int);
void cleanup();

static int max_processes_at_instant = 1; // to count the max running processes at any instance
int process_spawned = 0; // hold the process number
volatile sig_atomic_t cleanupCalled = 0;
key_t key;
key_t shMsgIDKey = 1234;
key_t resourceKey = 1339;
key_t pcbArrayKey = 15667;
key_t masterQueueKey = 14641;
int shmid, shmMsgID, semid, i, pcbShmid, resourceShmid, masterQueueId;
pid_t childpid;
shared_oss_struct *shpinfo;
PCB *pcbArray;
resource *resourceArray;
int timerVal;
int numChildren;
char *short_options = "hs:l:t:v:";
char *default_logname = "default.log";
char c;
FILE* file;
int status;
int messageReceived = 0;
int vFlag;
long long lastDeadlockCheck;
long long nextSpawnTime;
long long NANO_MODIFIER = 1000000000;
void resourceSnapshot(void);
void setupAllResources(void);
int processNumberSpawning;
int processMessageQueue(void);
void performActionsFromMessage(int);
long long totalProcessLifeTime;
void checkAndProcessRequests(void);
void performResourceRequest(int resourceType, int i);
void performResourceRelease(int resourceType, int i);
void setNextSpawnTime();
void performProcessCleanup(int i);
void killAProcess(void);
int reqAvail(int *work, int p);
int deadlock(void);
int vFlag = 1;


char *i_arg;
char *m_arg;
char *x_arg;
char *s_arg;
char *k_arg;
char *p_arg;

int main(int argc, char const *argv[])
{
	sem_t *semlockp;
	int helpflag = 0;
	int nonoptargflag = 0;
	int index;

	// memory for args
	i_arg = malloc(sizeof(char)*20);
	m_arg = malloc(sizeof(char)*20);
	x_arg = malloc(sizeof(char)*20);
	s_arg = malloc(sizeof(char)*20);
	k_arg = malloc(sizeof(char)*20); 
	p_arg = malloc(sizeof(char)*20);

	opterr = 0;

	while ((c = getopt (argc, argv, short_options)) != -1)
	switch (c) {
		case 'h':
			helpflag = 1;
			break;
		case 's':
			numChildren = atoi(optarg);
			if(numChildren > MAXCHILD) {
			  numChildren = 20;
			  fprintf(stderr, "No more than 20 child processes allowed at one instance. Reverting to 20.\n");
			}
			break;
		case 't':
			timerVal = atoi(optarg);  
			break;
		case 'v':
			vFlag = 1; 
			break;
		case 'l':
			default_logname = optarg;  
			break;
		case '?':
			if (optopt == 's') {
			  fprintf(stderr, "Option -%c requires an argument. Using default value [5].\n", optopt);
			  numChildren = 5;
			}
			else if (optopt == 't') {
			  fprintf(stderr, "Option -%c requires an argument. Using default value [20].\n", optopt);
			  timerVal = 20;
			}
			else if( optopt == 'l') {
			  fprintf(stderr, "Option -%c requires an argument. Using default value [default.log] .\n", optopt);
			  default_logname = "default.log";
			}
			else if (isprint (optopt)) {
			  fprintf(stderr, "Unknown option -%c. Terminating.\n", optopt);
			  return -1;
			}
			else {
			  showHelpMessage();
			  return 0; 
			}
	}

	//print out all non-option arguments
	for (index = optind; index < argc; index++) {
		fprintf(stderr, "Non-option argument %s\n", argv[index]);
		nonoptargflag = 1;
	}

	//if above printed out, print help message
	//and return from process
	if(nonoptargflag) {
		showHelpMessage();
		return 0;
	}

	//if help flag was activated, print help message
	//then return from process
	if(helpflag) {
		showHelpMessage();
		return 0;
	}

	if( numChildren<=0 || timerVal<=0 ) {
		showHelpMessage();
		return 0;
	}

	// handle SIGNALS callback attached
	signal(SIGALRM, intHandler);
	signal(SIGINT, intHandler);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);

	//set alarm
	alarm(timerVal);

	int sizeArray = sizeof(*pcbArray) * 18;
  	int sizeResource = sizeof(*resourceArray) * 20;

	// generate key using ftok
	key = ftok(".", 'c');

	// key = SHM_KEY;
	if(key == (key_t)-1) {
		fprintf(stderr, "Failed to derive key\n");
	}

	shmid = shmget(key, sizeof(shared_oss_struct), IPC_CREAT | 0777);
	if((shmid == -1) && (errno != EEXIST)){
		perror("Unable to create shared mem");
		exit(-1);
	}
	if(shmid == -1) {
		if (((shmid = shmget(key, sizeof(shared_oss_struct), PERM)) == -1) || 
			(shpinfo = (shared_oss_struct*)shmat(shmid, NULL, 0) == (void *)-1) ) {
			perror("Unable to attach existing shared memory");
			exit(-1);
		}
	} else {
		shpinfo = shmat(shmid, NULL, 0);
		if(shpinfo == (void *)-1){
			perror("Couldn't attach the shared mem");
			exit(-1);
		}

		// initialize shmem params
		// initalize clock to zero

		shpinfo -> seconds = 0;
		shpinfo -> nanoseconds = 0;
		shpinfo -> sigNotReceived = 1;

	}

	//get shmid for pcbArray of 18 pcbs
	if((pcbShmid = shmget(pcbArrayKey, sizeArray, IPC_CREAT | 0777)) == -1) {
		perror("Bad shmget allocation pcb array");
		exit(-1);
	}

	//try to attach pcb array to shared memory
	if((pcbArray = (struct PCB *)shmat(pcbShmid, NULL, 0)) == (void *) -1) {
		perror("Master could not attach to pcb array");
		exit(-1);
	}

	if((resourceShmid = shmget(resourceKey, sizeResource, IPC_CREAT | 0777)) == -1) {
		perror("Bad shmget allocation resource array");
		exit(-1);
	}

	if((resourceArray = (struct resource *)shmat(resourceShmid, NULL, 0)) == (void *) -1) {
		perror("Master could not attach to resource array");
		exit(-1);
	}

	//create message queue for the master process
	if((masterQueueId = msgget(masterQueueKey, IPC_CREAT | 0777)) == -1) {
		perror("Master msgget for master queue");
		exit(-1);
	}

	//Open file and mark the beginning of the new log
	file = fopen(default_logname, "a");
	if(!file) {
		perror("Error opening file");
		exit(-1);
	}

	fprintf(file,"***** BEGIN LOG *****\n");

	int i;
	for (i = 0; i < 18; i++) {
		pcbArray[i].processID = 0;
		pcbArray[i].deadlocked = 0;
		pcbArray[i].terminate = 0;
		pcbArray[i].request = -1;
		pcbArray[i].release = -1;
		pcbArray[i].totalTimeRan = 0;
		pcbArray[i].createTime = 0;
		int j;
		for(j = 0; j < 20; j++) {
		  pcbArray[i].ralloc.qty[j] = 0;
		}
	}

	// set resources for all process
	setupAllResources();

	for (int i = 0; i < 18; ++i)
	{
		spawnChildren(1);
	}
	// loop through clock and keep checking shmMsg

	while(shpinfo -> seconds < 2 && shpinfo->sigNotReceived) {
		
		if(nextSpawnTimeCheck()) {
		  spawnChildren(1);
		  setNextSpawnTime();
		}
		if( (shpinfo->seconds*1000000000+shpinfo->nanoseconds) - lastDeadlockCheck > 1000000000) {
		  lastDeadlockCheck = shpinfo->seconds*1000000000 + shpinfo->nanoseconds;
		  printf("---------------------------BEGIN DEADLOCK STUFF--------------------\n");
		  if(deadlock()) {
		    do {
		      killAProcess();
		    }while(deadlock());
		  }
		  printf("---------------------------END DEADLOCK STUFF--------------------\n");
		}

		// increment timer
		shpinfo -> nanoseconds += INCREMENTER;
		if( shpinfo -> nanoseconds == 1000000000 ) {
			shpinfo -> seconds += 1;
			shpinfo -> nanoseconds = 0;
		}


		if(vFlag) {
		  printf("---------------------------BEGIN MESSAGECHECKING--------------------\n");
		}
		performActionsFromMessage(processMessageQueue());
		if(vFlag) {
		   printf("----------------------------END MESSAGECHECKING---------------------\n");
		}
		checkAndProcessRequests();



		//fprintf(stderr, "CURRENT MASTER TIME : %ld.%ld\n", shpinfo -> seconds, shpinfo -> nanoseconds);

		if(max_processes_at_instant <= TOTAL_SLAVES) {
	      spawnChildren(1);
	    }
	    //fprintf(stderr, "CURRENT MASTER TIME : %ld.%ld\n", shpinfo -> seconds, shpinfo -> nanoseconds);
	}

	// Cleanup

	if(!cleanupCalled) {
		cleanupCalled = 1;
		printf("Master cleanup called from main\n");
		cleanup();
	}

	return 0;
}

int nextSpawnTimeCheck(void) {
  if(vFlag) {
    printf("Checking time to spawn: future = %llu.%09llu timer = %llu.%09llu\n", nextSpawnTime / NANO_MODIFIER, nextSpawnTime % NANO_MODIFIER, shpinfo->seconds, shpinfo->nanoseconds);
  }
  if(vFlag) {
    fprintf(file, "Checking time to spawn: future = %llu timer = %llu\n", nextSpawnTime, (shpinfo->seconds*NANO_MODIFIER + shpinfo->nanoseconds));
  }

  return (shpinfo->seconds*NANO_MODIFIER + shpinfo->nanoseconds) >= nextSpawnTime ? 1 : 0;
}

void setNextSpawnTime(void) {
  nextSpawnTime = (shpinfo->nanoseconds*NANO_MODIFIER + shpinfo->nanoseconds) + rand() % MAX_FUTURE_SPAWN;
  if(vFlag) {
    printf("Will try to spawn slave at time %s%s%llu.%09llu\n", nextSpawnTime / NANO_MODIFIER, nextSpawnTime % NANO_MODIFIER);
  }        
  if(vFlag) {
    fprintf(file, "Will try to spawn slave at time %llu\n", nextSpawnTime);
  }
}

// spawnChildren function
void spawnChildren(int childrenCount) {
	int j;
	processNumberSpawning = -1;

	int i;
    for(i = 0; i < 18; i++) {
      if(pcbArray[i].processID == 0) {
        processNumberSpawning = i;
        pcbArray[i].processID = 1;
        break;
      } 
    }

    if(processNumberSpawning == -1) {
      if(vFlag) {
        printf("PCB array is full.\n");
      }
      if(vFlag) {
        fprintf(file, "PCB array is full. No process created.\n");
      }
    }

	for (j = 0; j < childrenCount; ++j){
		printf("About to spawn process #%d\n", processNumberSpawning
			);
		
		//perror on bad fork
	    if((childpid = fork()) < 0) {
	      perror("Fork Failure");
	      //exit(1);
	    }

		/* child process */
		if(childpid == 0) {
			fprintf(stderr, "Max processes running now: %d\n", max_processes_at_instant++);
			childpid = getpid();
      		pid_t gpid = getpgrp();
      		pcbArray[processNumberSpawning].createTime = (shpinfo->seconds*NANO_MODIFIER + shpinfo->nanoseconds);
        	pcbArray[processNumberSpawning].processID = childpid;
			sprintf(i_arg, "%d", processNumberSpawning);
			sprintf(s_arg, "%d", max_processes_at_instant);
			sprintf(p_arg, "%d", pcbShmid);
			// share shmid with children
			sprintf(k_arg, "%d", shmid);
			char *userOptions[] = {"./user", "-i", i_arg, "-s", s_arg, "-k", k_arg, "-p", p_arg, (char *)0};
			execv("./user", userOptions);
			fprintf(stderr, "Print if error %s\n");
		}
		
	}	
}

int processMessageQueue(void) {
  struct msgbuf msg;

  if(msgrcv(masterQueueId, (void *) &msg, sizeof(msg.mText), 3, IPC_NOWAIT) == -1) {
    if(errno != ENOMSG) {
      perror("Error master receivineg message");
      return -1;
    }
    if(vFlag) {
      printf("No message for master\n");
    }
    return -1;
  }
  else {
    int processNumber = atoi(msg.mText);
    return processNumber;
  }
}

void performActionsFromMessage(int processNumber) {
  if(processNumber == -1) {
    return;
  }
  int resourceType;
  if((resourceType = pcbArray[processNumber].request) >= 0) {
    if(vFlag) {
      printf("Found a request from process %d for %d\n", processNumber, resourceType);
    }
    //If there are resources of the type available, assign it
    performResourceRequest(resourceType, processNumber);
  }
  else if ((resourceType = pcbArray[processNumber].release) >= 0) {
    performResourceRelease(resourceType, processNumber);
  }
  else if(pcbArray[processNumber].processID == -1) {
    performProcessCleanup(processNumber);
  }
  else {
    if(vFlag) {
      printf("Found no action for this message\n");
    }
  }

  performActionsFromMessage(processMessageQueue());

}

void updateAverageTurnaroundTime(int pcbPosition) {
  long long startToFinish = (shpinfo->seconds*NANO_MODIFIER+shpinfo->nanoseconds) - pcbArray[pcbPosition].createTime;
  totalProcessLifeTime += startToFinish;
}

void setupAllResources(void) {
  int i;
  //Set the resource types, quantity, and quantAvail
  for(i = 0; i < 20; i++) {
    resourceArray[i].qty = 1 + rand() % 10;
    resourceArray[i].availQty = resourceArray[i].qty;
  }

  //Between 3 and 5 resources will be shareable to all process
  int numShared = 3 + rand() % 3;

  //Get randomly choose a resource for those that
  //will be shared
  for(i = 0; i < numShared; i++) {
    int choice = rand() % 20;
    resourceArray[choice].qty = 9999;
    resourceArray[choice].availQty = 9999;
  }

  resourceSnapshot();
}

void resourceSnapshot(void) {
  int i;
  for(i = 0; i < 20; i++) {
    if(vFlag) {
      printf("Resource %d has %d available out of %d\n", i, resourceArray[i].availQty, resourceArray[i].qty);
    }
  }
}

void checkAndProcessRequests(void) {
  if(vFlag) {
    printf("---------------------------BEGIN CHECKING--------------------\n");
  }
  int i;
  int j;
  int request = -1;
  int release = -1;
  //Go through and look at all the request/release/processID members of each pcbArray element
  //and see if there is any processing to do 
  for(i = 0; i < 18; i++) {	
    int resourceType = -1;
    int quant;
    if(vFlag) {
      printf("--------------CHECKING PROCESS %d---------------\n", i);
    }
    //If the request flag is set with the value of a resource type, process the request
    if((resourceType = pcbArray[i].request) >= 0) {
      if(vFlag) {
        printf("Found a request from process %d for %d\n", i, resourceType);
      }
      //If there are resources of the type available, assign it
      performResourceRequest(resourceType, i);
    }
    //If the release flag is set with the value of the resourceType, process it
    else if((resourceType = pcbArray[i].release) >= 0) {
      performResourceRelease(resourceType, i);
    }
    //If the process set its processID to -1, that means it died and we can put all
    //the resources it had back into the resourceArray
    else if(pcbArray[i].processID == -1){
      performProcessCleanup(i);    
    }
    else {
      //If there is a process at that location but doesn't meet the above criteria, print
      if(pcbArray[i].processID > 0) {
        if(vFlag) {
          printf("No request for this process\n");
        }
      }
    }
  }
  if(vFlag) {
    printf("---------------------------END CHECKREQUESTS--------------------\n");
  }
}

void performResourceRequest(int resourceType, int i) {
  int qqty;
  if((qqty = resourceArray[resourceType].availQty) > 0) {
    if(vFlag) {
      printf("There are %d out of %d for resource %d available\n", qqty, resourceArray[resourceType].qty, resourceType);
    }
    if(vFlag) {
      printf("Increased resource %d for process %d\n", resourceType, i);
    }
    //Increase the quantity of the resourceType for the element in the pcbArray
    //requesting it
    pcbArray[i].ralloc.qty[resourceType]++;
    //Reset the request to -1
    pcbArray[i].request = -1;
    //Decrease the quantity of the resource type in the resource array
    resourceArray[resourceType].availQty--;
    if(vFlag) {
      printf("There are now %d out of %d for resource %d\n", resourceArray[resourceType].availQty, resourceArray[resourceType].qty, resourceType);
    }
  }
}

void performResourceRelease(int resourceType, int i) {
  if(vFlag) {
    printf("Releasing resouce %d from process %d\n", resourceType, i);
  }
  //Decrease the count of the quantity of that resource for that element in the pcbArray
  pcbArray[i].ralloc.qty[resourceType]--;
  //Increase the quantity of that resource type in the resourceArray
  resourceArray[resourceType].availQty++;
  if(vFlag) {
    printf("There are now %d out of %d for resource %d\n", resourceArray[resourceType].availQty, resourceArray[resourceType].qty, resourceType);
  }
  //Reset the release flag to -1
  pcbArray[i].release = -1;
}

void performProcessCleanup(int i) {
  if(vFlag) {
    printf("Process %d completed its time\n", i);
  }
  if(vFlag) {
    fprintf(file, "Process completed its time\n");
  }
  //Go through all the allocations to the dead process and put them back into the
  //resource array
  int j;
  for(j = 0; j < 20; j++) {
    //If the quantity is > 0 for that resource, put them back
    if(pcbArray[i].ralloc.qty[j] > 0) {
      if(vFlag) {
        printf("Before return of resources, there are %d out of %d for resource %d\n", resourceArray[j].availQty, resourceArray[j].qty, j);
      }
      //Get the quantity to put back
      int returnQqty = pcbArray[i].ralloc.qty[j];
      //Increase the resource type availQty in the resource array
      resourceArray[j].availQty += returnQqty;
      if(vFlag) {
        printf("Returning %d of resource %d from process %d\n", returnQqty, j, i);
      }
      if(vFlag) {
        printf("There are now %d out of %d for resource %d\n", resourceArray[j].availQty, resourceArray[j].qty, j);
      }
      //Set the quantity of the pcbArray to 0
      pcbArray[i].ralloc.qty[j] = 0;
    } 
  }
  //Reset all values
  pcbArray[i].processID = 0;
  pcbArray[i].totalTimeRan = 0;
  pcbArray[i].createTime = 0;
  pcbArray[i].request = -1;
  pcbArray[i].release = -1;

}

int deadlock(void) {
  printf("Begin deadlock detection at %llu.%llu\n", shpinfo->seconds, shpinfo->nanoseconds);
  int work[20];
  int finish[18];

  int p;
  for(p = 0; p < 20; p++) {
    work[p] = resourceArray[p].availQty;
  } 
  for(p = 0; p < 18; p++) {
    finish[p] = 0; 
  }
  for(p = 0; p < 18; p++) {
    if(!pcbArray[p].processID) {
      finish[p] = 1;
    }
    if(finish[p]) continue;
    if(reqAvail(work, p)) {
      finish[p] = 1;
      int i;
      for(i = 0; i < 20; i++) {
        work[i] += pcbArray[p].ralloc.qty[i];
      }
      p = -1;
    }       
  }


  int deadlockCount = 0;
  for(p = 0; p < 18; p++) {
    if(!finish[p]) {
      pcbArray[p].deadlocked = 1;
      deadlockCount++; 
    }
    else {
      pcbArray[p].deadlocked = 0;
    }
  }

  if(deadlockCount > 0) {
    printf("%d processes: ", deadlockCount);
    for(p = 0; p < 18; p++) {
      if(!finish[p]) {
        printf("%d ", p);
      }
    }
    printf("are deadlocked\n");
    return deadlockCount;
  }
  else {
    printf("There are no deadlocks\n");
    return deadlockCount;
  }
}

int reqAvail(int *work, int p) {
  if(pcbArray[p].request == -1) {
    return 1;
  } 
  if(work[pcbArray[p].request] > 0) {
    return 1;
  }
  else {
    return 0;
  }
}

void killAProcess(void) {
  int process;
  int max = 0;
  int i;
  int j;
  for(i = 0; i < 18; i++) {
    if(pcbArray[i].deadlocked) {
      int total = 0;
      for(j = 0; j < 20; j++) {
        total += pcbArray[i].ralloc.qty[j];
      }
      if(total > max) {
        max = total;
        process = i;
      }
    }
  }
  printf("Killing process %d\n", process);
  pcbArray[process].deadlocked = 0;
  pcbArray[process].terminate = 1;
  performProcessCleanup(process);
}

// Detach and remove sharedMem function
int detachAndRemove(int shmid, shared_oss_struct *shmaddr) {
  printf("Master: Remove Shared Memory\n");
  int error = 0;
  if(shmdt(shmaddr) == -1) {
    error = errno;
  }
  if((shmctl(shmid, IPC_RMID, NULL) == -1) && !error) {
    error = errno;
  }
  if(!error) {
    return 0;
  }

  return -1;
}

void cleanup() {
  signal(SIGQUIT, SIG_IGN);
  shpinfo -> sigNotReceived = 0;
  printf("Master sending SIGQUIT\n");
  kill(-getpgrp(), SIGQUIT);

  //free up the malloc'd memory for the arguments
  free(i_arg);
  free(s_arg);
  free(k_arg);
  free(x_arg);
  printf("Master waiting on all processes do die\n");
  childpid = wait(&status);

  printf("Master about to detach from shared memory\n");
  //Detach and remove the shared memory after all child process have died
  if(detachAndRemove(shmid, shpinfo) == -1) {
    perror("Failed to destroy shared memory segment");
  }

  if(detachAndRemove(pcbShmid, pcbArray
  	) == -1) {
    perror("Failed to destroy shared memory segment");
  }

  if(fclose(file)) {
    perror("    Error closing file");
  }

  printf("Master about to kill itself\n");
  //Kill this master process
  kill(getpid(), SIGKILL);
}

// handle interrupts
void intHandler(int SIGVAL) {
	signal(SIGQUIT, SIG_IGN);
	signal(SIGINT, SIG_IGN);

	if(SIGVAL == SIGINT) {
		fprintf(stderr, "%sCTRL-C Interrupt initiated.\n");
	}

	if(SIGVAL == SIGALRM) {
		fprintf(stderr, "%sMaster timed out. Terminating rest all process.\n");
	}

	kill(-getpgrp(), SIGQUIT);

}

// help message for running options
void showHelpMessage() {
	printf("-h: Prints this help message.\n");
    printf("-s: Allows you to set the number of child process to run.\n");
    printf("\tThe default value is 5. The max is 20.\n");
    printf("-l: filename for the log file.\n");
    printf("\tThe default value is 'default.log' .\n");
    printf("-t: Allows you set the wait time for the master process until it kills the slaves and itself.\n");
    printf("\tThe default value is 20.\n");
}

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#include "utils.h"
#include "logSpeed.h"

int    keepRunning = 1;       // have we been interrupted
size_t blockSize = 64*1024; // default 
int    exitAfterSeconds = 60; // default timeout
int    resetSeconds = 0; // ignore timeout
int    isSequential = 1;
int    verifyWrites = 0;
float  flushEveryGB = 0;
float  limitGBToProcess = 0;
int    offSetGB = 0;
int    sendTrim = 0;
volatile size_t    trimRemains = 0;

typedef struct {
  int threadid;
  char *path;
  size_t total;
  size_t startPosition;
  size_t exclusive;
  logSpeedType logSpeed;
} threadInfoType;

void intHandler(int d) {
  //  fprintf(stderr,"got signal\n");
  keepRunning = 0;
}
static void *runThread(void *arg) {
  threadInfoType *threadContext = (threadInfoType*)arg; // grab the thread threadContext args
  int mode = O_WRONLY | O_TRUNC | O_DIRECT;
  if (threadContext->exclusive) {
    mode = mode | O_EXCL;
  } else {
    fprintf(stderr, "*warning* %s opened without O_EXCL\n", threadContext->path);
  }
  
  int fd = open(threadContext->path, mode);
  if (fd < 0) {
    perror(threadContext->path);
    return NULL;
  }

  char abs[1000];
  ssize_t l = readlink(threadContext->path, abs, 1000);
  if (l >= 1) {
    abs[l] = 0;
  } else {
    strcpy(abs, threadContext->path);
  }
  
  //  char *suffix = getSuffix(threadContext->path);
  char *suffix = getSuffix(abs);
  char *sched = getScheduler(suffix);
  //fprintf(stderr,"*info* scheduler %s (%s) -> %s\n", threadContext->path, suffix, sched);
  if (sched) free(sched);
  if (suffix) free(suffix);
  
  if (sendTrim) {
    size_t bdsize = blockDeviceSizeFromFD(fd);
    trimDevice(fd, threadContext->path, 0, MIN(1L*1024*1024*1024, bdsize)); // only sending 1GiB for now
    trimRemains--;
    while (trimRemains > 0) {
      // wait
      //    fprintf(stderr,"trim Remains %zd\n", trimRemains);
      usleep(10);
    }
    sleep(2);
  }
  fsync(fd); // sync previous writes before we start timing

  logSpeedInit(&threadContext->logSpeed);

  lseek(fd, threadContext->startPosition, SEEK_SET);
  //fprintf(stderr,"thread %d, lseek fd=%d to pos=%zd\n", threadContext->threadid, fd, threadContext->startPosition);

  int chunkSizes[1] = {blockSize};
  int numChunks = 1;
  
  writeChunks(fd, threadContext->path, chunkSizes, numChunks, exitAfterSeconds, resetSeconds, &threadContext->logSpeed, blockSize, OUTPUTINTERVAL, isSequential, 1, limitGBToProcess, verifyWrites, flushEveryGB); // will close fd
  threadContext->total = threadContext->logSpeed.total;

  return NULL;
}

// sorting function, used by qsort
/*static int poscompare(const void *p1, const void *p2)
{
  threadInfoType *pos1 = (threadInfoType*)p1;
  threadInfoType *pos2 = (threadInfoType*)p2;
  if (logSpeedMean(&pos1->logSpeed) < logSpeedMean(&pos2->logSpeed)) return -1;
  else if (logSpeedMean(&pos1->logSpeed) > logSpeedMean(&pos2->logSpeed)) return 1;
  else return 0;
  }*/

void startThreads(int argc, char *argv[], int index) {
  if (argc - index > 0) {
    
    size_t threads = argc - index;
    pthread_t *pt = (pthread_t*) calloc((size_t) threads, sizeof(pthread_t));
    if (pt==NULL) {fprintf(stderr, "OOM(pt): \n");exit(-1);}

    threadInfoType *threadContext = (threadInfoType*) calloc(threads, sizeof(threadInfoType));
    if (threadContext == NULL) {fprintf(stderr,"OOM(threadContext): \n");exit(-1);}

    int startP = 0;

    // if trim
    for (size_t i = 0; i < threads; i++) {
      if (argv[i + index][0] != '-') {
	//	fprintf(stderr,"processing file %s\n", argv[i+index]);
	trimRemains++;
      }
    }

    
    for (size_t i = 0; i < threads; i++) {
      if (argv[i + index][0] != '-') {
	threadContext[i].path = argv[i + index];
	if (strcmp(threadContext[i].path, "/dev/sda") == 0) {
	  fprintf(stderr,"*warning* possible problem, hope you don't boot off /dev/sda\n");
	}
	threadContext[i].threadid = i;
	threadContext[i].exclusive = 1;
	// check previous to see if they are the same, if so make it non-exclusive
	for (size_t j = 0; j < i; j++) { // check up to this one
	  if (strcmp(threadContext[i].path, argv[j + index]) == 0) {
	    fprintf(stderr,"*info* duplicate %s\n", threadContext[i].path); // warn it's a duplicate
	    exit(1);
	    //	    threadContext[i].exclusive = 0;
	  }
	}

	//	fprintf(stderr,"path %s excl %zd\n", threadContext[i].path, threadContext[i].exclusive);
	    
	threadContext[i].startPosition = (1024L*1024*1024) * startP;
	startP += offSetGB;

	threadContext[i].total = 0;
	//	logSpeedInit(&threadContext[i].logSpeed);
	pthread_create(&(pt[i]), NULL, runThread, &(threadContext[i]));
      }
    }

    
    size_t allbytes = 0;
    double maxtime = 0;
    double allmb = 0;
    size_t minSpeed = (size_t)-1; // actually a big number
    size_t driveCount = 0;
    size_t zeroDrives = 0;
    char *minName = NULL;
    // sort threadContext by logSpeed
    //    qsort(threadContext, threads, sizeof(threadInfoType), poscompare);
    
    for (size_t i = 0; i < threads; i++) {
      if (argv[i + index][0] != '-') {
	pthread_join(pt[i], NULL);
	const double speed = logSpeedMean(&threadContext[i].logSpeed);
	allbytes += threadContext[i].total;
	allmb += speed;
	if (threadContext[i].total > 0) {
	  //	  fprintf(stderr,"NAME %s  %f\n", threadContext[i].path, speed/1024/1024.0);
	  if ((speed < minSpeed) && (speed > 0)) {
	    minSpeed = speed; minName = threadContext[i].path;
	    //	    fprintf(stderr,"MIN !!\n");
	  }
	  driveCount++;
	} else {
	  //	  if () {
	  //	    fprintf(stderr,"*warning* zero bytes to %s\n", threadContext[i].path);
	  //	  }
	  zeroDrives++;
	}
	if (logSpeedTime(&threadContext[i].logSpeed) > maxtime) {
	  maxtime = logSpeedTime(&threadContext[i].logSpeed);
	}
	logSpeedFree(&threadContext[i].logSpeed);
      }
    }
    fprintf(stdout, "*info* worst %s %.1lf MiB/s, synced case %.1lf MiB/s, %zd drives, %zd zero byte drives\n", minName ? minName : "", TOMiB(minSpeed), TOMiB(minSpeed * driveCount), driveCount, zeroDrives);
    fprintf(stdout, "Total write %.1lf GiB bytes, time %.1lf s, max/sum of mean = %.1lf MiB/s\n", TOGiB(allbytes), maxtime, TOMiB(allmb));
    free(threadContext);
    free(pt);
  }
}

int handle_args(int argc, char *argv[]) {
  int opt;
  
  while ((opt = getopt(argc, argv, "r:t:k:vf:o:T:M")) != -1) {
    switch (opt) {
    case 'k':
      if (optarg[0] == '-') {
	fprintf(stderr,"missing argument for '%c' option\n", opt);
	exit(-2);
      }
      if (atoi(optarg) <= 0) {
	fprintf(stderr,"incorrect argument for '%c' option\n", opt);
	exit(-2);
      }
      blockSize=atoi(optarg) * 1024;
      break;
    case 'r': 
      isSequential = 0;
      if (optarg[0] == '-') {
	fprintf(stderr,"missing argument for '%c' option\n", opt);
	exit(-2);
      }
      float l = atof(optarg); if (l < 0) l = 0;
      limitGBToProcess = l;
      break;
    case 't':
      exitAfterSeconds = atoi(optarg);
      break;
    case 'T':
      resetSeconds = atoi(optarg);
      break;
    case 'M':
      sendTrim = 1;
      break;
    case 'f':
      flushEveryGB = atof(optarg);
      break;
    case 'o':
      offSetGB = atoi(optarg);
      if (offSetGB < 0) {
	offSetGB = 0;
      }
      break;
    case 'v':
      verifyWrites = 1;
      break;
    case ':':
      fprintf(stderr,"argument missing for %c \n", optopt);
      exit(-2);
      break;
    default:
      fprintf(stderr,"no minus arg %s\n", optarg);
      exit(-1);
    }
  }
  return optind;
}


int main(int argc, char *argv[]) {

  int index = handle_args(argc, argv);
  signal(SIGTERM, intHandler);
  signal(SIGINT, intHandler);
  fprintf(stderr,"direct=%d, blocksize=%zd (%zd KiB), timeout=%d, resettime=%d, %s", 1, blockSize, blockSize/1024, exitAfterSeconds, resetSeconds, isSequential ? "sequential" : "random");
  if (!isSequential && limitGBToProcess > 0) {
    fprintf(stderr," (limit %.1lf GB)", limitGBToProcess);
  }
  fprintf(stderr,"\n");

  startThreads(argc, argv, index);
  return 0;
}

#define _DEFAULT_SOURCE
#define _GNU_SOURCE
 
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>

#include <pthread.h>

#include "aioRequests.h"
#include "positions.h"
#include "utils.h"
#include "devices.h"

int keepRunning = 1;
int verbose = 0;
int flushEvery = 0;
double MAXRAM = 1.0*1024*1024*1024 ; // 2GB
int tripleX = 0;
size_t waitEvery = 0;    

typedef struct {
  size_t fd;
  size_t id;
  size_t qd;
  size_t max;
  float readRatio;
  size_t total;
  io_context_t *ioc;
  size_t contextCount;
} threadInfoType;

static size_t blockSize = 65536;

volatile int ready = 0;
double startTime = 0;

void intHandler(int d) {
  fprintf(stderr,"got signal\n");
  keepRunning = 0;

}

static void *runThread(void *arg) {
  threadInfoType *threadContext = (threadInfoType*)arg;
  //  fprintf(stderr,"id: %zd\n", threadContext->id);
  
  size_t bdsize = blockDeviceSizeFromFD(threadContext->fd);
  //  fprintf(stderr,"[thread %zd] bdSize %zd (%.2lf GiB)\n", threadContext->id, bdsize, TOGiB(bdsize));

    
  size_t maxBlocks = bdsize / blockSize;
  if (bdsize > 1024.0*1024*1024*1024) { // if over 1TiB 
    maxBlocks = (size_t) (1024.0*1024*1024*1024 / blockSize);
  }
  double RAM = MAXRAM / threadContext->max;
  //  fprintf(stderr,"max ram %lf\n", RAM);
  size_t positionsNum = (RAM / sizeof(positionType)) + 1;
  if (positionsNum > maxBlocks) positionsNum = maxBlocks;
  //  fprintf(stderr,"max positions %zd\n", positionsNum);
  fprintf(stderr,"*info* %.1lf GiB in thread %zd (%.1lf GiB total)/ %zd, bdsize = %zd, positions %zd (%.1lf GiB covered)\n", TOGiB(RAM), threadContext->id, TOGiB(MAXRAM), threadContext->max, blockSize, positionsNum, TOGiB(blockSize * positionsNum));
  positionType *positions = createPositions(positionsNum);
  
  //  int fdA[1];
  //  fdA[0] = threadContext->fd;

  deviceDetails devs[1];
  devs[0].fd = threadContext->fd;
  const size_t devCount = 1;
  //    fprintf(stderr,"fd %d\n", fdA[0]);
  setupPositions(positions, &positionsNum, devs, devCount, 1, threadContext->readRatio, blockSize, blockSize, blockSize, 0, 0, 0, bdsize, 0, NULL, 0);
  
  
  char *randomBuffer;
  CALLOC(randomBuffer, blockSize, 1);
  
  generateRandomBuffer(randomBuffer, blockSize, 0);
  
  size_t ios, trb, twb;
  ready++;
  while (ready != threadContext->max && keepRunning) {
    usleep(100);
  }
  if (threadContext->id == 0) {
    startTime = timeAsDouble();
  }
  //  fprintf(stderr,"go(%zd)\n",threadContext->id);
  threadContext->total = aioMultiplePositions(positions, positionsNum, 100, threadContext->qd, 0, 0, NULL, NULL, randomBuffer, blockSize, blockSize, &ios, &trb, &twb, 0, 0, threadContext->ioc, threadContext->contextCount);

  free(randomBuffer);
  free(positions);


  return NULL;
}




void startThreads(deviceDetails *deviceList, size_t num, size_t qd, float rr, io_context_t *ioc, const size_t contextCount) {
  
  pthread_t *pt;
  CALLOC(pt, num, sizeof(pthread_t));
  assert(pt);

  threadInfoType *threadContext;
  CALLOC(threadContext, num, sizeof(threadInfoType));
  assert(threadContext);

  for (size_t i = 0; i < num; i++) {
    threadContext[i].fd = deviceList[i].fd;
    threadContext[i].id = i;
    threadContext[i].ioc = ioc;
    threadContext[i].contextCount = contextCount;
    threadContext[i].readRatio = rr;
    threadContext[i].qd = qd;
    threadContext[i].max = num;
    pthread_create(&(pt[i]), NULL, runThread, &(threadContext[i]));
  }

  for (size_t i = 0; i < num; i++) {
    pthread_join(pt[i], NULL);
  }

  double elapsed = timeAsDouble() - startTime;
  size_t sum = 0;
  for (size_t i = 0; i <num;i++) {
    sum += threadContext[i].total;
  }
  fprintf(stderr,"*info* summary: %zd devices, readRatio %.1lf, total bytes %zd, %.1lf secs, %.1lf GiB/s (%.0f MiB/s/device)\n", num, rr, sum, elapsed, TOGiB(sum) / elapsed, TOMiB(sum) / elapsed/num);
  free(threadContext);
  free(pt);
}


double readRatio = 1;

int handle_args(int argc, char *argv[]) {

  if (argc <= 1) {
    fprintf(stderr,"./aioMulti [-R ramInGiB] [-r (default)] [-w] ...devices...\n");
    exit(-1);
  } else {
    int opt;
    while ((opt = getopt(argc, argv, "rwR:XV")) != -1) {
      switch (opt) {
      case 'V':
	verbose++;
	break;
      case 'R':
	MAXRAM = atoi(optarg) * 1024L * 1024L * 1024L;
	break;
      case 'r':
	readRatio = 1;
	break;
      case 'w':
	readRatio = 0;
	break;
      case 'X':
	tripleX++;
	break;
      default:
	break;
      }
    }
    return optind;
  }
}


int main(int argc, char *argv[]) {
  deviceDetails *deviceList = NULL;
  size_t deviceCount = 0;

  signal(SIGTERM, intHandler);
  signal(SIGINT, intHandler);

  MAXRAM =totalRAM() /2 ;

  int opt = handle_args(argc, argv);
  if (argc - opt <= 0) {
    fprintf(stderr,"*error* no devices specified\n");
    exit(-1);
  }

  for (size_t i = opt; i < argc; i++) {
    addDeviceDetails(argv[i], &deviceList, &deviceCount);
  }

  const size_t qd = 1024 / deviceCount;
  size_t contextCount = deviceCount;
  io_context_t *ioc = createContexts(contextCount, qd);
  setupContexts(ioc, contextCount, qd);

  fprintf(stderr,"*info* maxRam %.1lf GiB, readRatio %.1f, %zd devices, each with qd=%zd (30,000/%zd)\n", TOGiB(MAXRAM), readRatio, deviceCount, qd, deviceCount);

  size_t maxSizeInBytes = 0;
  openDevices(deviceList, deviceCount, 0, &maxSizeInBytes, 65536, 65536, 4096, readRatio, 0, qd, contextCount);

  // do stuff
  startThreads(deviceList, deviceCount, qd, readRatio, ioc, contextCount);

  freeContexts(ioc, contextCount);
  freeDeviceDetails(deviceList, deviceCount);


  return 0;
}


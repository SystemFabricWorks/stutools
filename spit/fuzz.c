#define _POSIX_C_SOURCE 200809L
#define _ISOC11_SOURCE
#define _DEFAULT_SOURCE

#include "jobType.h"
#include <signal.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h>

/**
 * spit.c
 *
 * the main() for ./fuzz, a RAID stripe aware fuzzer
 *
 */
#include <stdlib.h>
#include <string.h>

#include "positions.h"
#include "utils.h"
#include "diskStats.h"

#define DEFAULTTIME 10
  
int verbose = 0;
int keepRunning = 1;



void zap(size_t pos, deviceDetails *deviceList, size_t deviceCount, char *selection, size_t blocksize, int print, char *block) {
  if (print) fprintf(stderr,"%9x (%5.1lf GiB):  ", (unsigned int)pos, TOGiB(pos));
  size_t mcount = 0;
  for (size_t i = 0; i < deviceCount; i++) {
    if (selection[i]) {
      assert(deviceList[i].fd > 0);
      ssize_t ret = pwrite(deviceList[i].fd, block, blocksize, pos);
      if (ret == blocksize) {
	mcount++;
	if (print) fprintf(stderr,"%2d ", deviceList[i].fd);
      } else {
	//	perror("wow");
      }
    } else {
      if (print) fprintf(stderr,"   ");
    }
  }
  if (print) fprintf(stderr,"\t[%zd]\n", mcount);
}





int main(int argc, char *argv[]) {
  int opt;

  char *device = NULL;
  deviceDetails *deviceList = NULL;
  size_t deviceCount = 0;
  size_t kdevices = 0, mdevices = 0, blocksize = 4096;
  size_t startAt = 16*1024*1024, finishAt = 1024L*1024L*1024L*4;
  unsigned short seed = 0;
  int printevery = 1;
  int tripleX = 0;
  
  optind = 0;
  while ((opt = getopt(argc, argv, "I:k:m:G:g:R:b:qX")) != -1) {
    switch (opt) {
    case 'b':
      blocksize = atoi(optarg);
      break;
    case 'k':
      kdevices = atoi(optarg);
      break;
    case 'G':
      finishAt = 1024L * 1024L * 1024L * atof(optarg);
      break;
    case 'g':
      startAt = 1024L * 1024L * 1024L * atof(optarg);
      break;
    case 'm':
      mdevices = atoi(optarg);
      break;
    case 'R':
      seed = atoi(optarg);
      break;
    case 'q':
      printevery *= 64;
      break;
    case 'I':
      {}
      loadDeviceDetails(optarg, &deviceList, &deviceCount);
      //      fprintf(stderr,"*info* added %zd devices from file '%s'\n", added, optarg);
      break;
    case 'X':
      tripleX++;
      break;
    default:
      exit(1);
      break;
    }
  }

  // first assign the device
  // if one -f specified, add to all jobs
  // if -F specified (with n drives), have c x n jobs
  if (deviceCount) {
    // if we have a list of files, overwrite the first one, ignoring -f
    if (device) {
      fprintf(stderr,"*warning* ignoring the value from -f\n");
    }
    device = deviceList[0].devicename;
  } else {
    fprintf(stderr,"*info* fuzz -I devices.txt -k 10 -m 6 [-R(seed) 0] [-g(start) 0.016] [-G(end) 4] [-b 4096] [-q(uiet)]\n");
    exit(1);
  }

  fprintf(stderr,"*info* k = %zd, m = %zd, device count %zd, seed = %d\n", kdevices, mdevices, deviceCount, seed);
  if (kdevices + mdevices != deviceCount) {
    fprintf(stderr,"*error* k + m need to add to %zd\n", deviceCount);
    exit(1);
  }
  
  size_t maxSizeInBytes = 0;
  openDevices(deviceList, deviceCount, 0, &maxSizeInBytes, 4096, 4096, 4096, 1, tripleX==3, 16, 1);
  infoDevices(deviceList, deviceCount);


  fprintf(stderr,"*info* number of open devices %zd\n", numOpenDevices(deviceList, deviceCount));
  if (numOpenDevices(deviceList, deviceCount) < deviceCount) {
    fprintf(stderr,"*error* not enough devices to open\n");
    exit(1);
  }
  fprintf(stderr,"*info* fuzz range [%.3lf GiB - %.3lf GiB) [%zd - %zd), block size = %zd\n", TOGiB(startAt), TOGiB(finishAt), startAt, finishAt, blocksize);
  srand48(seed);
  char *selection = malloc(deviceCount * sizeof(char));
  
  char *block = aligned_alloc(4096, blocksize);
  memset(block, 'Z', blocksize);
  
  for (size_t pos = startAt,pr=0; pos < finishAt; pos += blocksize,pr++) {
    // pick k
    memset(selection, 0, deviceCount);
    for (size_t i = 0; i < mdevices; i++) {
      int r = lrand48() % deviceCount;
      while (selection[r] != 0) {
	r = lrand48() % deviceCount;
      }
      selection[r] = 1;
    }

    zap(pos, deviceList, deviceCount, selection, blocksize, (pr % printevery)==0, block);
  }
  free(block);
  free(selection);
  
  for (size_t i = 0; i < deviceCount; i++) {
    if (deviceList[i].fd > 0) {
      close(deviceList[i].fd);
    }
  }
  freeDeviceDetails(deviceList, deviceCount);

  fprintf(stderr,"*info* exiting.\n"); fflush(stderr);
  
  exit(0);
}
  
  

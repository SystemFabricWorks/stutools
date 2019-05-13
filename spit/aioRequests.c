#define _GNU_SOURCE

#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <libaio.h>
#include <assert.h>
#include <math.h>

#include "utils.h"
#include "logSpeed.h"
#include "aioRequests.h"

extern volatile int keepRunning;

#define DISPLAYEVERY 1

void checkArray(const unsigned short *freeQueue, const size_t QD) {
  if (0) { // debug print
    fprintf(stderr,"Sub: ");
    for (size_t q1 = 0; q1 < QD; q1++) {
      if (freeQueue[q1] >= 0 && freeQueue[q1] < QD) {
	fprintf(stderr,"%3u", freeQueue[q1]);
      } else {
	fprintf(stderr,"  .");
      }
    }
    fprintf(stderr,"\n");
  }
  // check for incorrect duplications
  for (size_t q1 = 0 ; q1 < QD; q1++) if (freeQueue[q1] >=0 && freeQueue[q1] < QD) {
      for (size_t q2 = 0; q2 < QD; q2++) if (q1!=q2) {
	  if (freeQueue[q1] == freeQueue[q2]) fprintf(stderr,"duplicate %u\n", freeQueue[q1]);
	  assert (freeQueue[q1] != freeQueue[q2]);
	}
    }
}

size_t aioMultiplePositions( positionContainer *p,
			     const size_t sz,
			     const double finishtime,
			     const size_t origQD,
			     const int verbose,
			     const int tableMode, 
			     logSpeedType *alll,
			     logSpeedType *benchl,
			     const char *randomBuffer,
			     const size_t randomBufferSize,
			     size_t alignment,
			     size_t *ios,
			     size_t *totalRB,
			     size_t *totalWB,
			     const size_t oneShot,
			     const int dontExitOnErrors,
			     const int fd,
			     int flushEvery
			     ) {
  int ret;
  struct iocb **cbs;
  struct io_event *events;
  if (origQD > sz)  {
    fprintf(stderr,"*sorry* don't support QD over P\n");
    exit(1);
  }
  assert(origQD <= sz);
  const size_t QD = origQD;
  assert(sz>0);
  positionType *positions = p->positions;

  const double alignbits = log(alignment)/log(2);
  assert (alignbits == (size_t)alignbits);
  
  io_context_t ioc = 0;
  if (io_setup(QD, &ioc)) {
    fprintf(stderr,"*error* io_setup failed with %zd\n", QD);
    exit(-2);
  }
  
  assert(QD);
  if (!alignment) alignment=512;
  assert(alignment);

  CALLOC(events, QD, sizeof(struct io_event));
  CALLOC(cbs, QD, sizeof(struct iocb*));
  for (size_t i = 0; i < QD; i++) {
    CALLOC(cbs[i], 1, sizeof(struct iocb));
  }

  if (verbose >= 1) {
    for (size_t i = 0; i < 1; i++) {
      fprintf(stderr,"*info* io_context[%zd] = %p\n", i, (void*)ioc);
    }
  }
  
  /* setup I/O control block, randomised just for this run. So we can check verification afterwards */
  char **data = NULL;
  CALLOC(data, QD, sizeof(char*));

  // setup the buffers to be contiguous
  if (randomBufferSize * QD >= totalRAM()) {
    fprintf(stderr,"*info* can't allocate (block size %zd x QD %zd) bytes\n", randomBufferSize, QD);
    exit(-1);
  }
  //  if (verbose) fprintf(stderr,"*info* allocating %zd bytes\n", randomBufferSize * QD);
  CALLOC(data[0], randomBufferSize * QD, 1);

  char **readdata = NULL;
  CALLOC(readdata, QD, sizeof(char*));

  // setup the buffers to be contiguous
  //  if (verbose) fprintf(stderr,"*info* allocating %zd bytes\n", randomBufferSize * QD);
  CALLOC(readdata[0], randomBufferSize * QD, 1);

  unsigned short *freeQueue; // qd collisions
  size_t headOfQueue = 0, tailOfQueue = 0;
  CALLOC(freeQueue, QD, sizeof(size_t));
  for (size_t i = 0; i < QD; i++) {
    freeQueue[i] = i;
  }
  
  // grab [headOfQueue], put back onto [tailOfQueue]

  // there are 256 queue slots
  // O(1) next queue slot
  // take a slot, free a slot
  // free list
  // qd[..255] in flight
  
  for (size_t i = 0; i <QD; i++) {
    data[i] = data[0] + (randomBufferSize * i);
    readdata[i] = readdata[0] + (randomBufferSize * i);
  }

  // copy the randomBuffer to each data[]
  for (size_t i = 0; i < QD; i++) {
    if (verbose >= 2) {
      fprintf(stderr,"randomBuffer[%zd]: %p\n", i, (void*)data[i]);
    }
    strncpy(data[i], randomBuffer, randomBufferSize);
  }

  size_t inFlight = 0, pos = 0;

  double start = timedouble();
  double last = start, lastsubmit =start, lastreceive = start;
  logSpeedReset(benchl);
  logSpeedReset(alll);

  size_t submitted = 0, flushPos = 0, received = 0;
  size_t totalWriteBytes = 0, totalReadBytes = 0;
  
  size_t lastBytes = 0, lastIOCount = 0;

  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 10*1000; // 0.0001 seconds

  double flush_totaltime = 0, flush_mintime = 9e99, flush_maxtime = 0;
  size_t flush_count = 0;
  double thistime = 0;
  int qdIndex = 0;

  for (size_t i = 0; i < sz; i++) {
    positions[i].inFlight = 0;
    positions[i].success = 0;
  }
  
  while (keepRunning && ((thistime = timedouble()) < finishtime)) {
    assert (pos < sz);
    if (0) fprintf(stderr,"pos %zd, inflight %zd (%zd %zd)\n", positions[pos].pos, inFlight, tailOfQueue, headOfQueue);
    if (!positions[pos].inFlight) {
      
      // submit requests, one at a time
      if (sz && inFlight < QD) {
	assert(pos < sz);
	if (positions[pos].action != 'S') { // if we have some positions, sz > 0
	  size_t newpos = positions[pos].pos;
	  const size_t len = positions[pos].len;

	  int read = (positions[pos].action == 'R');

	  // check queue, every element should only appear once as it's a queue of slots
	  /*	  if (!(pos & 0xff)) {
	    checkArray(freeQueue, QD);
	    }*/
	    
	  assert(headOfQueue < QD);
	  qdIndex = freeQueue[headOfQueue];
	  assert(qdIndex >= 0);
	  assert(qdIndex < QD);

	  assert(positions[pos].inFlight == 0);

	  // setup the request
	  if (fd >= 0) {
	    positions[pos].q = qdIndex;
	    positions[pos].inFlight = 1;

	    // watermark the block with the position on the device
	    
	    if (read) {
	      if (verbose >= 2) {fprintf(stderr,"[%zd] read qdIndex=%d\n", newpos, qdIndex);}

	      p->readBytes += len;
	      p->readIOs++;

	      io_prep_pread(cbs[qdIndex], fd, readdata[qdIndex], len, newpos);
	      cbs[qdIndex]->data = &positions[pos];


	      totalReadBytes += len;
	    } else {
	      if (verbose >= 2) {fprintf(stderr,"[%zd] write qdIndex=%d\n", newpos, qdIndex);}

	      size_t *posdest = (size_t*)data[qdIndex];
	      *posdest = newpos;

	      size_t *uuiddest = (size_t*)data[qdIndex] + 1;
	      *uuiddest = p->UUID;

	      p->writtenBytes += len;
	      p->writtenIOs++;

	      io_prep_pwrite(cbs[qdIndex], fd, data[qdIndex], len, newpos);
	      cbs[qdIndex]->data = &positions[pos];

	      totalWriteBytes += len;
	      flushPos++;
	    }
	      
	    thistime = timedouble();
	    positions[pos].submittime = thistime;
	      
	    ret = io_submit(ioc, 1, &cbs[qdIndex]);
	      
	    if (ret > 0) {
	      // if success
	      freeQueue[headOfQueue] = -1; // take off queue
	      //	      freeQueue[headOfQueue] = -1;
	      headOfQueue++; if (headOfQueue == QD) headOfQueue = 0;

	      inFlight++;
	      lastsubmit = thistime; // last good submit
	      submitted++;
	      if (verbose >= 2 || (newpos & (alignment - 1))) {
		fprintf(stderr,"fd %d, pos %zd (%% %zd = %zd ... %s), size %zd, inFlight %zd, QD %zd, submitted %zd, received %zd\n", fd, newpos, alignment, newpos % alignment, (newpos % alignment) ? "NO!!" : "aligned", len, inFlight, QD, submitted, received);
	      }
	      
	    } else {
	      fprintf(stderr,"io_submit() failed, ret = %d\n", ret); perror("io_submit()"); if(!dontExitOnErrors) abort();
	    }
	  }
	}
	// onto the next one
	pos++;
	if (pos >= sz) {
	  if (oneShot) {
	    //	      	      fprintf(stderr,"end of function one shot\n");
	    goto endoffunction; // only go through once
	  }
	  pos = 0; // don't go over the end of the array
	}
      } // if > 0 and we can submit
	
      double timeelapsed = thistime - last;
      if (timeelapsed >= DISPLAYEVERY) {
	const double speed = TOMB(1.0*(totalReadBytes + totalWriteBytes - lastBytes) / timeelapsed);
	const double IOspeed = 1.0*(received - lastIOCount) / timeelapsed;
	if (benchl) logSpeedAdd2(benchl, TOMB(totalReadBytes + totalWriteBytes - lastBytes), (received - lastIOCount));
	if (!tableMode) {
	  if (verbose != -1) {
	    //	      fprintf(stderr,"[%.1lf] %.1lf GiB, qd: %zd, op: %zd, [%zd], %.0lf IO/s, %.1lf MB/s\n", gt - start, TOGiB(totalReadBytes + totalWriteBytes), inFlight, received, pos, submitted / (gt - start), speed);
	    fprintf(stderr,"[%.1lf] %.1lf GB, qd: %zd, op: %zd, [%zd], %.0lf IO/s, %.1lf MB/s\n", thistime - start, TOGB(totalReadBytes + totalWriteBytes), inFlight, received, pos, IOspeed, speed);
	  }
	  if (verbose >= 2) {
	    if (flush_count) fprintf(stderr,"*info* avg flush time %.4lf (min %.4lf, max %.4lf)\n", flush_totaltime / flush_count, flush_mintime, flush_maxtime);
	  }
	}
	lastBytes = totalReadBytes + totalWriteBytes;
	lastIOCount = received;
	last = thistime;
      }
    } else {
      //      fprintf(stderr,"already in flight\n");
      pos++;  if (pos >= sz) pos = 0;
      //      usleep(1);
    }// if inflight

    if (flushEvery) {
      if (flushPos >= flushEvery) {
	flushPos = flushPos - flushEvery;
	if (verbose >= 2) {
	  fprintf(stderr,"[%zd] SYNC: calling fsync()\n", pos);
	}
	double start_f = timedouble(); // time and store
	//	  io_prep_fsync(cbs[qdIndex], fd);
	fsync(fd);
	double elapsed_f = timedouble() - start_f;

	flush_totaltime += (elapsed_f);
	flush_count++;
	if (elapsed_f < flush_mintime) flush_mintime = elapsed_f;
	if (elapsed_f > flush_maxtime) flush_maxtime = elapsed_f;
      }
    }

    //    if (inFlight < 5) { // then don't timeout
    //      ret = io_getevents(ioc, 1, QD, events, NULL);
    //    } else {

    ret = io_getevents(ioc, 0, inFlight, events, &timeout);
    //    }
    if (ret > 0) {
      lastreceive = timedouble(); // last good receive

      // verify it's all ok
      int printed = 0;

      for (int j = 0; j < ret; j++) {
	//	struct iocb *my_iocb = events[j].obj;
	if (alll) logSpeedAdd2(alll, TOMB(events[j].res), 1);
	struct iocb *my_iocb = events[j].obj;
	positionType *pp = (positionType*) my_iocb->data;
	assert(pp->inFlight);

	
	int rescode = events[j].res;
	int rescode2 = events[j].res2;

	if ((rescode < 0) || (rescode2 != 0)) { // if return of bytes written or read
	  if (!printed) {
	    fprintf(stderr,"*error* AIO failure codes: res=%d (%s) and res2=%d (%s)\n", rescode, strerror(-rescode), rescode2, strerror(-rescode2));
	    fprintf(stderr,"*error* last successful submission was %.3lf seconds ago\n", timedouble() - lastsubmit);
	    fprintf(stderr,"*error* last successful receive was %.3lf seconds ago\n", timedouble() - lastreceive);
	  }
	  printed = 1;
	  //	  fprintf(stderr,"%ld %s %s\n", events[j].res, strerror(events[j].res2), (char*) my_iocb->u.c.buf);
	} else {
	  //	  fprintf(stderr,"---> %d %d\n", rescode, rescode2);
	  //successful result

	  //	  if (pp->success) {
	  //	    fprintf(stderr,"*warning* AIO duplicate result at position %zd\n", pp->pos);
	  //	  }

	  if (pp->action == 'W') {
	    
	    //	    fprintf(stderr,"[%d] pos %zd verify %d\n", pp->q, pp->pos, pp->verify);
	    // if we know we have written we can check, or if we have read a previous write
	    size_t *uucheck , *poscheck;
	    if (pp->action == 'W') {
	      poscheck = (size_t*)data[pp->q];
	      uucheck = (size_t*)data[pp->q] + 1;
	    } else {
	      poscheck = (size_t*)readdata[pp->q];
	      uucheck = (size_t*)readdata[pp->q] + 1;
	    }
	    
	    if ((p->UUID != *uucheck) || (pp->pos != *poscheck)) {
	      fprintf(stderr,"position (success %d) %zd ver=%d wrong. UUID %zd/%zd, pos %zd/%zd\n", pp->success, pp->pos, pp->verify, p->UUID, *uucheck, pp->pos, *poscheck);
	      //abort();
	    }
	  } // 'W'
	  
	  //	  fprintf(stderr,"received %d\n", pp->q);
	  pp->inFlight = 0;
	  //checkArray(freeQueue, QD);
	  freeQueue[tailOfQueue++] = pp->q; if (tailOfQueue == QD) tailOfQueue = 0;
	  //	    checkArray(freeQueue, QD);
	  pp->finishtime = lastreceive;
	  pp->success = 1; // the action has completed

	}

      } // for j

      if (0) {
	checkArray(freeQueue, QD);
      }

      inFlight -= ret;
      received += ret;
    }
    if (ret < 0) {
      fprintf(stderr,"eek\n");
      break;
    }
  } // while keepRunning

  
 endoffunction:
  // receive outstanding I/Os
  while (inFlight) {
    if (inFlight) {
      //      if (verbose >= 1) {
      //	fprintf(stderr,"*info* inflight = %zd\n", inFlight);
	//      }
      int ret = io_getevents(ioc, inFlight, inFlight, events, NULL);
      if (ret > 0) {
	for (int j = 0; j < ret; j++) {
	  // TODO refactor into the same code as above
	  struct iocb *my_iocb = events[j].obj;
	  positionType *pp = (positionType*) my_iocb->data;

	  freeQueue[tailOfQueue++] = pp->q; if (tailOfQueue == QD) tailOfQueue = 0;
	  
	  pp->finishtime = lastreceive;
	  pp->inFlight = 0;
	  pp->success = 1; // the action has completed
	}
	inFlight -= ret;
      }
    }
  }
  //  fprintf(stderr,"infligtht = %zd\n", inFlight);

  free(events);
  for (size_t i = 0; i < QD; i++) {
    free(cbs[i]);
  }
  free(cbs);

  free(data[0]);
  free(data);
  free(readdata[0]);
  free(readdata);
  free(freeQueue);
  io_destroy(ioc);
  
  *ios = received;

  *totalWB = totalWriteBytes;
  *totalRB = totalReadBytes;

  for (size_t i = 0; i < pos; i++) {
    assert(positions[i].submittime > 0);
  }
  
  return (*totalWB) + (*totalRB);
}


// sorting function, used by qsort
static int poscompare(const void *p1, const void *p2)
{
  const positionType *pos1 = (positionType*)p1;
  const positionType *pos2 = (positionType*)p2;

  if (pos1->pos < pos2->pos) return -1;
  else if (pos1->pos > pos2->pos) return 1;
  else return 0;
}

int aioVerifyWrites(positionType *positions,
		    const size_t maxpos,
		    const size_t maxBufferSize,
		    const size_t alignment,
		    const int verbose,
		    const char *randomBuffer,
		    const int fd) {


  fprintf(stderr,"*info* sorting verification array\n");
  qsort(positions, maxpos, sizeof(positionType), poscompare);

  size_t errors = 0, checked = 0, ioerrors = 0;
  char *buffer;
  CALLOC(buffer, maxBufferSize, sizeof(char));

  size_t bytesToVerify = 0, posTOV = 0;
  for (size_t i = 0; i < maxpos; i++) {
    /*    if (i < 10) {
      fprintf(stderr,"%d %zd %zd\n", positions[i].dev->fd, positions[i].pos, positions[i].len);
      }*/
    if (positions[i].success) {
      if (positions[i].action == 'W') {
	bytesToVerify += positions[i].len;
	posTOV++;
      }
    }
  }

  double start = timedouble();
  fprintf(stderr,"*info* started verification (%zd positions, %.1lf GiB)\n", posTOV, TOGB(bytesToVerify));

  for (size_t i = 0; i < maxpos; i++) {
    if (!keepRunning) break;
    if (positions[i].success) {
      if (positions[i].action == 'W') {
	//	assert(positions[i].len >= 1024);
	//	fprintf(stderr,"\n%zd: %c %zd %zd %d\n", i, positions[i].action, positions[i].pos, positions[i].len, positions[i].success);
	checked++;
	const size_t pos = positions[i].pos;
	const size_t len = positions[i].len;
	assert(len <= maxBufferSize);
	assert(len >= 0);
	
	if (lseek(fd, pos, SEEK_SET) == -1) {
	  if (errors + ioerrors < 10) {
	    fprintf(stderr,"*error* seeking to pos %zd: ", pos);
	    //	    perror("cannot seek");
	  }
	}
	//	buffer[0] = 256 - randomBuffer[0] ^ 0xff; // make sure the first char is different
	int bytesRead = read(fd, buffer, len);
	if (lseek(fd, pos, SEEK_SET) == -1) {
	  if (errors + ioerrors < 10) {
	    fprintf(stderr,"*error* seeking to pos %zd: ", pos);
	    //	    perror("cannot seek");
	  }
	}

	if ((size_t)bytesRead != len) {
	  ioerrors++;
	  if (ioerrors < 10) {
	    //	    perror("reading");
	    fprintf(stderr,"[%zd/%zd][position %zd, %zd/%zd %% %zd] verify read truncated bytesRead=%d instead of len=%zd\n", checked, maxpos, pos, pos / maxBufferSize, pos % maxBufferSize, maxBufferSize, bytesRead, len);
	  }
	} else {
	  assert(bytesRead == len);
	  if (strncmp(buffer, randomBuffer, bytesRead) != 0) {
	    errors++;	
	    if (errors < 10) {
	      char *bufferp = buffer;
	      char *randomp = (char*)randomBuffer;
	      for (size_t p = 0; p < bytesRead; p++) {
		if (*bufferp != *randomp) {
		  fprintf(stderr,"[%zd/%zd][position %zd, %zd/%zd %% %zd] verify error [size=%zd, read=%d] at location (%zd).  '%c'   '%c' \n", checked, maxpos, pos, pos / maxBufferSize, pos % maxBufferSize, maxBufferSize, len, bytesRead, p, buffer[p], randomBuffer[p]);
		  break;
		}
		bufferp++;
		randomp++;
	      }
	    }
	  } else {
	    if (verbose >= 2) {
	      fprintf(stderr,"[%zd] verified ok location %zd, size %zd\n", checked, pos, len);
	    }
	  }
	}
      }
    }
  }
  double elapsed = timedouble() - start;
  fprintf(stderr,"checked %zd/%zd blocks, I/O errors %zd, errors/incorrect %zd, elapsed = %.1lf secs (%.1lf MB/s)\n", checked, posTOV, ioerrors, errors, elapsed, TOMB(bytesToVerify)/elapsed);


  free(buffer);

  return ioerrors + errors;
}




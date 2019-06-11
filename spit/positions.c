#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>

#include "devices.h"
#include "utils.h"
#include "positions.h"

extern int verbose;
extern int keepRunning;

// sorting function, used by qsort
static int poscompare(const void *p1, const void *p2)
{
  const positionType *pos1 = (positionType*)p1;
  const positionType *pos2 = (positionType*)p2;
  if (pos1->pos < pos2->pos) return -1;
  else if (pos1->pos > pos2->pos) return 1;
  else {
    if (pos1->action < pos2->action) return -1;
    else if (pos1->action > pos2->action) return 1;
    else return 0;
  }
}

void calcLBA(positionContainer *pc) {
  size_t t = 0;
  for (size_t i = 0; i < pc->sz; i++) {
    t += pc->positions[i].len;
  }
  pc->LBAcovered = 100.0 * t / pc->bdSize;
}


positionType *createPositions(size_t num) {
  positionType *p;
  if (num == 0) {
    fprintf(stderr,"*warning* createPositions num was 0?\n");
    return NULL;
  }
  //fprintf(stderr,"create positions %zd\n", num);
  CALLOC(p, num, sizeof(positionType));
  return p;
}

void freePositions(positionType *p) {
  free(p);  p = NULL;
}


int checkPositionArray(const positionType *positions, size_t num, const size_t minbdSizeBytes, const size_t bdSizeBytes, size_t exitonerror) {
  fprintf(stderr,"*info*... checking position array with %zd values...\n", num);fflush(stderr);
  
  size_t rcount = 0, wcount = 0;
  size_t sizelow = -1, sizehigh = 0;
  positionType *p = (positionType *)positions;
  positionType *copy = NULL;
  
  for (size_t j = 0; j < num; j++) {
    if (p->len == 0) {
      fprintf(stderr,"len is 0!\n");
      abort();
    }
    if (p->len < sizelow) {
      sizelow = p->len;
    }
    if (p->len > sizehigh) {
      sizehigh = p->len;
    }
    if (p->action == 'R') {
      rcount++;
    } else {
      wcount++;
    }
    p++;
  }

  if (sizelow <= 0) {
    fprintf(stderr,"size low 0!\n");
    abort();
  }
  // check all positions are aligned to low and high lengths
  p = (positionType *) positions;
  if (sizelow > 0) {
    for (size_t j = 0; j < num; j++) {
      if (p->len == 0) {
	fprintf(stderr,"*error* len of 0\n"); 
      }
      //      if ((p->len %sizelow) != 0) {
      //	fprintf(stderr,"*error* len not aligned\n"); 
      //      }
      if (p->pos < minbdSizeBytes) {
	fprintf(stderr,"*error* before the start of the array %zd (%zd)!\n", p->pos, minbdSizeBytes);
      }
	
      if (p->pos + p->len > bdSizeBytes) {
	fprintf(stderr,"*error* off the end of the array %zd + %d is %zd (%zd)!\n", p->pos, p->len, p->pos + p->len, bdSizeBytes); 
      }
      //      if ((p->pos % sizelow) != 0) {
      //	fprintf(stderr,"*error* no %zd aligned position at %zd!\n", sizelow, p->pos); 
      //      }
      //      if ((p->pos % sizehigh) != 0) {
      //	fprintf(stderr,"*error* no %zd aligned position at %zd!\n", sizehigh, p->pos); 
      //      }
      p++;
    }
  }
  // duplicate, sort the array. Count unique positions
  CALLOC(copy, num, sizeof(positionType));
  memcpy(copy, positions, num * sizeof(positionType));
  qsort(copy, num, sizeof(positionType), poscompare);
  // check order
  size_t unique = 0, same = 0;
  for (size_t i = 0; i <num; i++) {
    if (verbose >= 3)    fprintf(stderr,"-----%zd %zd '%c'\n", i, copy[i].pos, copy[i].action);
    if (i==0) continue;
    
    if ((copy[i].pos == copy[i-1].pos) && (copy[i].action == copy[i-1].action)) {
      same++;
    } else {
      unique++;
      if (i==1) { // if the first number checked is different then really it's 2 unique values.
	unique++;
      }
    }
    
    if (copy[i].pos < copy[i-1].pos) {
      if (exitonerror) abort();
      //      if (verbose) fprintf(stderr,"not sorted %zd %zd, unique %zd\n",i, copy[i].pos, unique);
    }
    if (copy[i-1].pos + copy[i-1].len > copy[i].pos) {
      if (exitonerror) abort();
      //      if (verbose) fprintf(stderr,"eerk overlapping %zd %zd %d \n",i, copy[i].pos, copy[i].len);
    }
  }

  
  free(copy); copy = NULL;

  if (verbose)
    fprintf(stderr,"*info* action summary: reads %zd, writes %zd, checked ratio %.1lf, len [%zd, %zd], same %zd, unique %zd\n", rcount, wcount, rcount*1.0/(rcount+wcount), sizelow, sizehigh, same, unique);
  return 0;
}

positionContainer positionContainerCollapse(positionContainer merged, size_t *total) {
  if (verbose >= 1) {
    fprintf(stderr,"*info* collapsing %zd positions\n", *total);
  }
  qsort(merged.positions, *total, sizeof(positionType), poscompare);

  for (size_t i = 0; i < *total; i++) {
    if (toupper(merged.positions[i].action) == 'W' && merged.positions[i].finishtime > 0) {
      if (i>0) assert(merged.positions[i].pos >= merged.positions[i-1].pos);

      size_t j = i+1;
      while ((j < *total) && (merged.positions[i].pos + merged.positions[i].len > merged.positions[j].pos)) {
	// hit
	//	fprintf(stderr,"%zd + %d <  %zd\n", merged.positions[i].pos, merged.positions[i].len,  merged.positions[j].pos);

	if ((toupper(merged.positions[j].action)=='W') && (merged.positions[j].finishtime > 0)) {
	
	  double deltastart = merged.positions[j].submittime - merged.positions[i].submittime;
	  if (deltastart < 0) deltastart = -deltastart;
	
	  if (deltastart < 1) { // if they start within 1 us wknows
	    merged.positions[i].action = 'w'; // exclude from output
	    merged.positions[j].action = 'w'; // exclude from output
	    //	    fprintf(stderr,"deleting %zd and %zd\n", merged.positions[i].pos, merged.positions[j].pos);
	  }
	
	  if (merged.positions[j].submittime >= merged.positions[i].submittime &&
	      merged.positions[j].submittime <= merged.positions[i].finishtime ) {
	    //	fprintf(stderr,"  *error* time overlap\n");
	    merged.positions[i].action = 'w'; // exclude from output
	    merged.positions[j].action = 'w'; // exclude from output
	  }
	  
	  if (merged.positions[j].finishtime >= merged.positions[i].submittime &&
	      merged.positions[j].finishtime <= merged.positions[i].finishtime ) {
	    merged.positions[i].action = 'w'; // exclude from output
	    merged.positions[j].action = 'w'; // exclude from output
	    }
	}
	j++;
      }
    }
  }
  
  // collapse
  size_t left = 0;
  for (size_t i = 0; i < *total; i++) {
    if (merged.positions[i].action != 'X' && merged.positions[i].finishtime > 0) {
      left++;
    }
  }

  positionContainer shrunk;
  positionContainerInit(&shrunk, 0);
  positionContainerSetup(&shrunk, left, merged.device, merged.string);
  left = 0;
  for (size_t i = 0; i < *total; i++) {
    if (merged.positions[i].action != 'X' && merged.positions[i].finishtime > 0) {
      shrunk.positions[left++] = merged.positions[i];
    }
  }
  *total = left;

  for (size_t i = 1 ; i < *total; i++) {
    //    assert(shrunk.positions[i-1].pos + shrunk.positions[i-1].len  <= shrunk.positions[i].pos);
  }
  shrunk.sz = *total;

  positionContainerFree(&merged);
  
  return shrunk;
}
  



positionContainer positionContainerMultiply(positionContainer *original, const size_t multiply) {
  if (multiply == 1) {
    return *original;
  }
  
  positionContainer mult;
  mult = *original; // copy it 
  //  positionContainerInit(&mult, 0);
  positionContainerSetup(&mult, original->sz * multiply, original->device, original->string);

  size_t startpos = 0;
  for (size_t i = 0; i < multiply; i++) {
    memcpy(mult.positions + startpos, original->positions, original->sz * sizeof(positionType));
    startpos += original->sz;
  }
  assert(startpos == mult.sz);

  positionContainerFree(original);
  
  return mult;
}
  




positionContainer positionContainerMerge(positionContainer *p, const size_t numFiles) {
  size_t total = 0;
  size_t lastbd = 0;

  if (numFiles > 0) lastbd = p[0].bdSize;

  for (size_t i = 0; i < numFiles; i++) {
    //fprintf(stderr,"*info* input %zd, size %zd, bdSize %zd\n", i, p[i].sz, p[i].bdSize);
    total += p[i].sz;
    if (i > 0) {
      if (p[i].bdSize != lastbd) {
	if (verbose) {
	  fprintf(stderr,"*warning* not all the devices have the same size (%zd != %zd)\n", p[i].bdSize, lastbd);
	}
	//	exit(1);
      }
    }
    lastbd = p[i].bdSize;
  }
  positionContainer merged;
  positionContainerInit(&merged, 0);
  positionContainerSetup(&merged, total, p[0].device, p[0].string);
  merged.bdSize = lastbd;
  
  size_t startpos = 0;
  for (size_t i = 0; i < numFiles; i++) {
    memcpy(merged.positions + startpos, p[i].positions, p[i].sz * sizeof(positionType));
    startpos += p[i].sz;
  }
  assert(startpos == total);

  positionContainer shrunk = positionContainerCollapse(merged, &total);
  merged = shrunk;

  shrunk = positionContainerCollapse(merged, &total);
  merged = shrunk;

  shrunk = positionContainerCollapse(merged, &total);
  merged = shrunk;

  shrunk = positionContainerCollapse(merged, &total);
  merged = shrunk;

  // find maxbs
  // find minbs;
  size_t maxbs = 0;
  size_t minbs = 0;
  if (total > 0) minbs = merged.positions[0].len;
  for (size_t i = 0; i < total; i++) {
    if (merged.positions[i].len > maxbs) maxbs = merged.positions[i].len;
    if (merged.positions[i].len < minbs) minbs = merged.positions[i].len;
  }
  merged.maxbs = maxbs;
  merged.minbs = minbs;

  //  positionContainerFree(p);
  
  return merged;
}


// lots of checks
void positionContainerSave(const positionContainer *p, const char *name, const size_t bdSizeBytes, const size_t flushEvery) {
  if (name) {
    FILE *fp = fopen(name, "wt");
    if (!fp) {
      perror(name); return;
    }
    const positionType *positions = p->positions;
    for (size_t i = 0; i < p->sz; i++) {
      if (toupper(positions[i].action) == 'W' && positions[i].finishtime > 0 && !positions[i].inFlight) {
	const char action = positions[i].action;
	if (action == 'R' || toupper(action) == 'W') {
	  fprintf(fp, "%s\t%10zd\t%.2lf GiB\t%.1lf%%\t%c\t%u\t%zd\t%.2lf GiB\t%u\t%.8lf\t%.8lf\n", p->device, positions[i].pos, TOGiB(positions[i].pos), positions[i].pos * 100.0 / bdSizeBytes, action, positions[i].len, bdSizeBytes, TOGiB(bdSizeBytes), positions[i].seed, positions[i].submittime, positions[i].finishtime);
	}
	if (flushEvery && ((i+1) % (flushEvery) == 0)) {
	  fprintf(fp, "%s\t%10zd\t%.2lf GiB\t%.1lf%%\t%c\t%zd\t%zd\t%.2lf GiB\t%u\n", p->device, (size_t)0, 0.0, 0.0, 'F', (size_t)0, bdSizeBytes, 0.0, positions[i].seed);
	}
      }
    }
    fclose(fp);
  }
}



// create the position array
size_t setupPositions(positionType *positions,
		    size_t *num,
		    const int sf,
		    const double readorwrite,
		    const size_t lowbs,
		    const size_t bs,
		    size_t alignment,
		    const long startingBlock,
		    const size_t minbdSize,
		    const size_t bdSizeTotal,
		    unsigned short seed
		    ) {

  assert(lowbs <= bs);
  srand48(seed); // set the seed, thats why it was passed

  size_t anywrites = 0;

  if (*num == 0) {
    fprintf(stderr,"*error* setupPositions number of positions can't be 0\n");
  }

  if (verbose) {
    if (startingBlock != -99999) {
      fprintf(stderr,"*info* startingBlock is %ld\n", startingBlock);
    }
  }

  // list of possibles positions
  positionType *poss = NULL;
  size_t possAlloc = *num, count = 0, totalLen = 0;
  CALLOC(poss, possAlloc, sizeof(positionType));

  const int alignbits = (int)(log(alignment)/log(2) + 0.01);
  //  if (1<<alignbits != alignment) {
  //    fprintf(stderr,"*error* alignment of %zd not suitable, changing to %d\n", alignment, 1<<alignbits);
  //    alignment = 1<< alignbits;
  //  }//assert((1<<alignbits) == alignment);

  // setup the start positions for the parallel files
  // with a random starting position, -z sets to 0
  size_t *positionsStart, *positionsEnd;
  const int toalloc = (sf == 0) ? 1 : abs(sf);
  assert(toalloc);
  CALLOC(positionsStart, toalloc, sizeof(size_t));
  CALLOC(positionsEnd, toalloc, sizeof(size_t));
  
  for (size_t i = 0; i < toalloc; i++) {
    positionsStart[i] = alignedNumber(i * (bdSizeTotal / toalloc), lowbs); 
    if (i > 0) positionsEnd[i-1] = positionsStart[i];
    positionsEnd[toalloc-1] = bdSizeTotal;

    if (verbose >= 2) {
      fprintf(stderr,"*info* alignment start %zd: %zd (block %zd)\n", i, positionsStart[i], positionsStart[i] / bs);
    }
  }

  // setup the -P positions

  // do it nice
  count = 0;
  while (count < *num) {
    int nochange = 1;
    for (size_t i = 0; i < toalloc; i++) {
      size_t j = positionsStart[i]; // while in the range
      if (j < positionsEnd[i]) {
	const size_t thislen = randomBlockSize(lowbs, bs, alignbits, lrand48());
	assert(thislen >= 0);

	// grow destination array
	if (count >= possAlloc) {
	  possAlloc = possAlloc * 5 / 4 + 1; // grow 
	  positionType *poss2 = realloc(poss, possAlloc * sizeof(positionType));
	  if (!poss) {fprintf(stderr,"OOM: breaking from setup array\n");break;}
	  else {
	    if (verbose >= 2) {
	      fprintf(stderr,"*info*: new position size %.1lf MB array\n", TOMiB(possAlloc * sizeof(positionType)));
	    }
	    poss = poss2; // point to the new array
	  }
	}

	// if we have gone over the end of the range
	if (j + thislen > positionsEnd[i]) {positionsStart[i] += thislen; break;}

	poss[count].pos = j;
	poss[count].submittime = 0;
	poss[count].finishtime = 0;
	poss[count].len = thislen;
	assert(poss[count].len >= 0);
	//	poss[count].dev = dev;
	poss[count].seed = seed;
	poss[count].verify = 0;
	poss[count].q = 0;
	
	positionsStart[i] += thislen;
	
	count++;
	nochange = 0;
	if (count >= *num) break; // if too many break
      }
    }
    if (nochange) break;
  }
  if (count < *num) {
    if (verbose > 1) {
      fprintf(stderr,"*warning* there are %zd unique positions on the device\n", count);
    }
  }
  *num = count;

  // make a complete copy and rotate by an offset

  //  fprintf(stderr,"starting Block %ld, count %zd\n", startingBlock, count);
  int offset = 0;
  if (count) {
    if (startingBlock == -99999) {
      offset = (lrand48() % count);
    } else {
      offset = startingBlock % count;
    }
  }
  //    fprintf(stderr,"starting offset %d\n", offset);

  // rotate
  for (size_t i = 0; i < count; i++) {
    int index = i + offset;
    if (index >= count) {
      index -= count;
    }
    positions[i] = poss[index];
    if (drand48() <= readorwrite)
      positions[i].action='R';
    else {
      positions[i].action='W';
      anywrites = 1;
    }
    assert(positions[i].len >= 0);
  }

  if (verbose >= 2) {
        fprintf(stderr,"*info* %zd unique positions, max %zd positions requested (-P), %.2lf GiB of device covered (%.0lf%%)\n", count, *num, TOGiB(totalLen), 100.0*TOGiB(totalLen)/TOGiB(bdSizeTotal));
  }
  
  // if randomise then reorder
  if (sf == 0) {
    positionRandomize(positions, count);
  }

  // rotate
  size_t sum = 0;
  positionType *p = positions;
  for (size_t i = 0; i < *num; i++, p++) {
    sum += p->len;
    assert(p->len >= 0);
  }
  if (verbose >= 2) {
    fprintf(stderr,"*info* sum of %zd lengths is %.1lf GiB\n", *num, TOGiB(sum));
  }

  free(poss); poss = NULL;// free the possible locations
  free(positionsStart); positionsStart = NULL;
  free(positionsEnd); positionsEnd = NULL;


  if (minbdSize) {
    fprintf(stderr,"*info* adding position offset of %zd\n", minbdSize);
    
    for (size_t k = 0; k < *num; k++) {
      positions[k].pos += minbdSize;
    }
  }
  
  return anywrites;
}


void positionRandomize(positionType *positions, const size_t count) {
  if (verbose >= 1) {
    fprintf(stderr,"*info* shuffling the array %zd\n", count);
  }
  for (size_t shuffle = 0; shuffle < 1; shuffle++) {
    for (size_t i = 0; i < count; i++) {
      size_t j = i;
      if (count > 1) {
	while ((j = lrand48() % count) == i) {
	  ;
	}
      }
      // swap i and j
      positionType p = positions[i];
      positions[i] = positions[j];
      positions[j] = p;
    }
  }
}


		 
void positionStats(const positionType *positions, const size_t maxpositions, const deviceDetails *devList, const size_t devCount) {
  size_t len = 0;
  for (size_t i = 0; i < maxpositions; i++) {
    len += positions[i].len;
  }
  size_t totalBytes = 0;
  for (size_t i = 0; i <devCount; i++) {
    totalBytes += devList[i].bdSize;
  }

  fprintf(stderr,"*info* %zd positions, %.2lf GiB positions from a total of %.2lf GiB, coverage (%.0lf%%)\n", maxpositions, TOGiB(len), TOGiB(totalBytes), 100.0*TOGiB(len)/TOGiB(totalBytes));
}
    
  
positionType *loadPositions(FILE *fd, size_t *num, deviceDetails **devs, size_t *numDevs, size_t *maxSize) {

  char *line = malloc(200000);
  size_t maxline = 200000;
  ssize_t read;
  char *path;
  CALLOC(path, 1000, 1);
  //  char *origline = line; // store the original pointer, as getline changes it creating an unfreeable area
  positionType *p = NULL;
  size_t pNum = 0;
  double starttime, fintime;
  
  while ((read = getline(&line, &maxline, fd)) != -1) {
    size_t pos, len, seed, tmpsize;
    //    fprintf(stderr,"%zd\n", strlen(line));
    char op;
    starttime = 0;
    fintime = 0;
    
    int s = sscanf(line, "%s %zu %*s %*s %*s %c %zu %zu %*s %*s %zu %lf %lf", path, &pos, &op, &len, &tmpsize, &seed, &starttime, &fintime);
    if (s >= 5) {
      //      fprintf(stderr,"%s %zd %c %zd %zd\n", path, pos, op, len, seed);
      //      deviceDetails *d2 = addDeviceDetails(path, devs, numDevs);
      pNum++;
      p = realloc(p, sizeof(positionType) * (pNum));
      assert(p);
      //      fprintf(stderr,"%zd\n", pNum);
      //      p[pNum-1].fd = 0;
      p[pNum-1].pos = pos;
      p[pNum-1].submittime = starttime;
      p[pNum-1].finishtime = fintime;
      p[pNum-1].len = len;
      p[pNum-1].seed = seed;
      p[pNum-1].q = 0;
      p[pNum-1].action = op;
      p[pNum-1].success = 0;
      p[pNum-1].verify = 0;
      if (tmpsize > *maxSize) {
	*maxSize = tmpsize;
      }

      //      fprintf(stderr,"added %p\n", p[pNum-1].dev);
      
    //    addDeviceDetails(line, devs, numDevs);
    //    addDeviceToAnalyse(line);
    //    add++;
    //    printf("%s", line);
    }
  }
  fflush(stderr);

  free(line); line = NULL;
  free(path); path = NULL;
  *num = pNum;
  
  return p;
}

  
void dumpPositions(positionType *positions, const char *prefix, const size_t num, const size_t countToShow) {
  fprintf(stderr,"%s: total number of positions %zd\n", prefix, num);
  for (size_t i = 0; i < num; i++) {
    if (i >= countToShow) break;
    fprintf(stderr,"%s: [%02zd] action %c pos %9zd len %6d verify %d\n", prefix, i, positions[i].action, positions[i].pos, positions[i].len, positions[i].verify);
  }
}


void positionContainerInit(positionContainer *pc, size_t UUID) {
  memset(pc, 0, sizeof(positionContainer));
  pc->UUID = UUID;
}

void positionContainerSetup(positionContainer *pc, size_t sz, char *deviceString, char *string) {
  pc->sz = sz;
  pc->positions = createPositions(sz);
  pc->device = deviceString;
  pc->string = string;
}


void positionContainerAddMetadataChecks(positionContainer *pc) {
  size_t origsz = pc->sz;
  pc->positions = realloc(pc->positions, (origsz * 2) * sizeof(positionType));
  if (!pc->positions) {
    fprintf(stderr,"*error* can't realloc array to %zd\n", (origsz * 2) * sizeof(positionType));
    exit(-1);
  }

  for (size_t i = origsz; i < origsz * 2; i++) {
    pc->positions[i] = pc->positions[i - origsz];
    if (pc->positions[i].action == 'W') {
      pc->positions[i].action = 'R';
      pc->positions[i].verify = 1;
    }
  }

  pc->sz = origsz * 2;
}

void positionContainerFree(positionContainer *pc) {
  if (pc->positions) free(pc->positions);
  pc->positions = NULL;
}


// lots of checks
/*void positionContainerSave(const positionContainer *pc, const char *name, const size_t flushEvery) {
  if (name) {
    FILE *fp = fopen(name, "wt");
    if (!fp) {
      perror(name); return;
    }
    for (size_t i = 0; i < pc->sz; i++) {
      if (1 && pc->positions[i].success) {
	size_t bdSizeBytes = pc->dev->shouldBeSize;
	const char action = pc->positions[i].action;
	if (action == 'R' || action == 'W') {
	  fprintf(fp, "%s\t%10zd\t%.2lf GiB\t%.1lf%%\t%c\t%u\t%zd\t%.2lf GiB\t%ld\n", pc->dev->devicename, pc->positions[i].pos, TOGiB(pc->positions[i].pos), pc->positions[i].pos * 100.0 / bdSizeBytes, action, pc->positions[i].len, bdSizeBytes, TOGiB(bdSizeBytes), pc->positions[i].seed);
	}
	if (flushEvery && ((i+1) % (flushEvery) == 0)) {
	  fprintf(fp, "%s\t%10zd\t%.2lf GiB\t%.1lf%%\t%c\t%zd\t%zd\t%.2lf GiB\t%ld\n", pc->dev->devicename, (size_t)0, 0.0, 0.0, 'F', (size_t)0, pc->dev->bdSize, 0.0, pc->positions[i].seed);
	}
      }
    }
    fclose(fp);
  }
}

*/

void positionLatencyStats(positionContainer *pc, const int threadid) {
  size_t failed = 0;
  
  for (size_t i = 0; i < pc->sz;i++) {
    if (pc->positions[i].success && pc->positions[i].finishtime) {
      //
    } else {
      if (pc->positions[i].submittime) {
	failed++;
      }
    }
  }
  double elapsed = pc->elapsedTime;

  fprintf(stderr,"*info* [T%d] '%s': R %.0lf MB/s (%.0lf IO/s), W %.0lf MB/s (%.0lf IO/s), %.1lf s\n", threadid, pc->string, TOMB(pc->readBytes/elapsed), pc->readIOs/elapsed, TOMB(pc->writtenBytes/elapsed), pc->writtenIOs/elapsed, elapsed);
  if (verbose >= 2) {
    fprintf(stderr,"*failed or not finished* %zd\n", failed);
  }
  fflush(stderr);
}
  
size_t setupRandomPositions(positionType *pos,
			  const size_t num,
			  const double rw,
			  const size_t bs,
			  const size_t highbs,
			  const size_t alignment,
			  const size_t bdSize,
			  const size_t seedin) {
  unsigned int seed = seedin;
  const int alignbits = (int)(log(alignment)/log(2) + 0.01);
  const int bdSizeBits = (bdSize-highbs) >> alignbits;
  size_t anywrites = 0;

  for (size_t i = 0; i < num; i++) {
    long low = rand_r(&seed);
    long randVal = rand_r(&seed);
    randVal = (randVal << 31) | low;

    size_t thislen = randomBlockSize(bs, highbs, alignbits, randVal);

    low = rand_r(&seed);
    randVal = rand_r(&seed);
    randVal = (randVal << 31) | low;

    size_t randPos = (randVal % bdSizeBits) << alignbits;

    assert (randPos + thislen <= bdSize);
    pos[i].pos = randPos;
    pos[i].submittime = 0;
    pos[i].finishtime= 0;
    pos[i].len = thislen;
    pos[i].seed = seedin;
    pos[i].q = 0;

    if (rand_r(&seed) % 100 < 100*rw) {
      pos[i].action = 'R';
    } else {
      pos[i].action = 'W';
      anywrites = 1;
    }
    pos[i].success = 0;
    pos[i].verify = 0;
    //    fprintf(stderr,"%zd\t%zd\t%c\n",randPos, thislen, pos[i].action);
  }
  return anywrites;
}



void positionContainerLoad(positionContainer *pc, FILE *fd) {

  positionContainerInit(pc, 0);
  
  char *line = malloc(200000);
  size_t maxline = 200000, maxSize = 0, minbs = (size_t)-1, maxbs = 0;
  ssize_t read;
  char *path;
  CALLOC(path, 1000, 1);
  //  char *origline = line; // store the original pointer, as getline changes it creating an unfreeable area
  positionType *p = NULL;
  size_t pNum = 0;
  double starttime, fintime;

  while ((read = getline(&line, &maxline, fd)) != -1) {
    size_t pos, len, seed, tmpsize;
    //    fprintf(stderr,"%zd\n", strlen(line));
    char op;
    starttime = 0;
    fintime = 0;
    
    int s = sscanf(line, "%s %zu %*s %*s %*s %c %zu %zu %*s %*s %zu %lf %lf", path, &pos, &op, &len, &tmpsize, &seed, &starttime, &fintime);
    if (s >= 5) {
      //      fprintf(stderr,"%s %zd %c %zd %zd\n", path, pos, op, len, seed);
      //      deviceDetails *d2 = addDeviceDetails(path, devs, numDevs);
      pNum++;
      p = realloc(p, sizeof(positionType) * (pNum));
      assert(p);
      p[pNum-1].submittime = starttime;
      p[pNum-1].finishtime = fintime;
      //      fprintf(stderr,"%zd\n", pNum);
      //      p[pNum-1].fd = 0;
      p[pNum-1].pos = pos;
      p[pNum-1].len = len;
      if (len < minbs) minbs = len;
      if (len > maxbs) maxbs = len;
      p[pNum-1].action = op;
      p[pNum-1].success = 1;
      p[pNum-1].seed = seed;
      if (tmpsize > maxSize) {
	maxSize = tmpsize;
      }

      //      fprintf(stderr,"added %p\n", p[pNum-1].dev);
      
    //    addDeviceDetails(line, devs, numDevs);
    //    addDeviceToAnalyse(line);
    //    add++;
    //    printf("%s", line);
    }
  }
  fflush(stderr);

  free(line); line = NULL;
  
  pc->positions = p;
  pc->sz = pNum;
  pc->string = strdup("");
  pc->device = path;
  pc->bdSize = maxSize;
  pc->minbs = minbs;
  pc->maxbs = maxbs;
}

void positionContainerInfo(const positionContainer *pc) {
  assert(pc->device);
  fprintf(stderr,"device '%s', UUID '%zd', number of positions %zd, device size %zd (%.3lf GiB), k [%zd,%zd]\n", pc->device, pc->UUID, pc->sz, pc->bdSize, TOGiB(pc->bdSize), pc->minbs, pc->maxbs);
}




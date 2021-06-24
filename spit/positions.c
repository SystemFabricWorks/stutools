#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <sys/ioctl.h>

#include "devices.h"
#include "utils.h"
#include "positions.h"
#include "lengths.h"

extern int verbose;
extern int keepRunning;

// sorting function, used by qsort
// sort deviceid, then positions
int poscompare(const void *p1, const void *p2)
{
  const positionType *pos1 = (positionType*)p1;
  const positionType *pos2 = (positionType*)p2;

  //  assert(pos1->submitTime); assert(pos2->submitTime); assert(pos1->finishTime); assert(pos2->finishTime);
  if (pos1->deviceid < pos2->deviceid) return -1;
  else if (pos1->deviceid > pos2->deviceid) return 1;
  else { // same deviceid
    if (pos1->pos < pos2->pos) return -1;
    else if (pos1->pos > pos2->pos) return 1;
    else { // same pos
      if (pos1->len < pos2->len) return -1;
      else if (pos1->len > pos2->len) return 1;
      else { // same len
        if (pos1->finishTime > pos2->finishTime) return -1;
        else if (pos1->finishTime < pos2->finishTime) return 1;
        else return 0;
      }
    }
  }
}

void calcLBA(positionContainer *pc)
{
  size_t t = 0;
  for (size_t i = 0; i < pc->sz; i++) {
    t += pc->positions[i].len;
  }
  pc->LBAcovered = 100.0 * t / pc->maxbdSize;
}


positionType *createPositions(size_t num)
{
  positionType *p;
  if (num == 0) {
    fprintf(stderr,"*warning* createPositions num was 0\n");
    return NULL;
  }
  //  fprintf(stderr,"create/calloc positions %zd\n", num);
  CALLOC(p, num, sizeof(positionType));
  return p;
}

int positionContainerCheck(const positionContainer *pc, const size_t minmaxbdSizeBytes, const size_t maxbdSizeBytes, size_t exitonerror)
{
  const size_t num = pc->sz;
  const positionType *positions = pc->positions;
  if (verbose) fprintf(stderr,"*info*... checking position array with %zd values...\n", num);
  fflush(stderr);

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
      if (p->pos < minmaxbdSizeBytes) {
        fprintf(stderr,"*error* before the start of the array %zd (%zd)!\n", p->pos, minmaxbdSizeBytes);
      }

      if (p->pos + p->len > maxbdSizeBytes) {
        fprintf(stderr,"*error* off the end of the array %zd + %d is %zd (%zd)!\n", p->pos, p->len, p->pos + p->len, maxbdSizeBytes);
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
      if (verbose) fprintf(stderr,"not sorted %zd %zd, unique %zd\n",i, copy[i].pos, unique);
      if (exitonerror) abort();
    }
    if (copy[i-1].pos != copy[i].pos) {
      if (copy[i-1].pos + copy[i-1].len > copy[i].pos) {
        if (verbose) fprintf(stderr,"eerk overlapping %zd %zd %d \n",i, copy[i].pos, copy[i].len);
        if (exitonerror) abort();
      }
    }
  }


  free(copy);
  copy = NULL;

  if (verbose)
    fprintf(stderr,"*info* action summary: reads %zd, writes %zd, checked ratio %.1lf, len [%zd, %zd], same %zd, unique %zd\n", rcount, wcount, rcount*1.0/(rcount+wcount), sizelow, sizehigh, same, unique);
  return 0;
}

void positionContainerCheckOverlap(const positionContainer *merged)
{

  if (verbose) {
    size_t readcount = 0, writecount = 0, notcompletedcount = 0, conflict = 0, trimcount = 0;
    positionType *pp = merged->positions;
    for (size_t i = 0; i < merged->sz; i++,pp++) {
      if (pp->finishTime == 0 || pp->submitTime == 0) {
        notcompletedcount++;
      } else {
        if (pp->action == 'W') writecount++;
        else if (pp->action == 'R') readcount++;
        else if (pp->action == 'T') trimcount++;
        else conflict++;
      }
    }
    fprintf(stderr,"*info* checkOverlap: reads %zd, writes %zd, trims %zd, conflicts %zd, not completed %zd\n", readcount, writecount, trimcount, conflict, notcompletedcount);
  }

  // check they are sorted based on position, otherwise we can't prune
  if (merged->sz > 1) {
    for (size_t i = 0; i < merged->sz - 1; i++) {
      if (merged->positions[i].deviceid == merged->positions[i+1].deviceid) {
        assert(merged->positions[i].pos <= merged->positions[i+1].pos);
      }
    }

    size_t printed = 0;
    for (size_t i = 0; i < merged->sz - 1; i++) {
      if ((merged->positions[i].action == 'W' && merged->positions[i+1].action == 'W') && (merged->positions[i].deviceid == merged->positions[i+1].deviceid)) {
        int pe = 0;
        if (merged->positions[i].pos > merged->positions[i+1].pos) pe = 1;
        if (pe) {
          fprintf(stderr,"[%zd] %zd len %d %c seed %d device %d\n", i, merged->positions[i].pos, merged->positions[i].len, merged->positions[i].action, merged->positions[i].seed, merged->positions[i].deviceid);
          i++;
          fprintf(stderr,"[%zd] %zd len %d %c seed %d device %d\n", i, merged->positions[i].pos, merged->positions[i].len, merged->positions[i].action, merged->positions[i].seed, merged->positions[i].deviceid);
          i--;
        }

        if (merged->positions[i].seed != merged->positions[i+1].seed) {
          if (merged->positions[i].pos + merged->positions[i].len > merged->positions[i+1].pos) {
            if (merged->positions[i].finishTime > 0 && merged->positions[i+1].finishTime > 0) {
              if (merged->positions[i].finishTime < merged->positions[i+1].finishTime) {
                printed++;
                if (printed < 10)
                  fprintf(stderr,"[%zd] *warning* problem at position %zd (len %d) %c, next %zd (len %d) %c\n", i, merged->positions[i].pos, merged->positions[i].len,  merged->positions[i].action, merged->positions[i+1].pos, merged->positions[i+1].len, merged->positions[i+1].action);
                //	  abort();
              }
            }
          }
        }
      }
    }
  }
}



void positionContainerCollapse(positionContainer *merged)
{
  fprintf(stderr,"*info* remove action conflicts (%zd raw positions)\n", merged->sz);
  size_t  newstart = 0;
  for (size_t i =0 ; i < merged->sz; i++) {
    if (merged->positions[i].finishTime != 0) {
      if (newstart != i) {
        merged->positions[newstart] = merged->positions[i];
      }
      newstart++;
    }
  }
  merged->sz = newstart;

  fprintf(stderr,"*info* sorting %zd actions that have completed\n", merged->sz);
  qsort(merged->positions, merged->sz, sizeof(positionType), poscompare);

  /*  fprintf(stderr,"*info* checking pre-conditions for collapse()\n");
  positionType *pi = merged->positions;
  for (size_t i =0 ; i < merged->sz-1; i++) {
    positionType *pj = pi+1;
    assert(pi->pos <= pj->pos);
    if (pi->pos + pi->len < pj->pos) { // if this overlaps with the next
      // most recent time first
      if (pi->finishTime && pj->finishTime)
  assert(pi->finishTime <= pj->finishTime);
    }
    }*/

  //  for (size_t i = 0; i < merged->sz; i++) {
  //    fprintf(stderr,"[%zd] pos %zd, len %d, action '%c'\n", i, merged->positions[i].pos, merged->positions[i].len, merged->positions[i].action);
  //  }

#ifdef DEBUG
  size_t maxbs = merged->maxbs;
  assert(maxbs > 0);
#endif

  for (size_t i = 0; i < merged->sz; i++) {
    if ((toupper(merged->positions[i].action) != 'R') && (merged->positions[i].finishTime > 0)) {
      size_t j = i;
      // iterate upwards
      // if j pos is past i + len then exit
      while ((++j < merged->sz) && (merged->positions[j].pos < merged->positions[i].pos + merged->positions[i].len) && (merged->positions[i].deviceid == merged->positions[j].deviceid)) {

        if ((merged->positions[j].action == 'R') || (merged->positions[j].finishTime == 0)) {
          // if it's a not a write, move to the next one
          continue;
        }

        assert(j > i);
        assert(merged->positions[i].deviceid == merged->positions[j].deviceid);
        assert(merged->positions[i].pos <= merged->positions[j].pos);
        if (merged->positions[i].pos == merged->positions[j].pos) {
          assert(merged->positions[i].len <= merged->positions[j].len);
        }


        int seedsame = merged->positions[i].seed == merged->positions[j].seed;
        // if the same thread wrote to the same location with the same seed, they are OK
        if (merged->positions[i].pos == merged->positions[j].pos) {
          if (merged->positions[i].len == merged->positions[j].len) {
            if (seedsame) {
              continue;
            }
          }
        }


        if (merged->positions[j].pos < merged->positions[i].pos + merged->positions[i].len) { // should always be true

          // there is some conflict to check
          size_t overlap = 0;
          if (merged->positions[i].finishTime <= merged->positions[j].submitTime) {
            overlap = '1';
            merged->positions[i].action = overlap;
          } else if (merged->positions[i].submitTime >= merged->positions[j].finishTime) {
            overlap = '2';
            merged->positions[j].action = overlap;
          } else if (merged->positions[i].submitTime >= merged->positions[j].submitTime) {
            overlap = '3';
            merged->positions[i].action = overlap;
            merged->positions[j].action = overlap;
          } else if (merged->positions[i].submitTime <= merged->positions[j].submitTime) {
            overlap = '4';
            merged->positions[i].action = overlap;
            merged->positions[j].action = overlap;
          }
          //	fprintf(stderr,"*maybe2* %zd/%d/%d and %zd/%d/%d\n", merged->positions[i].pos, merged->positions[i].seed, merged->positions[i].deviceid, merged->positions[j].pos, merged->positions[j].seed, merged->positions[j].deviceid);

        } else {
          abort();
        }
      }
    }
  }
  size_t actionsr = 0, actionsw = 0, conflicts = 0, actionst = 0;
  positionType *pp = merged->positions;
  for (size_t i = 0; i < merged->sz; i++,pp++) {
    char action = pp->action;
    if (action == 'R') actionsr++;
    else if (action == 'W') actionsw++;
    else if (action == 'T') actionst++;
    else conflicts++;
  }
  fprintf(stderr,"*info* unique actions: reads %zd, writes %zd, trims %zd, conflicts %zd\n", actionsr, actionsw, actionst, conflicts);

  //    for (size_t i = 0; i < merged->sz; i++) {
  //    fprintf(stderr,"[%zd] pos %zd, len %d, action '%c'\n", i, merged->positions[i].pos, merged->positions[i].len, merged->positions[i].action);
  //  }

}




positionContainer positionContainerMultiply(const positionContainer *original, const size_t multiply)
{
  positionContainer mult;
  //  mult = *original; // copy it
  //  positionContainerInit(&mult, 0);
  positionContainerSetupFromPC(&mult, original);

  mult.sz = original->sz * multiply;
  mult.positions = createPositions(mult.sz);

  size_t startpos = 0;
  for (size_t i = 0; i < multiply; i++) {
    memcpy(mult.positions + startpos, original->positions, original->sz * sizeof(positionType));
    startpos += original->sz;
  }
  assert(startpos == mult.sz);

  return mult;
}



positionContainer positionContainerMerge(positionContainer *p, const size_t numFiles)
{
  size_t total = 0;
  size_t lastbd = 0;


  size_t maxbd = 0;
  if (numFiles > 0) {
    lastbd = (p[0].maxbdSize - p[0].minbdSize);
    maxbd = p[0].maxbdSize;
  }

  for (size_t i = 0; i < numFiles; i++) {
    if (verbose) {
      fprintf(stderr,"*info* containerMerge input %zd, size %zd, bdSize [%.3lf, %.3lf]\n", i, p[i].sz, TOGiB(p[i].minbdSize), TOGiB(p[i].maxbdSize));
    }
    total += p[i].sz;
    if (i > 0) {
      long diff = (long)p[i].maxbdSize - (long)p[i].minbdSize;
      diff = diff - lastbd;

      if (ABS(diff) > 4096) {
        if (verbose) {
          fprintf(stderr,"*warning* not all the devices have the same size (%zd != %zd)\n", p[i].maxbdSize - p[i].minbdSize, lastbd);
        }
        //	exit(1);
      }
    }
    lastbd = p[i].maxbdSize - p[i].minbdSize;
    if (p[i].maxbdSize > maxbd) maxbd = p[i].maxbdSize;
  }
  positionContainer merged;
  positionContainerInit(&merged, 0);
  positionContainerSetupFromPC(&merged, p);
  merged.sz = total;
  merged.positions = createPositions(total);
  merged.maxbdSize = maxbd;

  size_t startpos = 0;
  for (size_t i = 0; i < numFiles; i++) {
    memcpy(merged.positions + startpos, p[i].positions, p[i].sz * sizeof(positionType));
    startpos += p[i].sz;
  }
  assert(startpos == total);

  size_t maxbs = 0;
  size_t minbs = 0;
  if (total > 0) minbs = merged.positions[0].len;
  for (size_t i = 0; i < total; i++) {
    if (merged.positions[i].len > maxbs) maxbs = merged.positions[i].len;
    if (merged.positions[i].len < minbs) minbs = merged.positions[i].len;
  }
  merged.maxbs = maxbs;
  merged.minbs = minbs;

  positionContainerCollapse(&merged);

  return merged;
}


void positionDumpOne(FILE *fp, const positionType *p, const size_t maxbdSizeBytes, const size_t doflush, const char *name) {
  //  int nbytes;
  //  ioctl(fileno(fp), FIONREAD, &nbytes);
  //  fprintf(stderr,"%d\n", nbytes);
  
  if (0 || (p->finishTime > 0 && !p->inFlight)) {
    const char action = p->action;
    fprintf(fp, "%s\t%10zd\t%.2lf GiB\t%.1lf%%\t%c\t%u\t%zd\t%.2lf GiB\t%u\t%.8lf\t%.8lf\n", name, p->pos, TOGiB(p->pos), p->pos * 100.0 / maxbdSizeBytes, action, p->len, maxbdSizeBytes, TOGiB(maxbdSizeBytes), p->seed, p->submitTime, p->finishTime);
    if (doflush) {
      fprintf(fp, "%s\t%10zd\t%.2lf GiB\t%.1lf%%\t%c\t%zd\t%zd\t%.2lf GiB\t%u\n", name, (size_t)0, 0.0, 0.0, 'F', (size_t)0, maxbdSizeBytes, 0.0, p->seed);
    }
  }
  if (fp==stdout) {
    fflush(fp);
  }
}
  

// lots of checks
void positionContainerSave(const positionContainer *p, FILE *fp, const size_t maxbdSizeBytes, const size_t flushEvery, jobType *job)
{
  if (fp) {
    //    fprintf(stderr,"*info* saving positions to '%s' ...\n", name);
    //    FILE *fp = fopen(name, "wt");
    //if (!fp) {
    //      fprintf(stderr,"*error* saving file '%s' failed.\n", name);
    //      perror(name);
    //      return;
    //    }
    //    CALLOC(buffer, 1000000, 1);
    // setvbuf(fp, NULL, 0, _IONBF);
    const positionType *positions = p->positions;

    for (size_t i = 0; i < p->sz; i++) {
      positionDumpOne(fp, &positions[i], maxbdSizeBytes, flushEvery && ((i+1) % (flushEvery) == 0), job ? job->devices[positions[i].deviceid] : "");
    }
    //    fclose(fp);
    //    fp = NULL;
    //    free(buffer);
  }
}

// html display
void positionContainerHTML(positionContainer *p, const char *name)
{
  if (name) {
    FILE *fp = fopen(name, "wt");
    if (!fp) {
      fprintf(stderr,"*error* saving file '%s' failed.\n", name);
      perror(name);
      return;
    }

    positionContainerCollapse(p);

    fprintf(fp, "<html>\n");
    fprintf(fp, "<html><table border=1>\n");
    for (size_t i = 0; i < p->sz; i++) {
      size_t p1 = p->positions[i].pos;
      size_t j = i;
      while (++j < p->sz) {
        if (p->positions[j].pos != p1) break;
      }
      fprintf(fp,"<tr> <!-- [%zd ... %zd] pos %zd -->\n", i, j-1, p->positions[i].pos);

      for (size_t z = i; z < j; z++ ) {
        fprintf(fp," <td valign=top rowspan=%d>%zd</td>\n", p->positions[z].len / 4096, p->positions[z].pos);
      }
      fprintf(fp,"</tr>\n");
      i=j-1;
    }
    fprintf(fp, "</table></html>\n");
    fclose(fp);
  }
}


// create the position array
size_t positionContainerCreatePositions(positionContainer *pc,
                                        const unsigned short deviceid,
                                        const int sf,
                                        const size_t sf_maxsizebytes,
                                        const probType readorwrite,
                                        const lengthsType *len,
                                        //const size_t lowbs,
                                        //					const size_t bs,
                                        size_t alignment,
                                        const long startingBlock,
                                        const size_t minbdSize,
                                        const size_t maxbdSize,
                                        unsigned short seedin,
                                        const size_t mod,
                                        const size_t remain,
                                        const double fourkEveryMiB,
                                        const size_t jumpKiB,
                                        const size_t firstPPositions
                                       )
{

  positionType *positions = pc->positions;
  pc->minbs = lengthsMin(len);
  pc->maxbs = lengthsMax(len);
  pc->maxbdSize = maxbdSize;
  if (fourkEveryMiB) {
    fprintf(stderr,"*info* inserting a 4KiB operation every %.1lf MiB\n", fourkEveryMiB);
  }

  assert(pc->minbs <= pc->maxbs);
  unsigned int seed = seedin; // set the seed, thats why it was passed
  srand48(seed);

  size_t anywrites = 0, randomSubSample = 0;

  if (((pc->sz) * ((pc->minbs + pc->maxbs)/2)) < (maxbdSize - minbdSize) * 0.95) {
    // if we can't get good coverage
    if ((sf == 0) && (firstPPositions == 0) && (pc->minbs == pc->maxbs) && (pc->minbs == alignment)) {
      if (verbose >= 2) fprintf(stderr,"*info* using randomSubSample (with replacement) mode\n");
      //randomSubSample = 1;
      //xxx bad idea with overlapping block ranges
    }
  }

  //  if (firstPPositions) {
  //    randomSubSample = 1;
  //  }

  if (pc->sz == 0) {
    fprintf(stderr,"*error* setupPositions number of positions can't be 0\n");
  }

  if (mod != 1) {
    fprintf(stderr,"*info* modulo %zd, remain %zd\n", mod, remain);
  }

  if (startingBlock < 0) {
    if (startingBlock != -99999)
      fprintf(stderr,"*info* starting position offset is %ld\n", startingBlock);
  }

  if (verbose >= 2) {
    lengthsDump(len);
  }

  //  fprintf(stderr,"alloc %zd = %zd\n", *num, *num * sizeof(positionType));

  // list of possibles positions
  positionType *poss = NULL;
  size_t possAlloc = pc->sz, count = 0, totalLen = 0;
  CALLOC(poss, possAlloc, sizeof(positionType));

  const int alignbits = (int)(log(alignment)/log(2) + 0.01);
  //  if (1<<alignbits != alignment) {
  //    fprintf(stderr,"*error* alignment of %zd not suitable, changing to %d\n", alignment, 1<<alignbits);
  //    alignment = 1<< alignbits;
  //  }//assert((1<<alignbits) == alignment);

  // setup the start positions for the parallel files
  // with a random starting position, -z sets to 0
  size_t *positionsStart, *positionsEnd, *positionsCurrent;
  const int toalloc = (sf == 0) ? 1 : abs(sf);
  assert(toalloc);
  CALLOC(positionsStart, toalloc, sizeof(size_t));
  CALLOC(positionsCurrent, toalloc, sizeof(size_t));
  CALLOC(positionsEnd, toalloc, sizeof(size_t));

  double ratio = 1.0 * (maxbdSize - minbdSize) / toalloc;
  //  fprintf(stderr,"ratio %lf\n", ratio);

  double totalratio = minbdSize;
  for (int i = 0; i < toalloc; i++) {
    positionsStart[i] = alignedNumber((size_t)totalratio, alignment);
    totalratio += ratio;
  }
  for (int i = 1; i < toalloc; i++) {
    positionsEnd[i-1] = positionsStart[i];
  }
  positionsEnd[toalloc-1] = maxbdSize;


  if (verbose >= 2) {
    size_t sumgap = 0;
    for (int i = 0; i < toalloc; i++) {
      if (positionsEnd[i] - positionsStart[i])
        fprintf(stderr,"*info* range[%d]  [%12zd, %12zd]... size = %zd\n", i+1, positionsStart[i], positionsEnd[i], positionsEnd[i] - positionsStart[i]);
      sumgap += (positionsEnd[i] - positionsStart[i]);
    }
    fprintf(stderr,"*info* sumgap %zd, min %zd, max %zd\n", sumgap, minbdSize, maxbdSize);
    assert(sumgap == (maxbdSize - minbdSize));
  }


  // setup the -P positions
  for (int i = 0; i < toalloc; i++) {
    positionsCurrent[i] = positionsStart[i];
  }

  unsigned short xsubi[3] = {seed,seed,seed};
  // do it nice
  count = 0;
  while (count < pc->sz) {
    int nochange = 1;
    for (int i = 0; i < toalloc; i++) {
      size_t j = positionsCurrent[i]; // while in the range

      assert(j >= positionsStart[i]);

      size_t thislen = pc->minbs;
      if (pc->minbs != pc->maxbs) {
        thislen = lengthsGet(len, &seed);
      }
      //      assert(thislen >= 0);

      if ((j + thislen <= positionsEnd[i])) {



        // grow destination array
        if (count >= possAlloc) {
          possAlloc = possAlloc * 5 / 4 + 1; // grow
          positionType *poss2 = realloc(poss, possAlloc * sizeof(positionType));
          if (!poss) {
            fprintf(stderr,"OOM: breaking from setup array\n");
            break;
          } else {
            if (verbose >= 2) {
              fprintf(stderr,"*info*: new position size %.1lf MB array\n", TOMiB(possAlloc * sizeof(positionType)));
            }
            poss = poss2; // point to the new array
          }
        }

        // if we have gone over the end of the range
        //	if (j + thislen > positionsEnd[i]) {abort();fprintf(stderr,"hit the end"); continue;positionsCurrent[i] += thislen; break;}

        if (randomSubSample) {
	  abort();
          poss[count].pos = randomBlockSize(minbdSize, maxbdSize - thislen, alignbits, erand48(xsubi) * (maxbdSize - thislen - minbdSize));
          assert(poss[count].pos >= minbdSize);
          assert(poss[count].pos + thislen <= maxbdSize);
        } else {
          poss[count].pos = j;
          int overend = 0;
          if (poss[count].pos + thislen > positionsEnd[i]) overend = 1;
          if (sf_maxsizebytes && (poss[count].pos + thislen > positionsStart[i] + sf_maxsizebytes)) overend = 1;
          if (overend) {
            poss[count].pos = positionsStart[i];
            positionsCurrent[i] = positionsStart[i];
            //	    fprintf(stderr,"*warning* position wrapped around %zd [%zd, %zd]\n", poss[count].pos, positionsStart[i], positionsEnd[i]);
            //	    abort();
          }
        }

        assert(poss[count].pos >= positionsStart[i]);
        assert(poss[count].pos < positionsEnd[i]);
        if (sf_maxsizebytes) {
          assert(poss[count].pos <= positionsStart[i] + sf_maxsizebytes);
        }

        poss[count].submitTime = 0;
        poss[count].finishTime = 0;
        poss[count].len = thislen;
        //	assert(poss[count].len >= 0);
        poss[count].seed = seedin;
        poss[count].verify = 0;
        poss[count].q = 0;

        // only store depending on the module start
        if ((positionsCurrent[i] / pc->minbs) % mod == remain) count++;

        positionsCurrent[i] += thislen;

        nochange = 0;
        if (count >= pc->sz) break; // if too many break
      }
    }
    if (nochange) break;
  }
  if (count < pc->sz) {
    if (verbose > 1) {
      fprintf(stderr,"*warning* there are %zd unique positions on the device\n", count);
    }
  }
  pc->sz = count;

  // make a complete copy and rotate by an offset

  //  fprintf(stderr,"starting Block %ld, count %zd\n", startingBlock, count);
  int offset = 0;
  if (count) {
    if (startingBlock == -99999) {
      offset = rand_r(&seed) % count;
    } else {
      if (startingBlock >= 0) {
        offset = startingBlock % count;
      } else {
        offset = (count + startingBlock) % count;
      }
    }
  }
  //    fprintf(stderr,"starting offset %d\n", offset);

  // rotate
  for (size_t i = 0; i < count; i++) {
    int index = i + offset;
    if (index >= (int)count) {
      index -= count;
    }
    positions[i] = poss[index];
    char newaction = 'R';
    if (readorwrite.rprob == 1) {
      newaction = 'R';
    } else if (readorwrite.wprob == 1) {
      newaction = 'W'; // don't use drand unless you have to
      anywrites = 1;
    } else {
      double d = erand48(xsubi); // between [0,1]
      if (d <= readorwrite.rprob)
        newaction = 'R';
      else if (d <= readorwrite.rprob + readorwrite.wprob) {
        newaction = 'W';
        anywrites = 1;
      } else {
        newaction = 'T';
        anywrites = 1;
      }
    }
    positions[i].action = newaction;
    //assert(positions[i].len >= 0);
  }

  if (sf < 0) {
    if (verbose) {
      fprintf(stderr,"*info* reversing positions from the end of the BD, %zd bytes (%.1lf GB)\n", maxbdSize, TOGB(maxbdSize));
    }
    for (size_t i = 0; i < count; i++) {
      positions[i].pos = maxbdSize - (size_t) positions[i].len - (positions[i].pos - minbdSize);
    }
  }

  if (verbose >= 2) {
    fprintf(stderr,"*info* %zd unique positions, max %zd positions requested (-P), %.2lf GiB of device covered (%.0lf%%)\n", count, pc->sz, TOGiB(totalLen), 100.0*TOGiB(totalLen)/TOGiB(maxbdSize));
  }

  // if randomise then reorder
  //  if (sf == 0) {
  //    positionContainerRandomize(pc);
  //  }

  // rotate
  size_t sum = 0;
  positionType *p = positions;

  for (size_t i = 0; i < pc->sz; i++, p++) {
    p->deviceid = deviceid;
    sum += p->len;
    //assert(p->len >= 0);
  }
  if (verbose >= 2) {
    fprintf(stderr,"*info* sum of %zd lengths is %.1lf GiB\n", pc->sz, TOGiB(sum));
  }

  pc->minbdSize = minbdSize;
  pc->maxbdSize = maxbdSize;

  free(poss);
  free(positionsStart);
  free(positionsEnd);
  free(positionsCurrent);

  if (fourkEveryMiB > 0) {
    insertFourkEveryMiB(pc, minbdSize, maxbdSize, seed, fourkEveryMiB, jumpKiB);
  }

  positionContainerCheck(pc, minbdSize, maxbdSize, 1);
  
  return anywrites;
}

void insertFourkEveryMiB(positionContainer *pc, const size_t minbdSize, const size_t maxbdSize, unsigned int seed, const double fourkEveryMiB, const size_t jumpKiB)
{
  if (minbdSize || maxbdSize) {
  }
  fprintf(stderr,"*info* insertFourkEveryMiB: %.1lf MiB, initially %zd positions\n", fourkEveryMiB, pc->sz);

  if (pc->sz > 0) {
    size_t last = pc->positions[0].pos, count = 0;
    if (jumpKiB == 0) {
      for (size_t i = 0; i < pc->sz; i++) {
        if (pc->positions[i].pos - last >= fourkEveryMiB * 1024 * 1024) {
          count++;
          last = pc->positions[i].pos;
        }
      }
    }
    fprintf(stderr,"*info* need to insert %zd random operations\n", count);
    {

      positionType *newpos;
      CALLOC(newpos, pc->sz + count, sizeof(positionType));
      size_t newindex = 0, cumjump = 0;
      last = pc->positions[0].pos;

      for (size_t i = 0; i < pc->sz; i++) { // iterate through all, either copy or insert then copy
        //	fprintf(stderr,"%zd %zd %zd %zd\n", i, pc->sz, newindex, pc->sz + count);
        if (pc->positions[i].pos - last >= fourkEveryMiB * 1024 * 1024) {
          if (jumpKiB == 0) { //insert random
            // insert
            assert(newindex < pc->sz + count);
            newpos[newindex] = pc->positions[i]; // copy action and seed
            newpos[newindex].pos = (rand_r(&seed) % count) * 4096;
            newpos[newindex].len = 4096;
            newindex++;
            // copy existing
            if (i < pc->sz) {
              newpos[newindex] = pc->positions[i];
            }
            last = newpos[newindex].pos;
          } else { // offset pos
            assert(newindex < pc->sz + count);
            newpos[newindex] = pc->positions[i];
            cumjump += alignedNumber(jumpKiB * 1024, 4096);
            newpos[newindex].pos += cumjump;
            last = newpos[newindex].pos;
          }
        } else {
          assert(newindex < pc->sz + count);
          newpos[newindex] = pc->positions[i];
        }
        newindex++;
      }

      positionType *tod = pc->positions;
      pc->positions = newpos;
      free(tod);
      pc->sz += count;

      fprintf(stderr,"*info* new number of positions: %zd\n", pc->sz);

    }
  }
}



void positionContainerRandomize(positionContainer *pc, unsigned int seed)
{
  const size_t count = pc->sz;
  positionType *positions = pc->positions;

  if (verbose)  fprintf(stderr,"*info* shuffling/randomizing the array of %zd values, seed %u\n", count, seed);
  for (size_t shuffle = 0; shuffle < 1; shuffle++) {
    for (size_t i = 0; i < count; i++) {
      size_t j = i;
      if (count > 1) {
        while ((j = rand_r(&seed) % count) == i) {
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


void positionAddBlockSize(positionType *positions, const size_t count, const size_t addSize, const size_t minbdSize, const size_t maxbdSize)
{
  if (minbdSize) {}
  if (maxbdSize) {}
  if (verbose) {
    fprintf(stderr,"*info* adding %zd size\n", addSize);
  }
  positionType *p = positions;
  for (size_t i = 0; i < count; i++) {
    p->pos += addSize;
    p->submitTime = 0;
    p->finishTime = 0;
    p->inFlight = 0;
    p->success = 0;
    if (p->pos + p->len > maxbdSize) {
      if (verbose >= 1) fprintf(stderr,"*info* wrapping add block of %zd\n", addSize);
      p->pos = addSize;
    }
    p++;
  }
}


void positionPrintMinMax(positionType *positions, const size_t count, const size_t minbdsize, const size_t maxbdsize, const size_t glow, const size_t ghigh)
{
  positionType *p = positions;
  size_t low = 0;
  size_t high = 0;
  for (size_t i = 0; i < count; i++) {
    if ((p->pos < low) || (i==0)) {
      low = p->pos;
    }
    if (p->pos > high) {
      high = p->pos;
    }
    assert(low >= minbdsize);
    assert(high <= maxbdsize);
    p++;
  }
  const size_t range = ghigh - glow;
  fprintf(stderr,"*info* min position = LBA %.1lf %% (%zd, %.1lf GB), max position = LBA %.1lf %% (%zd, %.1lf GB), range [%.1lf,%.1lf] GB\n", 100.0 * ((low - glow) * 1.0 / range), low, TOGB(low), 100.0* ((high - glow) * 1.0 / range), high, TOGB(high), TOGB(minbdsize), TOGB (maxbdsize));
}



void positionContainerJumble(positionContainer *pc, const size_t jumble, unsigned int seed)
{
  size_t count = pc->sz;
  positionType *positions = pc->positions;
  assert(jumble >= 1);
  if (verbose >= 1) {
    fprintf(stderr,"*info* jumbling the array %zd with value %zd\n", count, jumble);
  }
  size_t i2 = 0;
  while (i2 < count - jumble) {
    for (size_t j = 0 ; j < jumble; j++) {
      size_t start = i2 + j;
      size_t swappos = i2 + (rand_r(&seed) % jumble);
      if (swappos < count) {
        // swap
        positionType p = positions[start];
        positions[start] = positions[swappos];
        positions[swappos] = p;
      }
    }
    i2 += jumble;
  }
}


void positionStats(const positionType *positions, const size_t maxpositions, const deviceDetails *devList, const size_t devCount)
{
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



void positionContainerDump(positionContainer *pc, const size_t countToShow)
{
  fprintf(stderr,"*info*: total number of positions %zd\n", pc->sz);
  const positionType *positions = pc->positions;
  size_t rcount = 0, wcount = 0, tcount = 0, hash = 0;
  for (size_t i = 0; i < pc->sz; i++) {
    hash = (hash + i) + positions[i].action + positions[i].pos + positions[i].len;
    if (positions[i].action == 'R') rcount++;
    else if (positions[i].action == 'W') wcount++;
    else if (positions[i].action == 'T') tcount++;

    if (i < countToShow) {
      fprintf(stderr,"\t[%02zd] action %c\tpos %12zd\tlen %7d\tdevice %d\tverify %d\tseed %6d\tusoffset %lf\n", i, positions[i].action, positions[i].pos, positions[i].len, positions[i].deviceid,positions[i].verify, positions[i].seed, positions[i].usoffset);
    }
  }
  fprintf(stderr,"\tSummary[%d]: reads %zd, writes %zd, trims %zd, hash %lx\n", positions[0].seed, rcount, wcount, tcount, hash);
}


void positionContainerInit(positionContainer *pc, size_t UUID)
{
  memset(pc, 0, sizeof(positionContainer));
  pc->UUID = UUID;
}

void positionContainerSetup(positionContainer *pc, size_t sz)
{
  pc->sz = sz;
  pc->positions = createPositions(sz);
}


void positionContainerSetupFromPC(positionContainer *pc, const positionContainer *oldpc)
{
  memset(pc, 0, sizeof(positionContainer));
  pc->positions = NULL;
  pc->sz = oldpc->sz;
  pc->LBAcovered = oldpc->LBAcovered;
  //  pc->device = oldpc->device;
  //  pc->string = oldpc->string;
  pc->minbdSize = oldpc->minbdSize;
  pc->maxbdSize = oldpc->maxbdSize;
  pc->minbs = oldpc->minbs;
  pc->maxbs = oldpc->maxbs;
  pc->UUID = oldpc->UUID+1;
  pc->elapsedTime = oldpc->elapsedTime;
  pc->diskStats = oldpc->diskStats;
}


void positionContainerAddMetadataChecks(positionContainer *pc, const size_t metadata)
{
  const size_t origsz = pc->sz;

  positionType *p = NULL;
  size_t maxalloc = (4 * origsz); // a single R becomes RPWP
  CALLOC(p, maxalloc, sizeof(positionType));
  if (p == NULL) {
    fprintf(stderr,"*error* can't alloc array\n");
    exit(-1);
  }

  size_t newpos = 0;
  for (size_t i = 0; i < origsz; i += metadata) {
    size_t gap = (origsz - i > metadata) ? metadata : origsz - i;
    assert (newpos < maxalloc);
    for (size_t j = 0; j < gap; j++) {
      //      assert (newpos < 2*origsz);
      p[newpos] = pc->positions[i + j];
      p[newpos].action = pc->positions[i + j].action;
      newpos++;
    }
    p[newpos++].action = 'P'; // pause
    assert (newpos < maxalloc);
    for (size_t j = 0; j < gap; j++) {
      //      assert (newpos < 2*origsz);
      p[newpos] = pc->positions[i + j];
      p[newpos].action = 'R';
      if (p[newpos - gap - 1].action == 'W') {
	p[newpos].verify = newpos - gap - 1; // min 1 for the 'P'
      }
      newpos++;
    }
    //    fprintf(stderr,"%zd %zd\n", newpos, maxalloc);
    assert (newpos < maxalloc);
    p[newpos++].action = 'P'; // pause after
  }
  free(pc->positions);
  //  fprintf(stderr,"reallo from %zd to %zd\n", maxalloc, newpos);
  p = realloc(p, newpos * sizeof(positionType)); // truncate
  pc->positions = p;
  pc->sz = newpos;
  // check a read has the same position as the original write
  //  fprintf(stderr,"*info* checking...\n");
  for (size_t i = 0; i < pc->sz; i++) {
    if (pc->positions[i].action == 'P') {
      assert(pc->positions[i-1].action != pc->positions[i].action); // check either side not p
      assert(pc->positions[i+1].action != pc->positions[i].action); 
      continue;
    } else {
      size_t vpos = pc->positions[i].verify;
      if (vpos > 0) {  
	assert(pc->positions[i].pos == pc->positions[vpos].pos);
	assert(pc->positions[i].len == pc->positions[vpos].len);
	//	assert(pc->positions[i].action != pc->positions[vpos].action);
	assert(pc->positions[vpos].action != 'P');
      }
    }
  }
}




void positionContainerAddDelay(positionContainer *pc, unsigned long iops, size_t threadid)
{
  size_t origsz = pc->sz;
  if (threadid == 0) fprintf(stderr,"*info* [t%zd] target %zd IOPS per thread\n", threadid, iops);

  for (size_t i = 0; i < origsz; i++) {
    pc->positions[i].usoffset = (i * 1.0 / iops);
  }
}



void positionContainerUniqueSeeds(positionContainer *pc, unsigned short seed, const int andVerify)
{
  size_t origsz = pc->sz;

  // originals
  positionType *origs = malloc(origsz * sizeof(positionType));
  if (!origs) {
    fprintf(stderr,"*error* can't alloc array for uniqueSeeds\n");
    exit(-1);
  }
  memcpy(origs, pc->positions, origsz * sizeof(positionType));

  // double the size
  pc->positions = realloc(pc->positions, (origsz * 2) * sizeof(positionType));
  if (!pc->positions) {
    fprintf(stderr,"*error* can't realloc array to %zd\n", (origsz * 2) * sizeof(positionType));
    exit(-1);
  }
  memset(pc->positions + origsz, 0, origsz);

  size_t j = 0;
  for (size_t i = 0; i < origsz; i++) {
    origs[i].seed = seed++;
    pc->positions[j] = origs[i];
    j++;

    if (andVerify) {
      pc->positions[j] = origs[i];
      if (pc->positions[j].action == 'W') {
        pc->positions[j].action = 'R';
        pc->positions[j].verify = j-1;
        assert(pc->positions[j].verify == j-1);
      }
      j++;
    }
  }
  free(origs);

  pc->sz = j;
}

void positionContainerFree(positionContainer *pc)
{
  if (pc->positions) free(pc->positions);
  pc->positions = NULL;
}


/*
void positionLatencyStats(positionContainer *pc, const int threadid)
{
  if (threadid) {}
  size_t failed = 0;

  for (size_t i = 0; i < pc->sz; i++) {
    if (pc->positions[i].success && pc->positions[i].finishTime) {
      //
    } else {
      if (pc->positions[i].submitTime) {
        failed++;
      }
    }
  }
  //const double elapsed = pc->elapsedTime;

  //  fprintf(stderr,"*info* [T%d] '%s': R %.0lf MB/s (%.0lf IO/s), W %.0lf MB/s (%.0lf IO/s), %.1lf s\n", threadid, pc->string, TOMB(pc->readBytes/elapsed), pc->readIOs/elapsed, TOMB(pc->writtenBytes/elapsed), pc->writtenIOs/elapsed, elapsed);
  if (verbose >= 2) {
    fprintf(stderr,"*failed or not finished* %zd\n", failed);
  }
  fflush(stderr);
}
*/


jobType positionContainerLoad(positionContainer *pc, FILE *fd) {
  return positionContainerLoadLines(pc, fd, 0);
}



jobType positionContainerLoadLines(positionContainer *pc, FILE *fd, size_t maxLines){

  positionContainerInit(pc, 0);
  jobType job;
  jobInit(&job);

  char *line = malloc(1000);
  size_t maxline = 1000, maxSize = 0, minbs = (size_t)-1, maxbs = 0;
  ssize_t read;
  char *path;
  CALLOC(path, 1000, 1);
  //  char *origline = line; // store the original pointer, as getline changes it creating an unfreeable area
  positionType *p = NULL;
  size_t pNum = 0;
  double starttime, fintime;

  while (keepRunning && (read = getline(&line, &maxline, fd) != -1)) {
    size_t pos, len, seed, tmpsize;
    //    fprintf(stderr,"in: %s %zd\n", line, strlen(line));
    char op;
    starttime = 0;
    fintime = 0;

    int s = sscanf(line, "%s %zu %*s %*s %*s %c %zu %zu %*s %*s %zu %lf %lf", path, &pos, &op, &len, &tmpsize, &seed, &starttime, &fintime);
    if (s >= 5) {
      //      fprintf(stderr,"%s %zd %c %zd %zd %lf %lf\n", path, pos, op, len, seed, starttime, fintime);
      //      deviceDetails *d2 = addDeviceDetails(path, devs, numDevs);

      int seenpathbefore = -1;
      for (int k = 0; k < job.count; k++) {
        if (strcmp(path, job.devices[k]) == 0) {
          seenpathbefore = k;
        }
      }
      if (seenpathbefore == -1) {
        jobAddBoth(&job, path, "w", -1);
        seenpathbefore = job.count - 1;
      }
      //
      pNum++;
      p = realloc(p, sizeof(positionType) * (pNum));
      assert(p);
      //      fprintf(stderr,"loaded %s deviceid %d\n", path, seenpathbefore);
      p[pNum-1].deviceid = seenpathbefore;
      assert(starttime);
      p[pNum-1].submitTime = starttime;
      p[pNum-1].finishTime = fintime;
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
    }
    if (maxLines) {
      if (pNum >= maxLines) {
	break;
      }
    }
  }
  fflush(stderr);

  free(line);
  line = NULL;

  pc->positions = p;
  pc->sz = pNum;
  for (size_t i = 0; i < pc->sz; i++) {
    assert(pc->positions[i].submitTime != 0);
    assert(pc->positions[i].finishTime != 0);
  }
  //  pc->string = strdup("");
  //  pc->device = path;
  pc->maxbdSize = maxSize;
  pc->minbs = minbs;
  pc->maxbs = maxbs;

  free(path);
  return job;
}

void positionContainerInfo(const positionContainer *pc)
{
  fprintf(stderr,"UUID '%zd', number of actions %zd (%zd/%zd), device size %zd (%.3lf GiB), k [%zd,%zd]\n", pc->UUID, pc->sz, pc->minbs, pc->maxbs, pc->maxbdSize, TOGiB(pc->maxbdSize), pc->minbs, pc->maxbs);
}





#include <stdlib.h>
#include <stdio.h>

#include "procDiskStats.h"

int keepRunning = 1;

void usage(float timems, float latencyms) {
  fprintf(stderr,"Usage:\n  diskperf [-t seconds] [-l latencyms]\n");
  fprintf(stderr,"\nOptions:\n");
  fprintf(stderr,"  -s n  \tsample time of n seconds (default %.0lf)\n", timems/1000.0);
  fprintf(stderr,"  -l n  \tonly print IO for drives that over n ms of latency (default %.0lf)\n", latencyms);
  fprintf(stderr,"  -d n  \tstop after n iterations of display\n");
}

int main(int argc, char *argv[]) {

  int opt;
  const char *getoptstring = "l:s:hd:";

  float timems = 1000;
  float latencyms = 0;
  size_t stopaftern = (size_t)-1;
  
  while((opt = getopt(argc, argv, getoptstring)) != -1) {
    switch (opt) {
    case 's':
      timems = atof(optarg) * 1000.0;
      break;
    case 'l':
      latencyms = atof(optarg);
      break;
    case 'd':
      stopaftern = atoi(optarg);
      break;
    case 'h':
      usage(timems, latencyms);
      exit(1);
    }
  }

  fprintf(stderr,"*info* time %.0lf ms, print if latency >= %.0lf ms\n", timems, latencyms);

  procDiskStatsType old,new, delta;

  procDiskStatsInit(&old);
  procDiskStatsSample(&old);
  size_t displaycount = 0;
  
  while (++displaycount <= stopaftern) {

    usleep(timems * 1000.0);
    
    procDiskStatsInit(&new);
    procDiskStatsSample(&new);
    
    delta = procDiskStatsDelta(&old, &new);
    
    procDiskStatsDumpThres(stdout, &delta, latencyms);
    procDiskStatsFree(&delta);

    procDiskStatsFree(&old);
    procDiskStatsCopy(&old, &new);

    procDiskStatsFree(&new);
  }

  procDiskStatsFree(&old);
  
  exit(0);
}

  

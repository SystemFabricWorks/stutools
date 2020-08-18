#ifndef _BLOCK_VERIFY_H
#define _BLOCK_VERIFY_H

#include "jobType.h"

int verifyPositions(positionContainer *pc, const size_t threads, jobType *job, const size_t o_direct, const size_t sort, const double runSeconds, size_t *correct, size_t *incorrect, size_t *ioerrors);


#endif

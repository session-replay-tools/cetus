#ifndef _CETUS_SETAFFINITY_H_INCLUDED_
#define _CETUS_SETAFFINITY_H_INCLUDED_

#include <stdint.h>
#include <stddef.h>

#if (CETUS_HAVE_SCHED_SETAFFINITY || CETUS_HAVE_CPUSET_SETAFFINITY)

#define CETUS_HAVE_CPU_AFFINITY 1

#if (CETUS_HAVE_SCHED_SETAFFINITY)

typedef cpu_set_t  cetus_cpuset_t;

#elif (CETUS_HAVE_CPUSET_SETAFFINITY)

#include <sys/cpuset.h>

typedef cpuset_t  cetus_cpuset_t;

#endif

void cetus_setaffinity(cetus_cpuset_t *cpu_affinity);

#else

#define cetus_setaffinity(cpu_affinity)

typedef uint64_t  cetus_cpuset_t;

#endif


#endif /* _CETUS_SETAFFINITY_H_INCLUDED_ */

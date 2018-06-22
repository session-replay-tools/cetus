#include "glib-ext.h"
#include "cetus-setaffinity.h"


#if (CETUS_HAVE_CPUSET_SETAFFINITY)

void
cetus_setaffinity(cetus_cpuset_t *cpu_affinity)
{
    unsigned i;

    for (i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, cpu_affinity)) {
            g_message("%s: cpuset_setaffinity(): using cpu #%ui", G_STRLOC, i);
        }
    }

    if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
                           sizeof(cpuset_t), cpu_affinity) == -1)
    {
        g_critical("%s: cpuset_setaffinity() failed", G_STRLOC);
    }
}

#elif (CETUS_HAVE_SCHED_SETAFFINITY)

void
cetus_setaffinity(cetus_cpuset_t *cpu_affinity)
{
    unsigned i;

    for (i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, cpu_affinity)) {
            g_message("%s: cpuset_setaffinity(): using cpu #%ui", G_STRLOC, i);
        }
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), cpu_affinity) == -1) {
        g_critical("%s: cpuset_setaffinity() failed", G_STRLOC);
    }
}

#endif

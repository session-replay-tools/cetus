#ifndef __CHASSIS_TIMINGS_H__
#define __CHASSIS_TIMINGS_H__

#include <sys/time.h> /* struct timeval */
#include <glib.h>
#include "chassis-exports.h"

#define SECONDS ( 1)
#define MINUTES ( 60 * SECONDS)
#define HOURS ( 60 * MINUTES)

CHASSIS_API int chassis_epoch_from_string(const char *str, gboolean *ok);
CHASSIS_API gboolean chassis_timeval_from_double(struct timeval *dst, double t);
void chassis_epoch_to_string(time_t *epoch, char *str, int len);

#endif

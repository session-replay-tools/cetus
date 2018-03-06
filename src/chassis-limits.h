#ifndef _CHASSIS_LIMITS_H_
#define _CHASSIS_LIMITS_H_

#include <glib.h>    /* GPtrArray */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "chassis-exports.h"

CHASSIS_API int chassis_fdlimit_set(gint64 max_files_number);
CHASSIS_API gint64 chassis_fdlimit_get(void);

#endif

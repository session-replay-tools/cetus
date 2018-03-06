#ifndef __CHASSIS_PATH_H__
#define __CHASSIS_PATH_H__

#include <glib.h>

#include "chassis-exports.h"

CHASSIS_API gchar *chassis_resolve_path(const char *base_dir, gchar *filename);
CHASSIS_API gchar *chassis_get_basedir(const gchar *prgname);

#endif


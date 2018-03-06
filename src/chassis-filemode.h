#ifndef _CHASSIS_PERM_H_
#define _CHASSIS_PERM_H_

#include <glib.h>
#include "chassis-exports.h"

#include <sys/stat.h>
#define CHASSIS_FILEMODE_SECURE_MASK (S_IROTH|S_IWOTH|S_IXOTH)

CHASSIS_API int chassis_filemode_check_full(const gchar *, int , GError **);

#endif

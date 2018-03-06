#ifndef _CHASSIS_KEYFILE_H_
#define _CHASSIS_KEYFILE_H_

#include <glib.h>

#include "chassis-exports.h"

/** @addtogroup chassis */
/*@{*/
/**
 * parse the configfile options into option entries
 *
 */
gboolean chassis_keyfile_to_options_with_error(GKeyFile *keyfile, 
        const gchar *groupname, GList *config_entries, GError **gerr);


/*@}*/

#endif


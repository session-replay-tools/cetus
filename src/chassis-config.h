#ifndef CHASSIS_CONFIG_H
#define CHASSIS_CONFIG_H

#include "glib-ext.h"

/**
 * The config manager module, it manages `options` and `object`
 * currently support `mysql` and `local` media type
 */

typedef struct chassis_config_t chassis_config_t;

chassis_config_t *chassis_config_from_url(char *url);

chassis_config_t *chassis_config_from_local_dir(char *dir, char *conf_file);

void chassis_config_free(chassis_config_t *);

GHashTable *chassis_config_get_options(chassis_config_t *);

gboolean chassis_config_parse_options(chassis_config_t *, GList *entries);

gboolean chassis_config_query_object(chassis_config_t *,
                                    const char *name, char **json);

gboolean chassis_config_write_object(chassis_config_t *,
                                     const char *name, const char *json);

gboolean chassis_config_is_object_outdated(chassis_config_t *, const char *name);

void chassis_config_update_object_cache(chassis_config_t *, const char *name);

char *chassis_config_get_id(chassis_config_t *);

gboolean chassis_config_register_service(chassis_config_t *conf, char *id, char *data);

void chassis_config_unregister_service(chassis_config_t *conf, char *id);

#endif /* CHASSIS_CONFIG_H */

#include "cetus-variable.h"
#include "chassis-mainloop.h"

#include <stdlib.h>
#include <string.h>

void
cetus_variables_init_stats(cetus_variable_t **vars, chassis *chas)
{
    query_stats_t *stats = &(chas->query_stats);

    cetus_variable_t stats_variables[] = {
        {"Com_select", &stats->com_select, VAR_INT64},
        {"Com_insert", &stats->com_insert, VAR_INT64},
        {"Com_update", &stats->com_update, VAR_INT64},
        {"Com_delete", &stats->com_delete, VAR_INT64},
        {"Com_select_shard", &stats->com_select_shard, VAR_INT64},
        {"Com_insert_shard", &stats->com_insert_shard, VAR_INT64},
        {"Com_update_shard", &stats->com_update_shard, VAR_INT64},
        {"Com_delete_shard", &stats->com_delete_shard, VAR_INT64},
        {"Com_select_global", &stats->com_select_global, VAR_INT64},
        {"Com_select_bad_key", &stats->com_select_bad_key, VAR_INT64},
        {NULL, NULL, 0}
    };
    int length = sizeof(stats_variables);
    int count = length / sizeof(cetus_variable_t);
    *vars = calloc(count, sizeof(cetus_variable_t));
    memcpy(*vars, stats_variables, length);
}

char *
cetus_variable_get_value_str(cetus_variable_t *var)
{
    char *value = NULL;
    switch (var->type) {
    case VAR_INT:
        value = g_strdup_printf("%u", *(gint *)(var->value));
        break;
    case VAR_INT64:
        value = g_strdup_printf("%lu", *(guint64 *)(var->value));
        break;
    case VAR_FLOAT:
        value = g_strdup_printf("%f", *(double *)(var->value));
        break;
    case VAR_STRING:
        value = g_strdup(var->value);
        break;
    default:
        value = g_strdup("error value");
    }
    return value;
}

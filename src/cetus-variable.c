#include "cetus-variable.h"
#include "chassis-mainloop.h"

#include <stdlib.h>
#include <string.h>


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

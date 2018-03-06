#ifndef CETUS_VARIABLE_H
#define CETUS_VARIABLE_H

#include <inttypes.h>

enum cetus_variable_type_t {
    VAR_INT,
    VAR_INT64,
    VAR_FLOAT,
    VAR_STRING,
};

struct chassis;
typedef struct cetus_variable_t {
    char *name;
    void *value;
    enum cetus_variable_type_t type;
} cetus_variable_t;

void cetus_variables_init_stats(/* out */ cetus_variable_t **vars, struct chassis *);

/**
 @return newly allocated string, must be freed
 */
char *cetus_variable_get_value_str(cetus_variable_t *var);

#endif /* CETUS_VARIABLES_H */

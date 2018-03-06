#ifndef CHARACTER_SET_H
#define CHARACTER_SET_H

#include "network-exports.h"

NETWORK_API int charset_get_number(const char *name);
NETWORK_API const char *charset_get_name(int number);

#endif // CHARACTER_SET_H

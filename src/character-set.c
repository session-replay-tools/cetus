#include "character-set.h"

#include <string.h>
#include <assert.h>

#define DEFAULT_CHARSET   '\x21'

int charset_get_number(const char *name)
{
    if (!name)
        return DEFAULT_CHARSET;
    static struct _charset_number_t {
        const char *name;
        int number;
    } map[] = {
        {"latin1",  0x08},
        {"big5",    0x01},
        {"gb2312",  0x18},
        {"gbk",     0x1c},
        {"utf8",    0x21},
        {"utf8mb4", 0x2d},
        {"binary",  0x3f}
    };
    int map_len = sizeof(map)/sizeof(struct _charset_number_t);
    int i = 0;
    while (i < map_len) {
        if (strcmp(name, map[i].name) == 0) {
            return map[i].number;
        }
        ++i;
    }
    return DEFAULT_CHARSET;
}

const char *charset_get_name(int number)
{
    assert(number < 64);
    static const char *charset[64] = {
        0, "big5", 0, 0, 0, 0, 0, 0, "latin1", 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, "gb2312", 0, 0, 0, "gbk", 0, 0, 0,
        0, "utf8", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "utf8mb4", 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "binary"
    };
    return charset[number];
}

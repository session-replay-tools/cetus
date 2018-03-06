#ifndef _CETUS_ERROR_H_
#define _CETUS_ERROR_H_

/**
 * ERROR codes extends ER_xx from mysqld_error.h
 */

enum {
    ER_CETUS_UNKNOWN = 5001,
    ER_CETUS_RESULT_MERGE,
    ER_CETUS_LONG_RESP,
    ER_CETUS_PARSE_SHARDING,
    ER_CETUS_NOT_SUPPORTED,
    ER_CETUS_SINGLE_NODE_FAIL,
    ER_CETUS_NO_GROUP,
};

#endif /*_CETUS_ERROR_H_*/

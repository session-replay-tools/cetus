#ifndef ADMIN_STATS_H
#define ADMIN_STATS_H

struct chassis;
typedef struct admin_stats_t admin_stats_t;
admin_stats_t* admin_stats_init(struct chassis* chas);
void admin_stats_free(admin_stats_t* stats);

#define ADMIN_STATS_QPS 1
#define ADMIN_STATS_TPS 2
void admin_stats_get_average(admin_stats_t* stats, int type, char* buf, int len);

#endif // ADMIN_STATS_H

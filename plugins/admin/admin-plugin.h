#ifndef ADMIN_PLUGIN_H
#define ADMIN_PLUGIN_H

#include "glib-ext.h"
#include "network-mysqld.h"
#include "admin-stats.h"
#include "cetus-process.h"

#ifndef PLUGIN_VERSION
#ifdef CHASSIS_BUILD_TAG
#define PLUGIN_VERSION CHASSIS_BUILD_TAG
#else
#define PLUGIN_VERSION PACKAGE_VERSION
#endif
#endif


struct chassis_plugin_config {
    gchar *address;               /**< listening address of the admin interface */

    gchar *admin_username;        /**< login username */
    gchar *admin_password;        /**< login password */

    gchar *allow_ip;                  /**< allow ip addr list */

    gchar *deny_ip;                  /**< deny ip addr list */

    gboolean has_shard_plugin;

    network_mysqld_con *listen_con;

    admin_stats_t *admin_stats;
};

#endif

#  $%BEGINLICENSE%$
#  Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.
# 
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License as
#  published by the Free Software Foundation; version 2 of the
#  License.
# 
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
#  GNU General Public License for more details.
# 
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
#  02110-1301  USA
# 
#  $%ENDLICENSE%$

INCLUDE_DIRECTORIES(${PROJECT_BINARY_DIR}) # for config.h

INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/lib)
include_directories(${PROJECT_BINARY_DIR}/lib)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

INCLUDE_DIRECTORIES(${GLIB_INCLUDE_DIRS})
LINK_DIRECTORIES(${GLIB_LIBRARY_DIRS})

INCLUDE_DIRECTORIES(${MYSQL_INCLUDE_DIRS})
LINK_DIRECTORIES(${MYSQL_LIBRARY_DIRS})

LINK_DIRECTORIES(${LIBINTL_LIBRARY_DIRS})

include_directories(${PROJECT_SOURCE_DIR}/libev)

STRING(REPLACE "." "" SHARED_LIBRARY_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})
ADD_DEFINITIONS(-DSHARED_LIBRARY_SUFFIX="${SHARED_LIBRARY_SUFFIX}")

SET(chassis_sources 
    chassis-plugin.c
    chassis-event.c
    chassis-log.c
    chassis-mainloop.c
    chassis-shutdown-hooks.c
    chassis-keyfile.c
    chassis-path.c
    chassis-filemode.c
    chassis-limits.c
    chassis-frontend.c
    chassis-options.c
    chassis-options-utils.c
    chassis-unix-daemon.c
    chassis-config.c
    cJSON.c
    chassis-sql-log.c
)

SET(timing_sources
    chassis-timings.c
)

SET(glibext_sources
    glib-ext.c
)

SET(proxy_sources 
    network-mysqld.c 
    network-mysqld-proto.c 
    network-mysqld-packet.c 
    network-conn-pool.c  
    network-conn-pool-wrap.c  
    network-queue.c
    network-socket.c
    network-address.c
    network-injection.c
    resultset_merge.c
    cetus-log.c
    cetus-setaffinity.c
    cetus-process.c
    cetus-process-cycle.c
    cetus-channel.c
    plugin-common.c
    network-backend.c
    sharding-config.c
    sharding-query-plan.c
    shard-plugin-con.c
    character-set.c
    server-session.c
    network-compress.c
    cetus-users.c
    cetus-util.c
    cetus-variable.c
    cetus-monitor.c
    cetus-acl.c
)

if (HAVE_OPENSSL)
  list(APPEND proxy_sources network-ssl.c)
endif(HAVE_OPENSSL)

ADD_LIBRARY(mysql-chassis SHARED ${chassis_sources})
ADD_LIBRARY(mysql-chassis-proxy SHARED ${proxy_sources})
ADD_LIBRARY(mysql-chassis-glibext SHARED ${glibext_sources})
ADD_LIBRARY(mysql-chassis-timing SHARED ${timing_sources})
ADD_EXECUTABLE(cetus mysql-proxy-cli.c)

if(SIMPLE_PARSER)
  target_compile_definitions(cetus PRIVATE DEFAULT_PLUGIN="proxy")
else(SIMPLE_PARSER)
  target_compile_definitions(cetus PRIVATE DEFAULT_PLUGIN="shard")
endif(SIMPLE_PARSER)

if(HAVE_TCMALLOC)
    TARGET_LINK_LIBRARIES(mysql-chassis-glibext
        ${GLIB_LIBRARIES} 
        ${GMODULE_LIBRARIES} 
        ${GTHREAD_LIBRARIES} 
        tcmalloc
        )

    TARGET_LINK_LIBRARIES(mysql-chassis-timing
        ${GLIB_LIBRARIES} 
        ${GMODULE_LIBRARIES} 
        ${GTHREAD_LIBRARIES} 
        mysql-chassis-glibext
        tcmalloc
        )

    TARGET_LINK_LIBRARIES(mysql-chassis 
        ${GLIB_LIBRARIES} 
        ${GMODULE_LIBRARIES} 
        ${GTHREAD_LIBRARIES} 
        ${MYSQL_LIBRARIES}
        ev-cetus
        tcmalloc
        mysql-chassis-timing
        mysql-chassis-glibext
        )

    TARGET_LINK_LIBRARIES(mysql-chassis-proxy
        tcmalloc
        mysql-chassis
        sqlparser
        mysql-chassis-glibext
        mysql-chassis-timing
        ${OPENSSL_LIBRARIES}
        ${ZLIB_LIBRARIES}
        )

    TARGET_LINK_LIBRARIES(cetus
        tcmalloc
        mysql-chassis
        mysql-chassis-proxy
        mysql-chassis-glibext
        mysql-chassis-timing
        ${GLIB_LIBRARIES}
        ${GMODULE_LIBRARIES}
        ${GTHREAD_LIBRARIES}
        ${MYSQL_LIBRARIES}
        ${ZLIB_LIBRARIES}
        ev-cetus
        )

else(HAVE_TCMALLOC)

    TARGET_LINK_LIBRARIES(mysql-chassis-glibext
        ${GLIB_LIBRARIES} 
        ${GMODULE_LIBRARIES} 
        ${GTHREAD_LIBRARIES} 
        )

    TARGET_LINK_LIBRARIES(mysql-chassis-timing
        ${GLIB_LIBRARIES} 
        ${GMODULE_LIBRARIES} 
        ${GTHREAD_LIBRARIES} 
        mysql-chassis-glibext
        )

    TARGET_LINK_LIBRARIES(mysql-chassis 
        ${GLIB_LIBRARIES} 
        ${GMODULE_LIBRARIES} 
        ${GTHREAD_LIBRARIES}
        ev-cetus
        ${MYSQL_LIBRARIES}
        mysql-chassis-timing
        mysql-chassis-glibext
        )

    TARGET_LINK_LIBRARIES(mysql-chassis-proxy
        mysql-chassis
        sqlparser
        mysql-chassis-glibext
        mysql-chassis-timing
        ${OPENSSL_LIBRARIES}
        ${ZLIB_LIBRARIES}
        )

    TARGET_LINK_LIBRARIES(cetus
        mysql-chassis
        mysql-chassis-proxy
        mysql-chassis-glibext
        mysql-chassis-timing
        ${GLIB_LIBRARIES}
        ${GMODULE_LIBRARIES}
        ${GTHREAD_LIBRARIES}
        ev-cetus
        ${MYSQL_LIBRARIES}
        ${ZLIB_LIBRARIES}
        )
endif(HAVE_TCMALLOC)

# Unix platforms provide a wrapper script to avoid relinking at install time
# figure out the correct name of the shared linker lookup path for this system, default to LD_LIBRARY_PATH
SET(DYNLIB_PATH_VAR "LD_LIBRARY_PATH")

IF("AIX" STREQUAL ${CMAKE_SYSTEM_NAME})
    SET(DYNLIB_PATH_VAR "LIBPATH")
ENDIF("AIX" STREQUAL ${CMAKE_SYSTEM_NAME})

IF("HP-UX" STREQUAL ${CMAKE_SYSTEM_NAME})
    SET(DYNLIB_PATH_VAR "SHLIB_PATH")
ENDIF("HP-UX" STREQUAL ${CMAKE_SYSTEM_NAME})

IF(APPLE)
    SET(DYNLIB_PATH_VAR "DYLD_LIBRARY_PATH")
ENDIF(APPLE)

# write the wrapper script, which uses DYNLIB_PATH_VAR
CONFIGURE_FILE(
    ${CMAKE_SOURCE_DIR}/scripts/cetus-binwrapper.in 
    ${PROJECT_BINARY_DIR}/cetus.sh
    @ONLY
    )
INSTALL(FILES ${PROJECT_BINARY_DIR}/cetus.sh
    DESTINATION bin/
    PERMISSIONS OWNER_EXECUTE OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    RENAME cetus
    )

INSTALL(TARGETS cetus
    RUNTIME DESTINATION libexec
    )

CHASSIS_INSTALL_TARGET(mysql-chassis)
CHASSIS_INSTALL_TARGET(mysql-chassis-proxy)
CHASSIS_INSTALL_TARGET(mysql-chassis-glibext)
CHASSIS_INSTALL_TARGET(mysql-chassis-timing)

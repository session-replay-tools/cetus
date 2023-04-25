#  $%BEGINLICENSE%$
#  Copyright (c) 2009, 2013, Oracle and/or its affiliates. All rights reserved.
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

PROJECT(cetus C)

SET(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

CMAKE_MINIMUM_REQUIRED(VERSION 2.8.11)

INCLUDE(CheckIncludeFiles)
INCLUDE(CheckFunctionExists)
INCLUDE(CheckLibraryExists)
INCLUDE(FindPkgConfig)
INCLUDE(CheckTypeSize)
INCLUDE(ChassisPlugin)
INCLUDE(ChassisInstall)
INCLUDE(CTest)
INCLUDE(Tar)

ENABLE_TESTING()

set(CETUS_TOOLS_DIR ${PROJECT_SOURCE_DIR}/tools)

OPTION(ENABLE_GCOV "Enable gcov (debug, Linux builds only)" OFF)

IF (ENABLE_GCOV AND NOT WIN32 AND NOT APPLE)
    MESSAGE(STATUS "***************building gcov version*************")
    SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fprofile-arcs -ftest-coverage")
    SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fprofile-arcs -ftest-coverage")
    SET(CMAKE_EXE_LINKER_FLAGS_DEBUG
        "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fprofile-arcs -ftest-coverage -lgcov")
ENDIF()

option(SIMPLE_PARSER "use the simple parser")
option(NETWORK_DEBUG_TRACE_IO "if NETWORK_DEBUG_TRACE_IO is defined, the network layer will log the raw MySQL packets as log-level debug")

option(NETWORK_DEBUG_TRACE_EVENT "if NETWORK_DEBUG_TRACE_EVENT is defined, cetus will log the abnormal event")

option(NETWORK_DEBUG_TRACE_STATE_CHANGES "if NETWORK_DEBUG_TRACE_STATE_CHANGES is defined the state engine for the mysql protocol will log all state changes")

option(USE_GLIB_DEBUG_LOG "g_debug() is default defined to nothing, change to ON if you want to use g_debug()")
option(WITH_OPENSSL "Compile with OpenSSL")

EXECUTE_PROCESS(COMMAND git describe --tags
                TIMEOUT 5
                OUTPUT_VARIABLE GIT_REVISION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
MESSAGE(STATUS "building from ${GIT_REVISION}")
SET(CPACK_PACKAGE_VERSION_MAJOR "2")
SET(CPACK_PACKAGE_VERSION_MINOR "4")
SET(CPACK_PACKAGE_VERSION_PATCH "0")
SET(CPACK_PACKAGE_VENDOR "MySQL")
SET(PACKAGE_NAME cetus)

## SET(CPACK_*) before the INCLUDE(CPack)
INCLUDE(CPack)

MESSAGE(STATUS "gthread  at libs:${GTHREAD_LIBRARY_DIRS} incl:${GTHREAD_INCLUDE_DIRS}")
MESSAGE(STATUS "gmodule  at libs:${GMODULE_LIBRARY_DIRS} incl:${GMODULE_INCLUDE_DIRS}")

## if the paths are not set, use pkg-config to fetch them
IF(NOT GLIB_DEV_BASE_DIR)
    SET(GMODULE_INCLUDE_DIRS  CACHE PATH "gmodule-2.0 include dir [see pkg-config gmodule-2.0 --cflags]")
    SET(GMODULE_LIBRARY_DIRS  CACHE PATH "gmodule-2.0 library dir [see pkg-config gmodule-2.0 --libs]")
    SET(GTHREAD_INCLUDE_DIRS  CACHE PATH "gthread-2.0 include dir [see pkg-config gthread-2.0 --cflags]")
    SET(GTHREAD_LIBRARY_DIRS  CACHE PATH "gthread-2.0 library dir [see pkg-config gthread-2.0 --libs]")
ENDIF(NOT GLIB_DEV_BASE_DIR)
SET(MYSQL_INCLUDE_DIRS CACHE PATH "MySQL include dir")
SET(MYSQL_LIBRARY_DIRS CACHE PATH "MySQL library dir")

MESSAGE(STATUS "glib2.0  at libs:${GLIB_LIBRARY_DIRS} incl:${GLIB_INCLUDE_DIRS}")
MESSAGE(STATUS "gthread  at libs:${GTHREAD_LIBRARY_DIRS} incl:${GTHREAD_INCLUDE_DIRS}")
MESSAGE(STATUS "gmodule  at libs:${GMODULE_LIBRARY_DIRS} incl:${GMODULE_INCLUDE_DIRS}")

IF(NOT GLIB_INCLUDE_DIRS) 
    SET(__pkg_config_checked_GLIB 0)
    PKG_CHECK_MODULES(GLIB REQUIRED glib-2.0>=2.28)
    ADD_DEFINITIONS(-DHAVE_GLIB)
ENDIF(NOT GLIB_INCLUDE_DIRS) 

IF(NOT GMODULE_INCLUDE_DIRS) 
    PKG_CHECK_MODULES(GMODULE REQUIRED gmodule-2.0>=2.28)
    ADD_DEFINITIONS(-DHAVE_GMODULE)
ENDIF(NOT GMODULE_INCLUDE_DIRS) 

IF(NOT GTHREAD_INCLUDE_DIRS) 
    PKG_CHECK_MODULES(GTHREAD REQUIRED gthread-2.0>=2.28)
ENDIF(NOT GTHREAD_INCLUDE_DIRS) 

MACRO(_mysql_config VAR _regex _opt)
    EXECUTE_PROCESS(COMMAND ${MYSQL_CONFIG_EXECUTABLE} ${_opt}
        OUTPUT_VARIABLE _mysql_config_output
        )

    SET(_var   ${_mysql_config_output})
    STRING(REGEX MATCHALL "${_regex}([^ ]+)" _mysql_config_output "${_mysql_config_output}")
    STRING(REGEX REPLACE "^[ \t]+" "" _mysql_config_output "${_mysql_config_output}")
    STRING(REGEX REPLACE "[\r\n]$" "" _mysql_config_output "${_mysql_config_output}")
    STRING(REGEX REPLACE "${_regex}" "" _mysql_config_output "${_mysql_config_output}")
    SEPARATE_ARGUMENTS(_mysql_config_output)
    SET(${VAR} ${_mysql_config_output})
ENDMACRO(_mysql_config _regex _opt)

IF(NOT MYSQL_INCLUDE_DIRS)
    FIND_PROGRAM(MYSQL_CONFIG_EXECUTABLE NAMES mysql_config DOC "full path of mysql_config")
    IF(NOT MYSQL_CONFIG_EXECUTABLE)
        MESSAGE(SEND_ERROR "mysql_config wasn't found, -DMYSQL_CONFIG_EXECUTABLE=...")
    ENDIF(NOT MYSQL_CONFIG_EXECUTABLE)

    _MYSQL_CONFIG(MYSQL_INCLUDE_DIRS "(^| )-I" "--include")
    _MYSQL_CONFIG(MYSQL_LIBRARIES    "(^| )-l" "--libs")
    _MYSQL_CONFIG(MYSQL_LIBRARY_DIRS "(^| )-L" "--libs")
ELSE(NOT MYSQL_INCLUDE_DIRS)
    SET(MYSQL_LIBRARIES mysql)
ENDIF(NOT MYSQL_INCLUDE_DIRS) 

SET(CMAKE_REQUIRED_INCLUDES 
    ${MYSQL_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${GTHREAD_INCLUDE_DIRS}
)

MESSAGE(STATUS "checking in dirs: ${CMAKE_REQUIRED_INCLUDES}")

CHECK_INCLUDE_FILES(inttypes.h   HAVE_INTTYPES_H)
CHECK_INCLUDE_FILES(netinet/in.h HAVE_NETINET_IN_H)
CHECK_INCLUDE_FILES(net/if.h     HAVE_NET_IF_H)
CHECK_INCLUDE_FILES(net/if_dl.h  HAVE_NET_IF_DL_H)
CHECK_INCLUDE_FILES(stddef.h     HAVE_STDDEF_H)
CHECK_INCLUDE_FILES(stdint.h     HAVE_STDINT_H)
CHECK_INCLUDE_FILES(stdlib.h     HAVE_STDLIB_H)
CHECK_INCLUDE_FILES(signal.h     HAVE_SIGNAL_H)
CHECK_INCLUDE_FILES(syslog.h     HAVE_SYSLOG_H)
CHECK_INCLUDE_FILES(sys/filio.h  HAVE_SYS_FILIO_H)
CHECK_INCLUDE_FILES(sys/ioctl.h  HAVE_SYS_IOCTL_H)
CHECK_INCLUDE_FILES(sys/param.h  HAVE_SYS_PARAM_H)
CHECK_INCLUDE_FILES(sys/resource.h HAVE_SYS_RESOURCE_H)
CHECK_INCLUDE_FILES(sys/socket.h HAVE_SYS_SOCKET_H)
CHECK_INCLUDE_FILES(sys/sockio.h HAVE_SYS_SOCKIO_H)
CHECK_INCLUDE_FILES(sys/time.h   HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES(sys/types.h  HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILES(sys/uio.h    HAVE_SYS_UIO_H)
CHECK_INCLUDE_FILES(sys/un.h     HAVE_SYS_UN_H)
CHECK_INCLUDE_FILES(time.h       HAVE_TIME_H)
CHECK_INCLUDE_FILES(unistd.h     HAVE_UNISTD_H)
CHECK_INCLUDE_FILES(mysql.h      HAVE_MYSQL_H)
CHECK_INCLUDE_FILES(glib.h       HAVE_GLIB_H)
CHECK_INCLUDE_FILES(zlib.h       HAVE_ZLIB_H)
CHECK_INCLUDE_FILES(glib/gthread.h    HAVE_GTHREAD_H)
CHECK_INCLUDE_FILES(pwd.h        HAVE_PWD_H)

CHECK_FUNCTION_EXISTS(inet_ntop  HAVE_INET_NTOP)
CHECK_FUNCTION_EXISTS(getcwd     HAVE_GETCWD)
CHECK_FUNCTION_EXISTS(signal     HAVE_SIGNAL)
CHECK_FUNCTION_EXISTS(strerror   HAVE_STRERROR)
CHECK_FUNCTION_EXISTS(srandom    HAVE_SRANDOM)
CHECK_FUNCTION_EXISTS(writev     HAVE_WRITEV)
CHECK_FUNCTION_EXISTS(sigaction  HAVE_SIGACTION)
CHECK_FUNCTION_EXISTS(getaddrinfo     HAVE_GETADDRINFO)
# check for gthread actually being present
CHECK_LIBRARY_EXISTS(gthread-2.0 g_thread_init "${GTHREAD_LIBRARY_DIRS}" HAVE_GTHREAD)

CHECK_LIBRARY_EXISTS(tcmalloc malloc "" HAVE_TCMALLOC)

#SET(OLD_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
#SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${GTHREAD_LIBRARIES})
# fails for some reason use check_library_exists instead and hope it's better
#CHECK_FUNCTION_EXISTS(g_thread_init HAVE_GTHREAD)
#SET(CMAKE_REQUIRED_LIBRARIES ${OLD_CMAKE_REQUIRED_LIBRARIES})
if (WITH_OPENSSL)
  find_package(OpenSSL QUIET)
  if (OPENSSL_FOUND)
    message("-- OpenSSL version: ${OPENSSL_VERSION}")
    set(HAVE_OPENSSL 1)
  else(OPENSSL_FOUND)
    message("-- OpenSSL not found")
    set(OPENSSL_LIBRARIES "")
  endif(OPENSSL_FOUND)
endif(WITH_OPENSSL)

find_package(ZLIB REQUIRED)
if (ZLIB_FOUND)
  message("-- ZLIB version: ${ZLIB_VERSION_STRING}")
else(ZLIB_FOUND)
  message("-- ZLIB not found")
endif(ZLIB_FOUND)

IF(${HAVE_SYS_TYPES_H})
    SET(CMAKE_EXTRA_INCLUDE_FILES sys/types.h)
    CHECK_TYPE_SIZE(ulong HAVE_ULONG)
    SET(CMAKE_EXTRA_INCLUDE_FILES )
ENDIF(${HAVE_SYS_TYPES_H})

IF(${HAVE_SYS_SOCKET_H})
    SET(CMAKE_EXTRA_INCLUDE_FILES sys/socket.h)
    CHECK_TYPE_SIZE(socklen_t        HAVE_SOCKLEN_T)
    SET(CMAKE_EXTRA_INCLUDE_FILES )
ENDIF(${HAVE_SYS_SOCKET_H})


IF(${HAVE_SYS_RESOURCE_H})
    SET(CMAKE_EXTRA_INCLUDE_FILES sys/resource.h)
    CHECK_TYPE_SIZE(rlim_t        SIZEOF_RLIM_T)
    SET(CMAKE_EXTRA_INCLUDE_FILES )
ENDIF(${HAVE_SYS_RESOURCE_H})

SET(OLD_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
MESSAGE(STATUS "CMAKE_REQUIRED_LIBRARIES is ${CMAKE_REQUIRED_LIBRARIES}")
SET(CMAKE_REQUIRED_LIBRARIES ${OLD_CMAKE_REQUIRED_LIBRARIES})

IF(GIT_REVISION)
  ADD_DEFINITIONS(-DCHASSIS_BUILD_TAG="${GIT_REVISION}")
ENDIF(GIT_REVISION)

CONFIGURE_FILE(config.h.cmake config.h)
ADD_DEFINITIONS(-DHAVE_CONFIG_H)

SET(PACKAGE_VERSION_ID "(${CPACK_PACKAGE_VERSION_MAJOR} << 16 | ${CPACK_PACKAGE_VERSION_MINOR} << 8 | ${CPACK_PACKAGE_VERSION_PATCH})")
SET(PACKAGE_VERSION_STRING "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
SET(PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
SET(PACKAGE_STRING "${CMAKE_PROJECT_NAME} ${CPACK_PACKAGE_VERSION}")
ADD_DEFINITIONS(
    -DPACKAGE_VERSION_ID=${PACKAGE_VERSION_ID}
    -DPACKAGE_VERSION="${PACKAGE_VERSION}"
    -DPACKAGE_STRING="${PACKAGE_STRING}"
    -DPACKAGE="${CMAKE_PROJECT_NAME}"
)

add_subdirectory(libev)
ADD_SUBDIRECTORY(src)
ADD_SUBDIRECTORY(plugins)
IF(EXISTS examples)
    ADD_SUBDIRECTORY(examples)
ENDIF(EXISTS examples)
ADD_SUBDIRECTORY(lib)

CONFIGURE_FILE(mysql-chassis.pc.cmake mysql-chassis.pc @ONLY)
CONFIGURE_FILE(cetus.pc.cmake cetus.pc @ONLY)
INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mysql-chassis.pc 
    DESTINATION lib/pkgconfig/
    )
INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/cetus.pc 
    DESTINATION lib/pkgconfig/
    )
INSTALL(FILES README_XA
    DESTINATION logs
    )
install(FILES
  doc/users.json.example
  doc/variables.json.example
  doc/sharding.json.example
  doc/shard.conf.example
  doc/proxy.conf.example
  DESTINATION conf
  )

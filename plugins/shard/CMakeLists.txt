#  $%BEGINLICENSE%$
#  Copyright (c) 2009, Oracle and/or its affiliates. All rights reserved.
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
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/src/)
INCLUDE_DIRECTORIES(${PROJECT_BINARY_DIR}) # for config.h

include_directories(${PROJECT_SOURCE_DIR}/lib)
include_directories(${PROJECT_BINARY_DIR}/lib)
include_directories(${PROJECT_SOURCE_DIR}/libev)
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR})

INCLUDE_DIRECTORIES(${GLIB_INCLUDE_DIRS})
INCLUDE_DIRECTORIES(${MYSQL_INCLUDE_DIRS})

LINK_DIRECTORIES(${GLIB_LIBRARY_DIRS})
LINK_DIRECTORIES(${LIBINTL_LIBRARY_DIRS})
LINK_DIRECTORIES(${MYSQL_LIBRARY_DIRS})

SET(_plugin_name shard)

set(SHARD_SOURCES
  ${_plugin_name}-plugin.c
  sharding-parser.c
  )
ADD_LIBRARY(${_plugin_name} SHARED ${SHARD_SOURCES})
TARGET_LINK_LIBRARIES(${_plugin_name} mysql-chassis-proxy)
if(HAVE_TCMALLOC)
TARGET_LINK_LIBRARIES(${_plugin_name} tcmalloc)
endif(HAVE_TCMALLOC)

if(SIMPLE_PARSER)
  target_compile_definitions(${_plugin_name} PRIVATE SIMPLE_PARSER=1)
endif(SIMPLE_PARSER)

CHASSIS_PLUGIN_INSTALL(${_plugin_name})


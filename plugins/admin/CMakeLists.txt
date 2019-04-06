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
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/lib/)
INCLUDE_DIRECTORIES(${PROJECT_BINARY_DIR}) # for config.h
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR})
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/libev)
INCLUDE_DIRECTORIES(${GLIB_INCLUDE_DIRS})
INCLUDE_DIRECTORIES(${MYSQL_INCLUDE_DIRS})

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

LINK_DIRECTORIES(${GLIB_LIBRARY_DIRS})
LINK_DIRECTORIES(${LIBINTL_LIBRARY_DIRS})
LINK_DIRECTORIES(${MYSQL_LIBRARY_DIRS})

set(_plugin_name admin)

find_package(FLEX)
set(ADMIN_LEXER_SRC ${CMAKE_CURRENT_BINARY_DIR}/admin-lexer.l.c)
set(ADMIN_LEXER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/admin-lexer.l.h)
FLEX_TARGET(AdminLexer lexer.l ${ADMIN_LEXER_SRC} COMPILE_FLAGS
  "--header-file=${ADMIN_LEXER_HEADER}")
set_source_files_properties(${ADMIN_LEXER_SRC} PROPERTIES GENERATED 1)
set_source_files_properties(${ADMIN_LEXER_HEADER} PROPERTIES GENERATED 1)

set(PARSER_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/admin-parser.y.c)
set(OUTPUT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/admin-parser.y.h)
set(GRAMMAR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/admin-parser.y)
set(LEMON_TEMPLATE_FILE ${CETUS_TOOLS_DIR}/lempar.c)
add_custom_command(
  OUTPUT  ${PARSER_OUTPUT} ${SQL_TOKENS_HEADER}
  DEPENDS ${LEMON_TEMPLATE_FILE} ${GRAMMAR_FILE}
  COMMAND lemon -q -T${LEMON_TEMPLATE_FILE} ${GRAMMAR_FILE} -o${PARSER_OUTPUT} -h${OUTPUT_HEADER}
)
set_source_files_properties(${PARSER_OUTPUT}  PROPERTIES GENERATED 1)
set_source_files_properties(${OUTPUT_HEADER}  PROPERTIES GENERATED 1)


ADD_LIBRARY(${_plugin_name} SHARED
  admin-plugin.c
  admin-commands.c
  admin-stats.c
  ${ADMIN_LEXER_SRC}
  ${PARSER_OUTPUT}
)

TARGET_LINK_LIBRARIES(${_plugin_name} mysql-chassis-proxy)
CHASSIS_PLUGIN_INSTALL(${_plugin_name})


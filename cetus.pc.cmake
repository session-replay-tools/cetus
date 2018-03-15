prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=@CMAKE_INSTALL_PREFIX@
libdir=@CMAKE_INSTALL_PREFIX@/lib
pkglibdir=${libdir}/cetus
plugindir=${pkglibdir}/plugins

Name: cetus
Version: @PACKAGE_VERSION_STRING@
Description: cetus
URL: https://github.com/Lede-Inc/cetus
Requires: glib-2.0 >= 2.16, mysql-chassis >= @PACKAGE_VERSION_STRING@
Libs: -L${libdir} -lmysql-chassis-proxy

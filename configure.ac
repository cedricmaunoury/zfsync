#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([zfsync],[0.001])
AM_INIT_AUTOMAKE([foreign])
#AC_CONFIG_SRCDIR([src/zfsync_send.c src/zfsync_recv.c])
AC_CONFIG_SRCDIR([src/])

README: README.md

# Checks for programs.
AC_PROG_CC

build_linux=no
build_freebsd=no


host_os=$(uname -s)

case "${host_os}" in
	Linux*)
		build_linux=yes
		;;
	FreeBSD*)
		build_freebsd=yes
		;;
	*)
		AC_MSG_ERROR(["OS $host_os is not supported"])
		;;
esac

LIBS="-lzfs -lnvpair -lzfs_core -luutil"

if [[ "${build_freebsd}" = "yes" ]]; then
	LIBS="-lgeom ${LIBS}"

fi
if [[ "${build_linux}" = "yes" ]]; then
	zfs_version=$(cat /sys/module/zfs/version|awk -F'-' '{print $1}')
fi


AC_ARG_VAR(openzfs, AS_HELP_STRING([Force configure to run OpenZFS checks]))
AC_ARG_VAR(freebsd_src, AS_HELP_STRING([Provide path for freebsd source tree. This defaults to /usr/src/]))
AC_ARG_VAR(patched, AS_HELP_STRING([Use patched version of OpenZFS]))

if [[ "$prefix" = "NONE" ]]; then
	header_prefix=/usr/local
else
	header_prefix=$prefix
fi

: ${freebsd_src:=/usr/src}

openzfs=false

if [[ -f "/usr/include/libzfs/libzfs.h" ]] || [[ "$openzfs" = true ]]; then
	openzfs=true

	#PKG_CHECK_MODULES(LIBZFS, libzfs >= 2.0.0)

	CFLAGS="${CFLAGS} \
-I/usr/include/libzfs \
-I/usr/include/libspl \
-I/usr/src/zfs-${zfs_version}/include \
-D_MACHINE_ENDIAN_H_ -DHAVE_ISSETUGID -D_GNU_SOURCE -DHAVE_ZOL"

	LDFLAGS="-L${header_prefix}/lib ${LDFLAGS}"

	LIBS="-lpthread ${LIBS}"

elif [[ -d "${freebsd_src}/sys/contrib/openzfs" ]]; then
	openzfs=true

	openzfs_prefix="${freebsd_src}/sys/contrib/openzfs"

	CFLAGS="${CFLAGS} \
-DIN_BASE -DHAVE_RPC_TYPES \
-I${openzfs_prefix}/include \
-I${openzfs_prefix}/include/os/freebsd \
-I${openzfs_prefix}/lib/libspl \
-I${openzfs_prefix}/lib/libspl/include \
-I${openzfs_prefix}/lib/libspl/include/os/freebsd \
-I${openzfs_prefix}/lib/libnvpair \
-I${openzfs_prefix}/lib/libumem/common \
-I${openzfs_prefix}/lib/libuutil/common \
-I${openzfs_prefix}/lib/libzfs/common \
-I${openzfs_prefix}/lib/libzfs_core/common \
-I${openzfs_prefix}/lib/libzpool/common \
-I${freebsd_src}/sys \
-I${freebsd_src}/cddl/contrib/opensolaris/head \
-I${freebsd_src}/cddl/compat/opensolaris/include \
-I${freebsd_src}/cddl/compat/opensolaris/lib/libumem \
-I${freebsd_src}/cddl/lib/libumem \
-I${freebsd_src}/sys/cddl/compat/opensolaris \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/common/zfs \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/uts/common \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/uts/common/fs/zfs \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/uts/common/sys \
-include ${openzfs_prefix}/include/os/freebsd/spl/sys/ccompile.h \
-DHAVE_ISSETUGID -DHAVE_ZOL"

	LIBS="-lspl -lzutil ${LIBS}"

	LDFLAGS="-L/usr/obj/usr/src/amd64.amd64/lib/msun -lm \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libnvpair -lnvpair \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libumem -lumem \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzpool -lzpool \
-L/usr/obj/usr/src/amd64.amd64/lib/libthr -lpthread \
-L/usr/obj/usr/src/amd64.amd64/lib/libz -lz \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libavl -lavl \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzfs_core -lzfs_core \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzfs -lzfs"

else

        CFLAGS="${CFLAGS} \
-I${freebsd_src}/cddl/compat/opensolaris/include \
-I${freebsd_src}/cddl/compat/opensolaris/lib/libumem \
-I${freebsd_src}/cddl/contrib/opensolaris/head \
-I${freebsd_src}/cddl/contrib/opensolaris/lib/libnvpair \
-I${freebsd_src}/cddl/contrib/opensolaris/lib/libumem/common \
-I${freebsd_src}/cddl/contrib/opensolaris/lib/libuutil/common \
-I${freebsd_src}/cddl/contrib/opensolaris/lib/libzfs/common \
-I${freebsd_src}/cddl/contrib/opensolaris/lib/libzfs_core/common \
-I${freebsd_src}/cddl/contrib/opensolaris/lib/libzpool/common \
-I${freebsd_src}/cddl/lib/libumem \
-I${freebsd_src}/sys/cddl/compat/opensolaris \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/common/zfs \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/uts/common \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/uts/common/fs/zfs \
-I${freebsd_src}/sys/cddl/contrib/opensolaris/uts/common/sys \
-DNEED_SOLARIS_BOOLEAN"

	LDFLAGS="-L/usr/obj/usr/src/amd64.amd64/lib/msun -lm \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libnvpair -lnvpair \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libumem -lumem \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzpool -lzpool \
-L/usr/obj/usr/src/amd64.amd64/lib/libthr -lpthread \
-L/usr/obj/usr/src/amd64.amd64/lib/libz -lz \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libavl -lavl \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzfs_core -lzfs_core \
-L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzfs -lzfs"

fi

if [[ "$patched" = true ]]; then
	CFLAGS="${CFLAGS} -DHAVE_OPENZFS_DOALL_PATCH"
fi

# Checks for header files.
AC_CHECK_HEADERS([netdb.h netinet/in.h stdlib.h sys/socket.h sys/time.h unistd.h sys/zfs_ioctl.h])

if [[ "$openzfs" = false ]];  then
	AC_CHECK_HEADERS([libzfs.h], [], [AC_MSG_ERROR(A working libzfs header is required)], [
		#include <sys/types.h>
		#include <sys/mnttab.h>
		#include <sys/fs/zfs.h>
		#include <sys/zfs_ioctl.h>
	])
else
	AC_CHECK_HEADERS([libzfs.h], [], [AC_MSG_ERROR(A working libzfs header is required)], [
		#include <sys/types.h>
		#include <sys/mnttab.h>
		#include <sys/fs/zfs.h>
	])
fi

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_CHECK_FUNCS([bzero gettimeofday memset mkdir socket strrchr strstr])

AC_CONFIG_FILES([Makefile
                 man/Makefile
                 src/Makefile
		 script/Makefile])

AC_OUTPUT

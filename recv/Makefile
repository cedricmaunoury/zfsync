# $FreeBSD: releng/12.2/cddl/sbin/zfs/Makefile 337160 2018-08-02 18:55:55Z mav $

#.PATH: ${SRCTOP}/cddl/contrib/opensolaris/cmd/zfs
.PATH: /home/gc/zfsha

PROG=	zfsync_recv
MAN=	zfsync_recv.8
SRCS=	zfsync_recv.c

WARNS?=	0
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/lib/libzpool/common
CFLAGS+= -I${SRCTOP}/cddl/compat/opensolaris/include
CFLAGS+= -I${SRCTOP}/cddl/compat/opensolaris/lib/libumem
CFLAGS+= -I${SRCTOP}/sys/cddl/compat/opensolaris
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/head
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/lib/libuutil/common
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/lib/libzfs/common
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/lib/libzfs_core/common
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/lib/libumem/common
CFLAGS+= -I${SRCTOP}/cddl/contrib/opensolaris/lib/libnvpair
CFLAGS+= -I${SRCTOP}/sys/cddl/contrib/opensolaris/uts/common
CFLAGS+= -I${SRCTOP}/sys/cddl/contrib/opensolaris/uts/common/fs/zfs
CFLAGS+= -I${SRCTOP}/sys/cddl/contrib/opensolaris/common/zfs

CFLAGS+= -DNEED_SOLARIS_BOOLEAN

#LIBADD=	jail nvpair uutil zfs_core zfs
#LDADD=	m nvpair umem zpool z avl zfs_core zfs pthread
LDADD= -L/usr/obj/usr/src/amd64.amd64/lib/msun -lm -L/usr/obj/usr/src/amd64.amd64/cddl/lib/libnvpair -lnvpair -L/usr/obj/usr/src/amd64.amd64/cddl/lib/libumem -lumem -L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzpool -lzpool -L/usr/obj/usr/src/amd64.amd64/lib/libthr -lpthread -L/usr/obj/usr/src/amd64.amd64/lib/libz -lz -L/usr/obj/usr/src/amd64.amd64/cddl/lib/libavl -lavl -L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzfs_core -lzfs_core -L/usr/obj/usr/src/amd64.amd64/cddl/lib/libzfs -lzfs

.include <bsd.prog.mk>

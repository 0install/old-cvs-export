HELPER_USER := @HELPER_USER@
ROOT_PREFIX = @ROOT_PREFIX@
FSTAB := /etc/fstab
myexecbindir := /bin

AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = 0show 0refresh
myexecbin_PROGRAMS = 0run
sbin_PROGRAMS = zero-install

SUBDIRS = Linux

EXTRA_DIST = Linux/lazyfs.c Linux/lazyfs.h Linux/Makefile.in Technical 0test.py
DISTCHECK_CONFIGURE_FLAGS = --with-user=zeroinst --with-distcheck

zero_install_SOURCES = zero-install.c support.c fetch.c control.c index.c \
		       zero-install.h support.h fetch.h control.h index.h \
		       global.h task.c task.h

INCLUDES = `pkg-config --cflags libxml-2.0`
zero_install_LDFLAGS = `pkg-config --libs libxml-2.0`

install-exec-local:
	make uninstall-local
	[ -n "${ROOT_PREFIX}" ] || make install-real

install-real:
	[ ! -L /uri/.lazyfs-cache ] || umount /uri
	[ -d /uri ] || mkdir /uri
	[ -d /uri/0install ] || mkdir /uri/0install
	[ ! -d /uri/0http -o -L /uri/0http ] || rmdir /uri/0http
	[ -L /uri/0http ] || ln -s 0install /uri/0http
	[ -L /uri/http ] || ln -s 0install /uri/http
	[ -d /var/cache/zero-inst ] || mkdir -m 0755 /var/cache/zero-inst
	chown "${HELPER_USER}" /var/cache/zero-inst
	echo "lazyfs	/uri/0install	lazyfs	/var/cache/zero-inst	0 0" >> ${FSTAB}

uninstall-local:
	if [ -z "${ROOT_PREFIX}" ]; then \
	  grep -v "^lazyfs[ 	]*/uri" "${FSTAB}" > "${FSTAB}.new" && \
	  mv "${FSTAB}.new" "${FSTAB}"; fi

CFLAGS = -O2 -Wall -Wstrict-prototypes -g

# Test program using glib

# CORE := /uri/http/zero-install.sourceforge.net/libs/core/1.0/Linux-i386
# 
# hello_glib.o: hello_glib.c
# 	PKG_CONFIG_PATH=${PKG_CONFIG_PATH} ${CC} -c -Wall `${CORE}/bin/pkg-config --cflags glib-2.0` -o $@ $<
# 
# hello_glib: hello_glib.o
# 	PKG_CONFIG_PATH=${PKG_CONFIG_PATH} gcc `${CORE}/bin/pkg-config --libs glib-2.0` -o $@ $<
# EXTRA_DIST = hello_glib.c

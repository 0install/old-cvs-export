HELPER_USER := @HELPER_USER@
ROOT_PREFIX = @ROOT_PREFIX@
FSTAB := /etc/fstab

AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = 0show 0refresh
sbin_PROGRAMS = zero-install

SUBDIRS = Linux

EXTRA_DIST = Linux/lazyfs.c Linux/lazyfs.h Linux/Makefile.in
DISTCHECK_CONFIGURE_FLAGS = --with-user=zeroinst --with-distcheck

zero_install_SOURCES = zero-install.c support.c fetch.c control.c index.c \
		       zero-install.h support.h fetch.h control.h index.h \
		       global.h

zero_install_LDADD = -lexpat

install-exec-local:
	([ -d /uri ] || mkdir /uri) && \
 	([ -d /var/cache/zero-inst ] || mkdir -m 0755 /var/cache/zero-inst) && \
	([ -n "${ROOT_PREFIX}" ] || \
 	 chown "${HELPER_USER}" /var/cache/zero-inst && \
 	 grep -q "^lazyfs" ${FSTAB} ||	echo "lazyfs          /uri            lazyfs  /var/cache/zero-inst            0 0" >> ${FSTAB})

uninstall-local:
	if [ -z "${ROOT_PREFIX}" ]; then \
	  grep -v "^lazyfs[ \t]*/uri[ \t]" "${FSTAB}" > "${FSTAB}.new" && \
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

HELPER_USER := @HELPER_USER@
ROOT_PREFIX = @ROOT_PREFIX@
FSTAB := /etc/fstab
myexecbindir := ${ROOT_PREFIX}/bin
initddir := ${ROOT_PREFIX}/etc/init.d

AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = 0refresh
myexecbin_PROGRAMS = 0run
sbin_PROGRAMS = zero-install
initd_SCRIPTS = 0install

SUBDIRS = Linux

EXTRA_DIST = Linux/lazyfs.c Linux/lazyfs.h Linux/compat20.h Linux/Makefile.in Technical 0test.py test_cases.py
DISTCHECK_CONFIGURE_FLAGS = --with-user=zeroinst --with-distcheck

zero_install_SOURCES = zero-install.c support.c fetch.c control.c index.c \
		       zero-install.h support.h fetch.h control.h index.h \
		       global.h task.c task.h gpg.c gpg.h

INCLUDES = `pkg-config --cflags libxml-2.0 dbus-1`
zero_install_LDFLAGS = `pkg-config --libs libxml-2.0 dbus-1`
0refresh_LDFLAGS = `pkg-config --libs libxml-2.0 dbus-1`

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
	echo "Mounting Zero Install filesystem"
	[ ! -L /uri/0install/.lazyfs-cache ] || (echo "Already mounted!"; exit 1)
	mount /uri/0install

uninstall-local:
	if [ -z "${ROOT_PREFIX}" ]; then \
	  grep -v "^lazyfs[ 	]*/uri" "${FSTAB}" > "${FSTAB}.new" && \
	  mv "${FSTAB}.new" "${FSTAB}"; fi

0install: 0install.in config.status
	sed "s:\@zero-install\@:${sbindir}/zero-install:" 0install.in | \
	 sed "s/\@helper_user\@/${HELPER_USER}/" >0install
	chmod a+x 0install

CFLAGS = -O2 -Wall -Wstrict-prototypes -g -Wmissing-prototypes

HELPER_USER := @HELPER_USER@
ROOT_PREFIX = @ROOT_PREFIX@
FSTAB := /etc/fstab
myexecbindir := ${ROOT_PREFIX}/bin
initddir := ${ROOT_PREFIX}/@INITDIR@

AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = 0refresh
myexecbin_PROGRAMS = 0run
sbin_PROGRAMS = zero-install
initd_SCRIPTS = 0install

EXTRA_DIST = Technical 0test.py test_cases.py 0install.in
DISTCHECK_CONFIGURE_FLAGS = --with-user=zeroinst --with-distcheck

zero_install_SOURCES = zero-install.c support.c fetch.c control.c index.c \
		       zero-install.h support.h fetch.h control.h index.h \
		       interface.h list.c list.h mirrors.c mirrors.h global.h \
		       task.c task.h gpg.c gpg.h xml.c xml.h
CLEANFILES = 0install

INCLUDES = `pkg-config --cflags dbus-1`
zero_install_LDFLAGS = `pkg-config --libs dbus-1` -lexpat
0refresh_LDFLAGS = `pkg-config --libs dbus-1`

install-exec-local: uninstall-local
	@[ -n "${ROOT_PREFIX}" ] || make install-real

uninstall-local:
	@[ -n "${ROOT_PREFIX}" ] || make uninstall-real

install-real:
	@echo
	@echo Creating directories...
	[ -d /uri ] || mkdir /uri
	[ -d /uri/0install ] || mkdir /uri/0install
	[ -d /var/cache/zero-inst ] || mkdir -m 0755 /var/cache/zero-inst
	chown "${HELPER_USER}" /var/cache/zero-inst
	@echo
	@echo Updating ${FSTAB}...
	echo "lazyfs	/uri/0install	lazyfs@LAZYFS_VERSION@	/var/cache/zero-inst	0 0" >> ${FSTAB}
	@echo
	@echo Stopping any previous version...
	@${initddir}/0install stop 2>/dev/null || echo "(not yet running)"
	@[ ! -L /uri/0install/.lazyfs-cache ] || umount -l /uri/0install

uninstall-real:
	grep -v "^lazyfs[ 	]*/uri" "${FSTAB}" > "${FSTAB}.new" && \
	mv "${FSTAB}.new" "${FSTAB}"

0install: 0install.in config.status
	sed "s:\@zero-install\@:${sbindir}/zero-install:" $(srcdir)/0install.in | \
	 sed "s/\@helper_user\@/${HELPER_USER}/" >0install
	chmod a+x 0install

splint:
	splint 0refresh.c +unixlib -predboolint -boolops +boolint +matchanyintegral `pkg-config --cflags dbus-1` -I.

CFLAGS = -O2 -Wall -Wstrict-prototypes -g -Wmissing-prototypes -Wwrite-strings

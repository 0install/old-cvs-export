import os, time
from os.path import expanduser, join, ismount

# You can edit these three lines to suit...
test_dir = expanduser('~/0inst-test')	# Where to put test files
version = '0.1.25'				# Version of lazyfs to use

log = file('log', 'w', 1)
print >>log, "Log for zero install test run on %s" % time.ctime()

version = version.replace('.', 'd')
fs = join(test_dir, '0install')
cache = join(test_dir, 'cache')
site = join(test_dir, 'site')
www = join(test_dir, 'www')

os.system('sync')	# Just in case we crash the kernel ;-)

if os.getuid() == 0:
	print "Unit-tests must not be run as root."
	sys.exit()

# Setup the test environment...

if not os.path.isdir(test_dir):
	os.mkdir(test_dir)

if ismount(fs):
	os.system("sudo umount '%s'" % fs)
	assert not ismount(fs)

os.environ['DEBUG_URI_0INSTALL_DIR'] = fs

#!/usr/bin/env python
from multitest import Actor, ThreadOut
import os, sys, unittest, time
import lazyfs
from os.path import realpath, basename, dirname, join
import signal
from server import Webserver
from support import build
from config import log, fs, cache, site
import codecs

def to_utf8(s):
	return codecs.utf_8_encode(s)[0]

zero_install = join(dirname(dirname(realpath(sys.argv[0]))), 'zero-install')

print "Logging to logfile '%s'" % log.name
assert os.path.exists(zero_install)

webserver = Webserver()
user = Actor('User')

if os.system('gpg --list-secret-keys 0install@foo.com > /dev/null'):
	print 'Please create a key for "Zero Install <0install@foo.com>"'
	print 'eg: gpg --gen-key'
	sys.exit()

if os.system('''sudo -v -p "I need to use sudo to get root access in order to
mount and unmount the lazyfs filesystem.
Please enter %u's password:"'''):
	print 'Sudo failed. Make sure it is installed and configured (visudo).'
	sys.exit()

sys.stdout = ThreadOut(log)

class TestSimple(lazyfs.LazyFSTest):
	actors = (user, webserver)

	def setUp(self):
		lazyfs.LazyFSTest.setUp(self)
		self.zero_pid = os.fork()
		if self.zero_pid == 0:
			os.dup2(log.fileno(), 1)
			os.dup2(log.fileno(), 2)
			try:
				os.execl(zero_install, zero_install, '--debug')
			finally:
				os._exit(1)
		while not os.path.exists(join(cache, '.control2')):
			print "Waiting..."
			time.sleep(0.1)
	
	def tearDown(self):
		os.kill(self.zero_pid, signal.SIGTERM)
		os.waitpid(self.zero_pid, 0)
		lazyfs.LazyFSTest.tearDown(self)

	def test01Nothing(self):
		self.assertLs(['...', '.control2', '.0inst-pid'], cache)
	
	def test02Fail(self):
		"""Server refuses the connection. Client gets an IO error."""
		if user():
			print join(fs, 'foo.com')
			try:
				self.assertLs([], join(fs, 'foo.com'))
				assert 0
			except:
				pass
		if webserver():
			webserver.reject('foo.com')

	def test03EmptySite(self):
		if user():
			print join(fs, 'foo.com')
			self.assertLs([], join(fs, 'foo.com'))
		if webserver():
			build('foo.com')
			webserver.handle_index('foo.com')
			webserver.handle_any('foo.com')	# The index.bz

	def test04ReadFile(self):
		if user():
			print join(fs, 'foo.com')
			self.assertLs(['hello'], join(fs, 'foo.com'))
		if webserver():
			a = file(join(site, 'hello'), 'w')
			a.write('World')
			a.close()
			build('foo.com')
			webserver.handle_index('foo.com')
			webserver.handle_any('foo.com')	# The index.bz

		self.sync()

		if user():
			self.assertEquals('World', file(join(fs, 'foo.com/hello')).read())
		if webserver():
			webserver.handle_any('foo.com')	# The file
	
	def test05InvalidSites(self):
		"""Ensure that requesting impossible names doesn't generate requests."""
		if user():
			def must_fail(site):
				try:
					self.assertLs(['error'], join(fs, site))
					raise Exception('%s should be invalid!' % site)
				except:
					pass
			must_fail('.foo.com')
			must_fail("0")
			must_fail("`")
			must_fail("{")

			must_fail("AppRun")
			must_fail(".DirIcon")

			must_fail("a..b")
			must_fail("a--b-")

			must_fail(u"rox.sourceforge.net~foo.\u00a3?")
			must_fail("some name.com")

			must_fail("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")

			must_fail("b.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
				  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")

	def test06ValidSites(self):
		def ok(site):
			if user():
				try:
					self.assertLs(['error'], to_utf8(join(fs, site)))
					raise Exception('%s should be rejected!' % site)
				except:
					pass
			if webserver():
				webserver.reject(site)
			self.sync()
		ok("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		ok("b.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		ok("a")
		ok("z")
		ok("a.b")
		ok("a--b")
		ok("zero-install.sourceforge.net")
		ok("rox.sourceforge.net")
		ok(u"rox.sourceforge.net#~foo.\u00a3?")
		ok("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
		   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
		   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
		   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		ok("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
	           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
		   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
		   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")


# Run the tests
sys.argv.append('-v')
unittest.main()

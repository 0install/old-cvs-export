#!/usr/bin/env python
import unittest
import sys
import os, signal, time
import traceback
import mmap

# You can edit these three lines to suit...
test_dir = os.path.expanduser('~/lazyfs-test')	# Where to put test files
version = '0.1.23'				# Version of lazyfs to test
verbose = False					# Give extra debug information

if len(sys.argv) > 1 and sys.argv[1] == '--read-hello':
	fd = os.open('hello', os.O_RDONLY)
	try:
		print os.read(fd, 100)
	except OSError:
		print "IOError"
	sys.exit()

prog_name = os.path.abspath(sys.argv[0])

if os.getuid() == 0:
	print "Unit-tests must not be run as root."
	sys.exit()

# Setup the test environment...
version = version.replace('.', 'd')

if not os.path.isdir(test_dir):
	os.mkdir(test_dir)
fs = os.path.join(test_dir, '0install')
cache = os.path.join(test_dir, 'cache')

if os.path.ismount(fs):
	os.system("sudo umount " + fs)
	assert not os.path.ismount(fs)

os.system("sudo rmmod lazyfs" + version)

if not os.path.isdir(fs):
	print "Creating", fs
	os.mkdir(fs)
if not os.path.isdir(cache):
	print "Creating", cache
	os.mkdir(cache)
uid = os.geteuid()
cache_uid = os.stat(cache).st_uid 
if cache_uid != uid:
	raise Exception('%s must be owned by user %s (not %s)' %
			(cache, uid, cache_uid))

# Base classes for the test cases
class LazyFS(unittest.TestCase):
	def setUp(self):
		for item in os.listdir(cache):
			os.system("rm -r '%s'" % os.path.join(cache, item))
		assert not os.path.ismount(fs)
		os.system("sudo mount -t lazyfs%s lazyfs %s -o %s" % (version, fs, cache))

	def tearDown(self):
		os.system("sudo umount " + fs)
	
def cstest(base):
	def test(self):
		assert self.c is not None
		client = getattr(self, 'client' + base)
		server = getattr(self, 'server' + base)
		child = os.fork()
		if child == 0:
			try:
				os.close(self.c)
				self.c = None
				client()
			except:
				print "Error from client:"
				traceback.print_exc()
				os._exit(1)
			os._exit(0)
		else:
			try:
				server()
				error = None
			except Exception, ex:
				error = ex
			for x in range(10):
				died, status = os.waitpid(child, os.WNOHANG)
				if died == child:
					if status and not error:
						error = Exception('Error in child')
					break
				time.sleep(0.1)
			else:
				print "Killing", child
				os.kill(child, signal.SIGTERM)
				os.waitpid(child, 0)
				if not error:
					error = Exception('Error in child')
			if error:
				raise error or Exception("Child didn't quit")
	return test

class WithHelper(LazyFS):
	def setUp(self):
		LazyFS.setUp(self)
		self.c = os.open(fs + '/.lazyfs-helper', os.O_RDONLY)
	
	def tearDown(self):
		if self.c is not None:
			os.close(self.c)
		self.c = None
		LazyFS.tearDown(self)

# The actual tests

class Test1WithoutHelper(LazyFS):
	def test1CacheLink(self):
		assert os.path.islink(fs + '/.lazyfs-cache')
		assert os.readlink(fs + '/.lazyfs-cache') == cache
	
	def test2UnconnectedEmpty(self):
		try:
			file(fs + '/f')
			assert 0
		except IOError:
			pass

class Test2WithHelper(WithHelper):
	def read_rq(self):
		if verbose:
			print "Server: reading request..."
		fd = os.read(self.c, 1000)
		assert fd[-1] == '\0'
		if verbose:
			print "Server: got request " + fd
		return int(fd.split(' ', 1)[0])
	
	def next(self):
		fd = self.read_rq()
		path = os.read(fd, 1000)
		assert path[-1] == '\0'
		if verbose:
			print "Server: got request '" + path[:-1] + "'"
		return (fd, path[:-1])
	
	def assert_ls(self, dir, ls):
		real = os.listdir(dir)
		#os.system("dmesg| tail -15")
		real.sort()
		ls = ls[:]
		ls.sort()
		self.assertEquals(ls, real)

	def clientNothing(self): pass
	def serverNothing(self): pass
	test01Nothing = cstest('Nothing')
	
	def clientReleaseHelper(self):
		os.open(fs + '/.lazyfs-helper', os.O_RDONLY)
	
	def serverReleaseHelper(self):
		os.close(self.c)
		self.c = None

	test02ReleaseHelper = cstest('ReleaseHelper')
	
	def clientDoubleOpen(self):
		# Check that we can't open the helper a second time
		try:
			c = os.open(fs + '/.lazyfs-helper', os.O_RDONLY)
			assert 0
		except OSError:
			pass

	def serverDoubleOpen(self):
		# This is a request for /. Ignore it.
		fd = self.read_rq()
		os.close(fd)

	test03DoubleOpen = cstest('DoubleOpen')

	def clientLsRoot(self):
		self.assert_ls(fs, ['.lazyfs-helper', '.lazyfs-cache'])
	def serverLsRoot(self):
		self.send_dir('/', [])

	test04LsRoot = cstest('LsRoot')

	def clientGetDir(self):
		self.assert_ls(fs + '/rox', ['apps'])
	
	def serverGetDir(self):
		fd, path = self.next()
		self.assertEquals('/', path)
		f = file(cache + '/...', 'w').write('LazyFS\nd 1 1 rox\0')
		os.close(fd)

		fd, path = self.next()
		self.assertEquals('/rox', path)
		os.mkdir(cache + '/rox')
		f = file(cache + '/rox/...', 'w').write('LazyFS\nd 1 1 apps\0')
		os.close(fd)
	
	test05GetDir = cstest('GetDir')

	def put_dir(self, dir, contents):
		"""contents is None for a dynamic directory"""
		f = file(cache + dir + '/....', 'w')
		if contents is None:
			f.write('LazyFS Dynamic\n')
		elif contents:
			f.write('LazyFS\n' + '\0'.join(contents) + '\0')
		else:
			f.write('LazyFS\n')
		f.close()
		os.rename(cache + dir + '/....', cache + dir + '/...')
	
	def put_file(self, rq, contents, mtime):
		f = file(cache + rq, 'w')
		f.write(contents)
		f.close()
		os.utime(cache + rq, (mtime, mtime))

	def send_dir(self, dir, contents = None):
		assert dir.startswith('/')
		fd, path = self.next()
		self.assertEquals(dir, path)
		self.put_dir(dir, contents)
		os.close(fd)
	
	def send_file(self, rq, contents, mtime):
		assert rq.startswith('/')
		if verbose:
			print "Expecting", rq
		fd, path = self.next()
		self.assertEquals(rq, path)
		self.put_file(rq, contents, mtime)
		os.close(fd)

	def send_reject(self, rq):
		assert rq.startswith('/')
		fd, path = self.next()
		self.assertEquals(rq, path)
		if verbose:
			print "Rejecting", rq
		os.close(fd)

	def clientDownload(self):
		assert not os.path.exists(cache + '/...')
		self.assert_ls(fs + '/', ['.lazyfs-cache', '.lazyfs-helper', 'hello'])
		self.assertEquals('Hello', file(fs + '/hello').read())
		self.assertEquals('Hello', file(fs + '/hello').read())
		os.unlink(cache + '/hello')
		self.assertEquals('World', file(fs + '/hello').read())
		self.put_dir('/', ['f 6 3 hello'])
		self.assertEquals('Worlds', file(fs + '/hello').read())
	
	def serverDownload(self):
		self.send_dir('/', ['f 5 3 hello'])
		self.send_file('/hello', 'Hello', 3)
		self.send_file('/hello', 'World', 3)
		self.send_file('/hello', 'Worlds', 3)

	test06Download = cstest('Download')

	def clientDynamic(self):
		assert not os.path.exists(cache + '/...')
		self.assert_ls(fs + '/', ['.lazyfs-cache', '.lazyfs-helper'])
		assert os.path.isdir(fs + '/dir')
		self.assert_ls(fs + '/dir', ['hello'])
		assert os.path.isdir(cache + '/dir')
		os.spawnlp(os.P_WAIT, 'rm', 'rm', '-r', '--', cache + '/dir')
		assert not os.path.isdir(cache + '/dir')
		assert not os.path.isdir(fs + '/dir')
		assert not os.path.isdir(fs + '/foo')
	
	def serverDynamic(self):
		self.send_dir('/')
		os.mkdir(cache + '/dir')
		self.send_dir('/dir', ['f 5 3 hello'])
		self.send_reject('/dir')
		self.send_reject('/dir')
		self.send_reject('/foo')
		
	test07Dynamic = cstest('Dynamic')

	def clientMMap1(self):
		# File is changed in-place
		fd = os.open(fs + '/hello', os.O_RDONLY)
		m = mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
		os.close(fd)
		self.assertEquals('Hello', m[:])
		self.put_dir('/', ['f 6 3 hello'])
		file(fs + '/hello').read()	# Force update
		self.assertEquals('Worlds', file(cache + '/hello').read())
		self.assertEquals('World', m[:])
	
	def serverMMap1(self):
		self.send_dir('/', ['f 5 3 hello'])
		self.send_file('/hello', 'Hello', 3)
		self.send_file('/hello', 'Worlds', 3)

	test08MMap1 = cstest('MMap1')

	def clientMMap2(self):
		# A new file is created
		fd = os.open(fs + '/hello', os.O_RDONLY)
		m = mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
		os.close(fd)
		self.assertEquals('Hello', m[:])
		self.put_dir('/', ['f 6 3 hello'])
		os.unlink(cache + '/hello')
		self.assertEquals('Worlds', file(fs + '/hello').read())
		self.assertEquals('Hello', m[:])

		fd = os.open(fs + '/hello', os.O_RDONLY)
		m2 = mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
		self.assertEquals('Hello', m[:])
		self.assertEquals('World', m2[:])
		os.close(fd)
	
	serverMMap2 = serverMMap1
	
	test09MMap2 = cstest('MMap2')

	def clientMMap3(self):
		# A new file is created in the cache, but the lazyfs
		# inode remains the same.
		# We can't support this, because Linux mappings are
		# per-inode, not per-open-file, however we make sure the kernel
		# handles it gracefully (newer 2.6 kernels may provide a way to
		# fix the underlying problem, but it's not a big issue).
		fd = os.open(fs + '/hello', os.O_RDONLY)
		m = mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
		os.close(fd)
		self.assertEquals('Hello', m[:])
		os.unlink(cache + '/hello')
		self.assertEquals('World', file(fs + '/hello').read())
		self.assertEquals('Hello', m[:])

		fd = os.open(fs + '/hello', os.O_RDONLY)
		try:
			mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
			assert False
		except (IOError, EnvironmentError):
			# (I think EnvironmentError is a Python bug; it should
			# report the -EBUSY from mmap, but it actually reports
			# the error from the following msync)
			pass
		self.assertEquals('Hello', m[:])
		os.close(fd)
	
	def serverMMap3(self):
		self.send_dir('/', ['f 5 3 hello'])
		self.send_file('/hello', 'Hello', 3)
		self.send_file('/hello', 'World', 3)
	
	test10MMap3 = cstest('MMap3')

	def clientMultiple(self):
		# Opening a file again before the file is cached from the
		# first request doesn't send another request.
		# Hello isn't cached until after bob has been requested.
		f1 = file(fs + '/hello')
		f2 = file(fs + '/hello')
		f3 = file(fs + '/bob')
		self.assertEquals('Bob', f3.read())
		self.assertEquals('Hello', f1.read())
		self.assertEquals('Hello', f2.read())
	
	def serverMultiple(self):
		self.send_dir('/', ['f 5 3 hello',
				    'f 3 3 bob'])
		fd1, path = self.next()
		self.assertEquals('/hello', path)
		fd2, path = self.next()
		self.assertEquals('/bob', path)
		self.put_file('/bob', 'Bob', 3)
		os.close(fd2)
		self.put_file('/hello', 'Hello', 3)
		os.close(fd1)
	
	test11Multiple = cstest('Multiple')

	def clientMultiUser(self):
		# Like clientMultiple, but we do get multiple requests
		# because we have different users. We can cancel requests
		# individually.
		os.chdir(fs)
		f1 = os.open('hello', os.O_RDONLY)
		f2 = os.popen("sudo '%s' --read-hello" % prog_name)
		self.assertEquals('IOError\n', f2.read())
		self.assertEquals('Bob', file('bob').read())
		self.assertEquals('Hello', os.read(f1, 100))
	
	def serverMultiUser(self):
		self.send_dir('/', ['f 5 3 hello',
				    'f 3 3 bob'])
		fd1, path = self.next()
		self.assertEquals('/hello', path)
		fd2, path = self.next()
		self.assertEquals('/hello', path)
		os.close(fd2)
		self.send_file('/bob', 'Bob', 3)
		self.put_file('/hello', 'Hello', 3)
		os.close(fd1)
	
	test12MultiUser = cstest('MultiUser')

	def clientParallelReads(self):
		# Try to read from an uncached file from two processes
		# This causes a ref-leak in lazyfs 0.1.22 and earlier, but
		# we only trigger it, we don't report it. You have to enable
		# ref counting debugging in the kernel module to see it.
		fd = os.open(fs + '/hello', os.O_RDONLY)
		def child_mmap():
			child = os.fork()
			if child == 0:
				try:
					m = mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
					os.close(fd)
					assert m[0] == 'H'
					os._exit(0)
				except:
					os._exit(1)
		child_mmap()
		# Blocks until the subprocess mmaps the file
		self.assertEquals('Bob', file(fs + '/bob').read())

		child_mmap()
		# Blocks until the subprocess mmaps the file
		self.assertEquals('Fred', file(fs + '/fred').read())

		m = mmap.mmap(fd, 5, mmap.MAP_SHARED, mmap.PROT_READ)
		os.close(fd)
		assert m[0] == 'H'
	
	def serverParallelReads(self):
		self.send_dir('/', ['f 5 3 hello',
				    'f 3 3 bob',
				    'f 4 3 fred'])
		fd, path = self.next()
		self.assertEquals('/hello', path)
		self.send_file('/bob', 'Bob', 3)
		self.send_file('/fred', 'Fred', 3)
		self.put_file('/hello', 'Hello', 3)
		os.close(fd)

	test13ParallelReads = cstest('ParallelReads')

# Run the tests
sys.argv.append('-v')
unittest.main()

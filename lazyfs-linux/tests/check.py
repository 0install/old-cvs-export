#!/usr/bin/env python
import unittest
import os, signal, time
import traceback

fs = '/test/0install'
cache = '/test/cache'

os.system("sudo umount " + fs)

# Note: this test suite is likely to crash older versions!!
os.system("sudo rmmod lazyfs0d1d22")

assert not os.path.ismount(fs)

class LazyFS(unittest.TestCase):
	def setUp(self):
		for item in os.listdir(cache):
			os.system("rm -r '%s'" % os.path.join(cache, item))
		os.system("sudo mount -t lazyfs0d1d21 lazyfs %s -o %s" % (fs, cache))

	def tearDown(self):
		os.system("sudo umount /test/0install")
	
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
					if status:
						error = Exception('Error in child')
					break
				time.sleep(0.1)
			else:
				print "Killing", child
				os.kill(child, signal.SIGTERM)
				os.waitpid(child, 0)
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

class TestWithoutHelper(LazyFS):
	def testCacheLink(self):
		assert os.path.islink(fs + '/.lazyfs-cache')
		assert os.readlink(fs + '/.lazyfs-cache') == cache
	
	def testUnconnectedEmpty(self):
		try:
			file(fs + '/f')
			assert 0
		except IOError:
			pass

class TestWithHelper(WithHelper):
	def read_rq(self):
		#print "Reading request..."
		fd = os.read(self.c, 1000)
		return int(fd.split(' ', 1)[0])
	
	def next(self):
		fd = self.read_rq()
		path = os.read(fd, 1000)
		assert path[-1] == '\0'
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
	testANothing = cstest('Nothing')
	
	def clientReleaseHelper(self):
		os.open(fs + '/.lazyfs-helper', os.O_RDONLY)
	
	def serverReleaseHelper(self):
		os.close(self.c)
		self.c = None

	testReleaseHelper = cstest('ReleaseHelper')
	
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

	testDoubleOpen = cstest('DoubleOpen')

	def clientLsRoot(self):
		self.assert_ls(fs, ['.lazyfs-helper', '.lazyfs-cache'])
	def serverLsRoot(self):
		self.send_dir('/', [])

	testLsRoot = cstest('LsRoot')

	def clientGetROX(self):
		self.assert_ls(fs + '/rox', ['apps'])
	
	def serverGetROX(self):
		fd, path = self.next()
		self.assertEquals('/', path)
		f = file(cache + '/...', 'w').write('LazyFS\nd 1 1 rox\0')
		os.close(fd)

		fd, path = self.next()
		self.assertEquals('/rox', path)
		os.mkdir(cache + '/rox')
		f = file(cache + '/rox/...', 'w').write('LazyFS\nd 1 1 apps\0')
		os.close(fd)
	
	testGetROX = cstest('GetROX')

	def put_dir(self, dir, contents):
		f = file(cache + dir + '/....', 'w')
		if contents:
			f.write('LazyFS\n' + '\0'.join(contents) + '\0')
		else:
			f.write('LazyFS\n')
		f.close()
		os.rename(cache + dir + '/....', cache + dir + '/...')

	def send_dir(self, dir, contents):
		assert dir.startswith('/')
		fd, path = self.next()
		#print "Got request for ", dir
		self.assertEquals(dir, path)
		self.put_dir(dir, contents)
		os.close(fd)
	
	def send_file(self, rq, contents, mtime):
		assert rq.startswith('/')
		#print "Expecting", rq
		fd, path = self.next()
		self.assertEquals(rq, path)
		f = file(cache + rq, 'w')
		f.write(contents)
		f.close()
		os.utime(cache + rq, (mtime, mtime))
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

	testDownload = cstest('Download')
	
if __name__ == '__main__':
	unittest.main()

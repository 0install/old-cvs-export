from multitest import MultiTest
import sys, os, shutil
from config import log, fs, cache, site, www, version

class LazyFSTest(MultiTest):
	"""Base class that mounts an empty lazyfs filesystem for each test."""

	def setUp(self):
		assert not os.path.ismount(fs)
		for x in (fs, cache, site, www):
			if os.path.isdir(x):
				shutil.rmtree(x)
			os.mkdir(x)
		uid = os.geteuid()
		cache_uid = os.stat(cache).st_uid 
		if cache_uid != uid:
			raise Exception('%s must be owned by user %s (not %s)' %
					(cache, uid, cache_uid))
		os.system("sudo mount -t lazyfs%s lazyfs %s -o %s" %
				(version, fs, cache))

	def tearDown(self):
		os.system("sudo umount " + fs)
	
	def assertLs(self, ls, path):
		#actual = os.listdir(path) (blocks all threads!!)
		actual = os.popen("ls -a '%s' 2>&%d" % (path, log.fileno())).read().split('\n')
		try:
			actual.remove('')
			actual.remove('.')
			actual.remove('..')
		except:
			raise Exception('ls failed for: ' + path)
		actual.sort()
		ls = ls[:]
		ls.sort()
		self.assertEquals(ls, actual)

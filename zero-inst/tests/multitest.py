import unittest
import threading
import traceback
import sys, os
import codecs

verbose = False

current_test = None
main_thread = threading.currentThread()
buffer = {}		# Thread -> str

step = threading.Event()

current_actors = []

sync_condition = threading.Condition()
syncing = 0

def to_utf8(s): return codecs.utf_8_encode(s)[0]

class ThreadOut:
	def __init__(self, stream):
		self.stream = stream

	def write(self, s):
		current = threading.currentThread()
		s = buffer.get(current, '') + to_utf8(s)

		if current is main_thread:
			prefix = 'Main: '
		else:
			prefix = current.actor.name + ': '

		lines = s.split('\n')
		for line in lines[:-1]:
			self.stream.write(prefix + line + '\n')

		buffer[current] = lines[-1]

class Actor:
	thread = None

	def __init__(self, name):
		self.name = name

	def __call__(self):
		if verbose: print "Called Actor", self
		assert current_test

		if not self.thread:
			raise Exception('%s not active (add to actors)' % self)

		current = threading.currentThread()
		assert current is not main_thread
		return current == self.thread
	
	def create_thread(self):
		assert self.thread is None
		self.thread = ActorThread(self)
		current_actors.append(self)

	def start_thread(self):
		assert self.thread is not None
		self.failure = None
		self.thread.start()
	
	def join_thread(self):
		assert self.thread is not None
		if verbose: print "Joining", self.thread
		while self.thread.isAlive():
			self.thread.join(1)
			if self.thread.isAlive():
				print "(again)"
		if verbose: print "Joined", self.thread
		self.thread = None
		current_actors.remove(self)
	
	def __str__(self):
		return "<%s>" % self.name
	
class ActorThread(threading.Thread):
	def __init__(self, actor):
		self.actor = actor
		threading.Thread.__init__(self, name = actor.name)
	
	def run(self):
		try:
			try:
				current_test()
			except Exception, ex:
				print "ERROR:"
				traceback.print_exc()
				self.actor.failure = ex
		finally:
			self.sync()
	
	def sync(self):
		"""Wait for all threads to call sync(), then
		continue all threads at once."""
		if verbose: print "Syncing"
		global syncing
		sync_condition.acquire()
		syncing += 1
		if syncing < len(current_actors):
			if verbose: print "Waiting for others"
			sync_condition.wait()
			if verbose: print "Resuming"
		else:
			if verbose: print "Waking others"
			syncing = 0
			sync_condition.notifyAll()
			if verbose: print "Continuing"
		sync_condition.release()


class MultiTestMeta(type):
	def __init__(self, name, bases, unused_dict):
		for x in self.__dict__.keys():
			if x.startswith('test'):
				setattr(self, x,
					threaded_test(getattr(self, x)))

class MultiTest(unittest.TestCase):
	__metaclass__ = MultiTestMeta

	def __init__(self, methodName = 'runTest'):
		unittest.TestCase.__init__(self, methodName)

	def sync(self):
		current = threading.currentThread()
		current.sync()

def threaded_test(fn):
	def run(self):
		global verbose
		old_verbose = verbose	# Test cases may set verbose

		print "\nRunning", fn
		assert syncing == 0
		global current_test
		current_test = lambda: fn(self)
		step_number = 0
		try:
			for a in self.actors:
				a.create_thread()
			for a in self.actors:
				if verbose: print "Start", a
				a.start_thread()
		finally:
			failed = []
			try:
				for a in self.actors:
					a.join_thread()
					if a.failure:
						failed.append('%s: %s' %
							(a, a.failure))
			except KeyboardInterrupt:
				import signal
				os.kill(os.getpid(), signal.SIGTERM)
			if verbose: print "Joined all"
			verbose = old_verbose
			if failed:
				raise Exception('Failures from:\n' +
					'\n'.join(failed))
	return run

#!/usr/bin/env python

import traceback
import os, socket
import time

def unmount():
	if os.path.exists('/uri/.lazyfs-helper'):
		print "Unmounting..."
		os.system('sudo umount /uri')
		if os.path.exists('/uri/.lazyfs-helper'):
			raise Exception("Can't unmount /uri!")
		lines = os.popen('dmesg').readlines()
		lines.reverse()
		i = lines.index("lazyfs: Resource usage after put_super:\n")
		lines.reverse()
		print ">> dmesg output"
		print ''.join(lines[-i:])

unmount()
os.system('rm -rf /var/cache/zero-inst/0test')
os.system('sudo mount /uri')

http = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

for port in range(3000, 3100):
	try:
		http.bind(('localhost', port))
	except socket.error:
		continue
	break
else:
	raise Exception('No available ports!')
http.listen(5)

os.environ['http_proxy'] = 'http://localhost:%d' % port

zero = os.spawnlp(os.P_NOWAIT, './zero-install', './zero-install')

class Request:
	def __init__(self, want_uri):
		#print "waiting..."
		(self.c, addr) = http.accept()
		#print "Got connection", self.c
		data = self.c.recv(1024)
		uri = data.split('\n', 1)[0].split()[1]
		print "[s] Zero-Install requested", uri
		assert uri == want_uri

	def reply(self, message):
		self.c.send('HTTP/1.0 200 OK\n\n' + message)
		self.close()
	
	def close(self):
		self.c.close()

def test():
	if not server:
		# Helper might not have started yet...
		time.sleep(0.2)
	import test_cases

child = os.fork()
if child == 0:
	# Child
	try:
		server = False
		test()
		print "All tests passed"
		os._exit(0)
	except:
		traceback.print_exc()
		os._exit(1)
else:
	# Parent
	try:
		server = True
		test()
	finally:
		print "Waiting for test child to finish"
		http.close()
		(pid, status) = os.waitpid(child, 0)
		import signal
		os.kill(zero, signal.SIGINT)
		print "Waiting for zero-install to finish"
		os.waitpid(zero, 0)
		print
		if status:
			print "Error from child (code %d)" % status
		else:
			print "All tests passed!"

unmount()

#!/usr/bin/env python

import traceback
import os, socket

os.system('rm -rf /var/cache/zero-inst/http/0test')

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
print "Using port", port

os.environ['http_proxy'] = 'http://localhost:%d' % port

zero = os.spawnlp(os.P_NOWAIT, './zero-install', './zero-install')

class Request:
	def __init__(self, want_uri):
		print "waiting..."
		(self.c, addr) = http.accept()
		print "Got connection", self.c
		data = self.c.recv(1024)
		uri = data.split('\n', 1)[0].split()[1]
		print "Get", uri
		assert uri == want_uri

	def reply(self, message):
		self.c.send('HTTP/1.0 200 OK\n\n' + message)
		self.close()
	
	def close(self):
		self.c.close()

def test(server):
	print "started", server
	if server:
		r = Request('http://0test/.0inst-index.xml')
		r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net"><dir size="1" mtime="2"/></site-index>')
	else:
		assert os.listdir('/uri/http/0test') == []
	print "finished", server

child = os.fork()
if child == 0:
	# Child
	try:
		test(0)
		print "All tests passed"
	except:
		traceback.print_exc()
		os._exit(1)
else:
	# Parent
	try:
		test(1)
	finally:
		print "Waiting for test child to finish"
		http.close()
		(pid, status) = os.waitpid(child, 0)
		import signal
		os.kill(zero, signal.SIGINT)
		print "Waiting for zero-install to finish"
		os.waitpid(zero, 0)
		if status:
			print "Error from child (code %d)" % status
		else:
			print "All tests passed!"

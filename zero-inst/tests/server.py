"""This module implements a very simple web-proxy. By setting the http_proxy
environment variable, we can get the zero-install daemon process to contact us
when it needs to download anything, rather than the real server."""

import socket, os, codecs
from multitest import Actor
from config import www
from os.path import join

port = 9143	# Any free port will do

_to_utf8 = codecs.getencoder('utf-8')

def unescape(uri):
	"Convert each %20 to a space, etc."
	if '%' not in uri: return uri
	import re
	utf8 = re.sub('%[0-9a-fA-F][0-9a-fA-F]',
		lambda match: chr(int(match.group(0)[1:], 16)),
		uri)
	return codecs.utf_8_decode(utf8)[0]

def escape(uri):
	"Convert each space to %20, etc"
	import re
	return re.sub('[^-_./a-zA-Z0-9:]',
		lambda match: '%%%02X' % ord(match.group(0)),
		_to_utf8(uri)[0])

class Webserver(Actor):
	def __init__(self):
		Actor.__init__(self, 'Web server')
		self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		# Allow the port to be reused right away.
		self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
		self.socket.bind(('', port))
		self.socket.listen(5)

		os.environ['http_proxy'] = 'http://127.0.0.1:%d' % port
	
	def reject(self, site):
		site = site.replace('#', '/', 1)
		c = self.accept('http://' + site + '/.0inst-index.tar.bz2')
		c.close()
		print "Closed"
	
	def accept_path(self):
		print "Waiting for request"
		s, addr = self.socket.accept()
		
		c = s.makefile()
		s.close()
		rq = c.readline().strip()
		for line in c:
			line = line.strip()
			if not line: break
		start = 'GET '
		end = ' HTTP/1.0'
		assert rq.startswith(start)
		assert rq.endswith(end)
		rq = rq[len(start):-len(end)]
		print "Got request!"
		print "Request is for", rq
		return c, rq
	
	def accept(self, url):
		c, rq = self.accept_path()
		assert unescape(rq) == url, \
			'Bad request %s (wanted %s)\n(raw: %s)' % (`unescape(rq)`, `url`, rq)

		return c

	def handle_index(self, site):
		c = self.accept('http://' + site + '/.0inst-index.tar.bz2')
		c.write('HTTP/1.1 200 OK\r\n')
		c.write('\r\n')
		print "Reading..."
		c.write(file(join(www, '.0inst-index.tar.bz2')).read())
		c.close()
		print "Done"
	
	def handle_any(self, site):
		print "Waiting for request"
		s, addr = self.socket.accept()
		
		c = s.makefile()
		s.close()
		rq = c.readline().strip()
		start = 'GET http://'
		end = ' HTTP/1.0'
		assert rq.startswith(start)
		assert rq.endswith(end)
		rq_site, path = rq[len(start):-len(end)].split('/', 1)

		assert unescape(rq_site) == site

		print "Got request for", path
		for line in c:
			line = line.strip()
			if not line: break

		c.write('HTTP/1.1 200 OK\r\n\r\n')
		c.write(file(join(www, path)).read())
		c.close()

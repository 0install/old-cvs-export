#!/usr/bin/env python

import os
import urllib2
import traceback
from xml import sax
from xml.sax import ContentHandler

cache = '/var/cache/zero-inst/'
info_xml = '.0inst.xml'

class ListingHandler(ContentHandler):
	def __init__(self):
		self.dir = []

	def startElement(self, element, attrs):
		print "Start", `element`, `attrs`
		self.attrs = attrs
		self.data = ''
	
	def characters(self, content):
		self.data += content
	
	def endElement(self, element):
		c = {'file':'f', 'dir': 'd',
		     'exec':'x', 'link': 's'}.get(element, None)
		if c:
			self.dir.append('%s %d %d %s' %
				(c, int(self.attrs['size']),
				int(self.attrs['mtime']), self.data))

def write_dir(dir, entries):
	if dir == '/':
		dir = cache
	else:
		dir = os.path.join(cache, dir[1:])
	if not os.path.isdir(dir):
		os.makedirs(dir)
	tmp_path = os.path.join(dir, '....')
	print "Write", tmp_path
	tmp = file(tmp_path, 'w')
	if entries is None:
		tmp.write('LazyFS Dynamic\n')
	else:
		tmp.write('LazyFS\n' + ''.join([e + '\0' for e in entries]))
	tmp.close()
	os.rename(tmp_path, tmp_path[:-1])

def check_host(host):
	print "Check host", host
	if host == 'AppRun' or host.startswith('.'):
		return	# Don't annoy DNS with stupid queries
	import socket
	try:
		socket.gethostbyname(host)
	except:
		print "No such host as", host
		return
	handler = ListingHandler()
	sax.parse(urllib2.urlopen('http://%s/%s' % (host, info_xml)), handler)
	write_dir('/http/%s/' % host, handler.dir)

def check_res(path, host, resource):
	leaf = os.path.basename(resource)
	ddd = os.path.join(cache, 'http', host, os.path.dirname(resource),
				'...')
	link = 0
	f = file(ddd)
	assert f.read(7) == 'LazyFS\n'
	for line in f.read().split('\0'):
		if link:
			link = 0
			continue
		if line[0] == 'l':
			link = 1
		print "got", line
		if line.endswith(' ' + leaf):
			break
	else:
		raise Exception('Nothing known about %s!' % resource)
	print "cached", ddd
	print line
	if line[0] == 'd':
		print "write", path
		handler = ListingHandler()
		uri = 'http://%s/%s/%s' % (host, resource, info_xml)
		print "Fetch", uri
		sax.parse(urllib2.urlopen(uri), handler)
		write_dir(path, handler.dir)
	elif line[0] == 'f':
		uri = 'http://%s/%s' % (host, resource)
		import shutil
		shutil.copyfileobj(urllib2.urlopen(uri),
			    file(os.path.join(cache, path[1:]), 'w'))
	else:
		raise Exception("Shouldn't be asked for '%s'" % path)

def handle(path):
	if path == '/':
		write_dir(path, ['d 0 0 http'])
	elif path == '/http':
		write_dir(path, None)
	elif path.startswith('/http/'):
		rest = path[6:].split('/', 1)
		if len(rest) == 1:
			check_host(rest[0])
		else:
			check_res(path, rest[0], rest[1])
	else:
		print "'%s'" % path

commands = os.open('/uri/.lazyfs-helper', os.O_RDONLY)

while 1:
	request = int(os.read(commands, 40)[:-1])
	path = os.read(request, 4096)[:-1]
	print "Handle", path
	try:
		handle(path)
	except:
		traceback.print_exc()
		print "(while handling %s)" % path
	os.close(request)

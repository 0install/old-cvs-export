#!/usr/bin/env python

import os
import urllib2, urlparse
import traceback
from xml import sax
from xml.sax import ContentHandler
import shutil

cache = '/var/cache/zero-inst/'
info_xml = '.0inst.xml'

class Done(Exception): pass

class ArchiveFinder(ContentHandler):
	def __init__(self, target):
		self.target = target
		self.contents = {}
		self.current_archive = None
		self.getting_leaf = False
	
	def startElement(self, element, attrs):
		if element in ('group', 'directory'):
			self.current_archive = None
			self.found = False
		self.data = ''
	
	def endElement(self, element):
		if element in ('group', 'directory'):
			print "Leaving group", self.found, self.current_archive
			if self.target in self.contents:
				raise Done
			self.current_archive = None
			self.contents = {}
		if element in ('exec', 'file', 'dir', 'link'):
			print "Got", self.data
			self.contents[self.data] = element
		if element == 'archive':
			self.current_archive = self.data
	
	def characters(self, content):
		self.data += content

class ListingHandler(ContentHandler):
	def __init__(self):
		self.dir = []

	def startElement(self, element, attrs):
		#print "Start", `element`, `attrs`
		self.attrs = attrs
		self.data = ''
	
	def characters(self, content):
		self.data += content
	
	def endElement(self, element):
		c = {'file':'f', 'dir': 'd',
		     'exec':'x', 'link': 'l'}.get(element, None)
		if c:
			self.dir.append('%s %d %d %s' %
				(c, int(self.attrs['size']),
				int(self.attrs['mtime']), self.data))
		if c == 'l':
			self.dir[-1] += '\0%s' % self.attrs['target']

def write_dir(host, resource):
	resource = '/' + resource
	dir = os.path.join(cache, 'http', host) + resource
	uri = 'http://%s%s/%s' % (host, resource, info_xml)
	print "Fetch", uri
	index = urllib2.urlopen(uri).read()

	if not os.path.isdir(dir):
		os.makedirs(dir)
	cached_index = os.path.join(dir, info_xml)
	file(cached_index, 'w').write(index)

	handler = ListingHandler()
	sax.parseString(index, handler)

	write_ddd(dir, 'LazyFS\n' + ''.join([e + '\0' for e in handler.dir]))

def write_ddd(dir, contents):
	tmp_path = os.path.join(dir, '....')
	print "Write", tmp_path
	file(tmp_path, 'w').write(contents)
	os.rename(tmp_path, tmp_path[:-1])

def check_host(host):
	print "Check host", host
	if host == 'AppRun' or host.startswith('.'):
		return	# Don't annoy DNS with stupid queries
	write_dir(host, '')

def extract_archive(stream):
	tmp_dir = os.path.join(cache, 'tmp')
	if not os.path.exists(tmp_dir):
		os.makedirs(tmp_dir)
	tgz = os.path.join(tmp_dir, 'archive.tgz')
	shutil.copyfileobj(stream, file(tgz, 'w'))
	stream.close()
	print "Extract to ", tmp_dir
	os.spawnvp(os.P_WAIT, 'tar', ['tar', 'xzvf', tgz, '-C', tmp_dir, '--'])

	print "Done"
				  
	return tmp_dir

def check_res(path, host, resource):
	leaf = os.path.basename(resource)
	index = os.path.join(cache, 'http', host,
				os.path.dirname(resource), info_xml)
	if not os.path.exists(index):
		write_dir(host, os.path.dirname(resource))
	handler = ArchiveFinder(leaf)
	try:
		sax.parse(file(index), handler)
	except Done:
		pass
	else:
		raise Exception("'%s' not found!" % leaf)
	if handler.contents[leaf] in ('file', 'exec'):
		uri = urlparse.urljoin('http://%s/%s' % (host, resource),
					handler.current_archive)
		print "Get '%s' from '%s'\n" % (leaf, uri)
		archive = urllib2.urlopen(uri)
		tmp_dir = extract_archive(archive)
		print "Got archive", uri, "in", tmp_dir
		dir = os.path.join(cache, 'http', host, os.path.dirname(resource))
		for item in handler.contents:
			print "Pulling out", item
			print os.path.join(tmp_dir, item)
			os.rename(os.path.join(tmp_dir, item),
				  os.path.join(dir, item))
	else:
		assert handler.contents[leaf] == 'dir'
		print "It's a directory..."
		write_dir(host, resource)

def handle(path):
	if path == '/':
		write_dir(path, ['d 0 0 http'])
	elif path == '/http':
		write_ddd(os.path.join(cache, 'http'), 'LazyFS Dynamic\n')
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

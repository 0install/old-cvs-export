#!/usr/bin/env python

# This file is imported twice; once in each of the two test processes.
# In one, server is True; in the other, False.
# The file is a series of tests, each having two parts; the server part
# and the client part:
#
# if server:
#	operations_for_server()
# else:
#	operations_for_client()
#
# The two branches of the if execute in parallel in the two processes.

from __main__ import server, Request
import os

# Missing path=
if server:
	r = Request('http://0test/.0inst-index.tgz')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net"><dir size="1" mtime="2"/></site-index>')
	r = Request('http://0test/.0inst-index.tgz')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net"><dir size="1" mtime="2"/></site-index>')
else:
	try:
		os.chdir('/uri/0http/0test')
		assert False
	except OSError:
		pass
	try:
		os.listdir('/uri/0http/0test')
		assert False
	except OSError:
		pass

# Empty root
if server:
	r = Request('http://0test/.0inst-index.tgz')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net" path="/uri/0install/0test"><dir size="1" mtime="2"/></site-index>')
else:
	assert os.listdir('/uri/0http/0test') == []

# Single symlink
if server:
	r = Request('http://0test/.0inst-index.tgz')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net" path="/uri/0http/0test"><dir size="1" mtime="2"><link size="3" mtime="2" target="end" name="link"/></dir></site-index>')
else:
	assert os.popen('cd /uri/0http/0test; 0refresh').read() == 'OK\n'
	assert os.listdir('/uri/0http/0test') == ['link']
	assert os.readlink('/uri/0http/0test/link') == 'end'

# Update to invalid index
if server:
	r = Request('http://0test/.0inst-index.tgz')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net" path="/uri/0http/0test"><dir size="1" mtime="2"><link size="3" mtime="2" target="end"/></dir></site-index>')
else:
	assert os.popen('cd /uri/0http/0test; 0refresh').read() == 'FAIL\n'
	assert os.listdir('/uri/0http/0test') == ['link']

print "finished", server

# Update back to empty
if server:
	r = Request('http://0test/.0inst-index.tgz')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net" path="/uri/0http/0test"><dir size="1" mtime="2"></dir></site-index>')
else:
	assert os.popen('cd /uri/0http/0test; 0refresh').read() == 'OK\n'
	assert os.listdir('/uri/0http/0test') == []

print "finished", server

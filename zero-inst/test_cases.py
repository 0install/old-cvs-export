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

barrier_count = 0
def barrier():
	global barrier_count
	barrier_count += 1
	if server:
		accept_barrier(barrier_count)
		print "Step", barrier_count, "done"
	else:
		os.popen('0refresh 0test.%d' % barrier_count)

def check_ls(dir, wanted):
	got = os.listdir(dir)
	if got != wanted:
		print "Expected", wanted
		print "Got     ", got
		assert got == wanted

import os
from __main__ import send_tree, server, accept_barrier
import stat

def refresh():
	assert os.popen('0refresh 0test').read() == 'OK\n'

# Missing path
if server:
	send_tree('<dir size="1" mtime="2"/>', path = None)
	send_tree('<dir size="1" mtime="2"/>', path = None)
else:
	try:
		os.chdir('/uri/0install/0test')
		assert False
	except OSError:
		pass
	try:
		os.listdir('/uri/0install/0test')
		assert False
	except OSError:
		pass

barrier()

# Empty root
if server:
	send_tree('<dir size="1" mtime="2"/>')
else:
	check_ls('/uri/0install/0test', [])

# Single symlink
if server:
	send_tree('<dir size="1" mtime="2"><link size="3" mtime="2" target="end" name="link"/></dir>')
else:
	assert os.popen('cd /uri/0install/0test; 0refresh').read() == 'OK\n'
	check_ls('/uri/0install/0test', ['link'])
	assert os.readlink('/uri/0install/0test/link') == 'end'

barrier()

# Update to invalid index
if server:
	send_tree('<dir size="1" mtime="2"><link size="3" mtime="2" target="end"/></dir>')
else:
	assert os.popen('cd /uri/0install/0test; 0refresh').read() != 'FAIL\n'
	check_ls('/uri/0install/0test', ['link'])

barrier()

# Update back to empty
if server:
	send_tree('<dir size="1" mtime="2"></dir>')
else:
	assert os.popen('cd /uri/0install/0test; 0refresh').read() == 'OK\n'
	check_ls('/uri/0install/0test', [])

barrier()

# Try to trigger a bug in our d_genocide. If you were in directory A,
# containing a file B, and an ancestor of A got removed from the tree,
# then the count on B went down to zero, but it remained hashed.
if server:
	send_tree('<dir size="1" mtime="2"><dir name="apps" size="0" mtime="1"><dir name="ZeroProgress" size="4" mtime="3"><link name="foo" size="1" mtime="2" target="bar"/></dir></dir></dir>')
	send_tree('<dir size="3" mtime="2"><dir name="apps" size="1" mtime="2"/></dir>')
else:
	assert os.popen('0refresh 0test').read() == 'OK\n'
	#os.system('ls -R /uri/0install/0test')
	check_ls("/uri/0install/0test", ['apps'])
	check_ls("/uri/0install/0test/apps", ["ZeroProgress"])
	check_ls("/uri/0install/0test/apps/ZeroProgress", ['foo'])
	os.chdir('/uri/0install/0test/apps/ZeroProgress')
	check_ls('.', ['foo'])
	check_ls("/uri/0install/0test", ['apps'])
	assert os.popen('0refresh').read() == 'OK\n'
	check_ls("/uri/0install/0test", ['apps'])
	os.system("pwd")
	check_ls('..', [])
	#print os.listdir('.')
	check_ls('.', [])
	#file('foo') (forces creation of a negative dentry)
	os.chdir('/')

barrier()

# Now make sure the kernel module notices that it's cache isn't uptodate...
if server:
	send_tree('<dir size="3" mtime="2"><link name="link" target="one" size="2" mtime="3"/></dir>')
	send_tree('<dir size="3" mtime="2"><link name="link" target="two" size="2" mtime="3"/></dir>')
else:
	refresh()
	check_ls('/uri/0install/0test', ['link'])
	assert os.readlink('/uri/0install/0test/link') == 'one'
	refresh()
	assert os.readlink('/uri/0install/0test/link') == 'two'

barrier()

if server:
	send_tree('<dir size="3" mtime="2"><dir name="a" size="2" mtime="3"><dir name="b" size="2" mtime="3"><dir name="c" size="2" mtime="3"><link target="end" name="link" size="2" mtime="3"/></dir></dir></dir></dir>')
else:
	refresh()
	assert os.readlink('/uri/0install/0test/a/b/c/link') == 'end'
	#os.chdir('/uri/0install/0test/a/b/c')
	a = open('/uri/0install/0test/a/b')
	os.system("rm -r /var/cache/zero-inst/0test/a")
	assert os.readlink('/uri/0install/0test/a/b/c/link') == 'end'
	assert os.path.exists('/var/cache/zero-inst/0test/a/b/c')

barrier()

def inum(file): return os.lstat(file)[stat.ST_INO]
def size(file): return os.lstat(file)[stat.ST_SIZE]

# Check that we don't create new inodes without reason
if server:
	send_tree('<dir size="3" mtime="2"><dir name="a" size="2" mtime="3"><link name="b" size="2" mtime="3" target="one"/></dir></dir>')
	send_tree('<dir size="3" mtime="2"><dir name="a" size="3" mtime="3"><link name="b" size="2" mtime="3" target="one"/></dir></dir>')
	send_tree('<dir size="3" mtime="2"><link name="a" size="3" mtime="3" target="new"/></dir>')
else:
	refresh()
	ai = inum('/uri/0install/0test/a')
	bi = inum('/uri/0install/0test/a/b')
	assert size('/uri/0install/0test/a') == 2
	refresh()
	assert inum('/uri/0install/0test/a') == ai
	assert inum('/uri/0install/0test/a/b') == bi
	assert size('/uri/0install/0test/a') == 3
	refresh()
	assert inum('/uri/0install/0test/a') != ai

#print "finished", server

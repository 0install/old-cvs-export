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
		os.popen('0refresh test.%d' % barrier_count)

def check_ls(dir, wanted):
	got = os.listdir(dir)
	if got != wanted:
		print "Expected", wanted
		print "Got     ", got
		assert got == wanted

import os
from __main__ import send_tree, server, accept_barrier, fs, cache
import stat
from os.path import join

def refresh():
	assert os.popen('0refresh test').read() == 'OK\n'

# Missing path
if server:
	send_tree('<dir size="1" mtime="2"/>', path = None)
	send_tree('<dir size="1" mtime="2"/>', path = None)
else:
	try:
		os.chdir(join(fs, 'test'))
		assert False
	except OSError:
		pass
	try:
		os.listdir(join(fs, 'test'))
		assert False
	except OSError:
		pass

barrier()

# Empty root
if server:
	print "Sending tree"
	send_tree('<dir size="1" mtime="2"/>')
	print "Sent"
else:
	check_ls(join(fs, 'test'), [])

# Single symlink
if server:
	send_tree('<dir size="1" mtime="2"><link size="3" mtime="2" target="end" name="link"/></dir>')
else:
	assert os.popen('cd /uri/0install/test; 0refresh').read() == 'OK\n'
	check_ls(join(fs, 'test'), ['link'])
	assert os.readlink(join(fs, 'test/link')) == 'end'

barrier()

# Update to invalid index
if server:
	send_tree('<dir size="1" mtime="2"><link size="3" mtime="2" target="end"/></dir>')
else:
	assert os.popen('cd /uri/0install/test; 0refresh').read() != 'FAIL\n'
	check_ls(join(fs, 'test'), ['link'])

barrier()

# Update back to empty
if server:
	send_tree('<dir size="1" mtime="2"></dir>')
else:
	assert os.popen('cd /uri/0install/test; 0refresh').read() == 'OK\n'
	check_ls(join(fs, 'test'), [])

barrier()

# Try to trigger a bug in our d_genocide. If you were in directory A,
# containing a file B, and an ancestor of A got removed from the tree,
# then the count on B went down to zero, but it remained hashed.
if server:
	send_tree('<dir size="1" mtime="2"><dir name="apps" size="0" mtime="1"><dir name="ZeroProgress" size="4" mtime="3"><link name="foo" size="1" mtime="2" target="bar"/></dir></dir></dir>')
	send_tree('<dir size="3" mtime="2"><dir name="apps" size="1" mtime="2"/></dir>')
else:
	assert os.popen('0refresh test').read() == 'OK\n'
	#os.system('ls -R /uri/0install/test')
	check_ls(join(fs, "test"), ['apps'])
	check_ls(join(fs, "test/apps"), ["ZeroProgress"])
	check_ls(join(fs, "test/apps/ZeroProgress"), ['foo'])
	os.chdir(join(fs, 'test/apps/ZeroProgress'))
	check_ls('.', ['foo'])
	check_ls(join(fs, "test"), ['apps'])
	assert os.popen('0refresh').read() == 'OK\n'
	check_ls(join(fs, "test"), ['apps'])
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
	check_ls(join(fs, 'test'), ['link'])
	assert os.readlink(join(fs, 'test/link')) == 'one'
	refresh()
	assert os.readlink(join(fs, 'test/link')) == 'two'

barrier()

if server:
	send_tree('<dir size="3" mtime="2"><dir name="a" size="2" mtime="3"><dir name="b" size="2" mtime="3"><dir name="c" size="2" mtime="3"><link target="end" name="link" size="2" mtime="3"/></dir></dir></dir></dir>')
else:
	refresh()
	assert os.readlink(join(fs, 'test/a/b/c/link')) == 'end'
	#os.chdir(join(fs, 'test/a/b/c'))
	a = open(join(fs, 'test/a/b'))
	os.system("rm -r /var/cache/zero-inst/test/a")
	assert os.readlink(join(fs, 'test/a/b/c/link')) == 'end'
	assert os.path.exists('/var/cache/zero-inst/test/a/b/c')

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
	ai = inum(join(fs, 'test/a'))
	bi = inum(join(fs, 'test/a/b'))
	assert size(join(fs, 'test/a')) == 2
	refresh()
	assert inum(join(fs, 'test/a')) == ai
	assert inum(join(fs, 'test/a/b')) == bi
	assert size(join(fs, 'test/a')) == 3
	refresh()
	assert inum(join(fs, 'test/a')) != ai

#print "finished", server

#!/usr/bin/env python

from __main__ import server, Request
import os

if server:
	r = Request('http://0test/.0inst-index.xml')
	r.reply('<?xml version="1.0"?><site-index xmlns="http://zero-install.sourceforge.net" path="/uri/0install/0test"><dir size="1" mtime="2"/></site-index>')
else:
	assert os.listdir('/uri/http/0test') == []
print "finished", server

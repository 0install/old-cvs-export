#!/usr/bin/env python

"""Some unittests for the multitest unittesting module."""

import unittest
from multitest import Actor, MultiTest, threaded_test
import sys
from time import sleep

fred = Actor('Fred')
jim = Actor('Jim')

scratch = []

class TestMultiTest(MultiTest):
	actors = (fred, jim)

	def testEmpty(self):
		pass
	
	def setUp(self):
		global scratch
		scratch = []
	
	def testPara(self):
		if fred():
			scratch.append(1)
		if jim():
			sleep(0.1)
			scratch.append(3)
		if fred():
			scratch.append(2)

		self.sync()
		self.assertEquals([1,2,3], scratch)

	def testSync(self):
		if fred():
			scratch.append(1)
		if jim():
			sleep(0.1)
			scratch.append(2)
		self.sync()	# Extra sync
		if fred():
			scratch.append(3)

		self.sync()
		self.assertEquals([1,2,3], scratch)


# Run the tests
sys.argv.append('-v')
unittest.main()

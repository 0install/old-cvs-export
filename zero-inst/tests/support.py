import os, sys
from os.path import dirname, join, exists, realpath
from config import site, www

zero_build = join(realpath(dirname(sys.argv[0])), '0build')

def build(site_name):
	os.chdir(site)
	if os.spawnl(os.P_WAIT, zero_build, zero_build, '--quiet', www, site_name):
		raise Exception('Error from 0build')
	os.chdir('..')

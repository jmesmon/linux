#
# gdb helper commands and functions for Linux kernel debugging
#
#  loader module
#
# Copyright (c) Siemens AG, 2012
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import os

sys.path = [ os.path.dirname(__file__) + "/scripts/gdb" ] + sys.path

from utils import gdb_version

if gdb_version < "7.1":
	print "NOTE: gdb 7.1 or later required for Linux helper scripts " \
	      "to work."
else:
	import utils
	import symbols
	import module
	import dmesg
	import task
	import percpu

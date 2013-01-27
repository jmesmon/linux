#
# gdb helper commands and functions for Linux kernel debugging
#
#  common utilities
#
# Copyright (c) Siemens AG, 2011-2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
import re

gdb_version = re.sub("^[^0-9]*", "", gdb.VERSION)


class CachedType:
	_type = None

	def _new_objfile_handler(self, event):
		self._type = None
		gdb.events.new_objfile.disconnect(self._new_objfile_handler)

	def __init__(self, name):
		self._name = name

	def get_type(self):
		if self._type == None:
			self._type = gdb.lookup_type(self._name)
			if self._type == None:
				raise gdb.GdbError("cannot resolve " \
						   "type '%s'" % self._name)
			gdb.events.new_objfile.connect(
				self._new_objfile_handler)
		return self._type

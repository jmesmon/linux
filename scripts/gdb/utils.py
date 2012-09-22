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


long_type = CachedType("long")

def get_long_type():
	global long_type
	return long_type.get_type()


def offset_of(typeobj, field):
	element = gdb.Value(0).cast(typeobj)
	return int(str(element[field].address).split()[0], 16)

def container_of(ptr, typeobj, member):
	return (ptr.cast(get_long_type()) -
		offset_of(typeobj, member)).cast(typeobj)


class ContainerOf(gdb.Function):
	__doc__ = "Return pointer to containing data structure.\n" \
		  "\n" \
		  "$container_of(PTR, \"TYPE\", \"ELEMENT\"): Given PTR, return a pointer to the\n" \
		  "data structure of the type TYPE in which PTR is the address of ELEMENT.\n" \
		  "Note that TYPE and ELEMENT have to be quoted as strings."

	def __init__(self):
		super(ContainerOf, self).__init__("container_of")

	def invoke(self, ptr, typename, elementname):
		return container_of(ptr,
				    gdb.lookup_type(typename.string()).pointer(),
				    elementname.string())

ContainerOf()

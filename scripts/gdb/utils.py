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


char_type = CachedType("char")

def get_char_type():
	global char_type
	return char_type.get_type()


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


BIG_ENDIAN = 0
LITTLE_ENDIAN = 1
target_endianness = None

def get_target_endianness():
	global target_endianness
	if target_endianness == None:
		endian = gdb.execute("show endian", False, True)
		if endian.find("little endian") >= 0:
			target_endianness = LITTLE_ENDIAN
		elif endian.find("big endian") >= 0:
			target_endianness = BIG_ENDIAN
		else:
			raise gdb.GdgError("unknown endianness '%s'" % endian)
	return target_endianness

def read_u16(buffer):
	if get_target_endianness() == LITTLE_ENDIAN:
		return ord(buffer[0]) + (ord(buffer[1]) << 8)
	else:
		return ord(buffer[1]) + (ord(buffer[0]) << 8)

def read_u32(buffer):
	if get_target_endianness() == LITTLE_ENDIAN:
		return read_u16(buffer[0:2]) + (read_u16(buffer[2:4]) << 16)
	else:
		return read_u16(buffer[2:4]) + (read_u16(buffer[0:2]) << 16)

def read_u64(buffer):
	if get_target_endianness() == LITTLE_ENDIAN:
		return read_u32(buffer[0:4]) + (read_u32(buffer[4:8]) << 32)
	else:
		return read_u32(buffer[4:8]) + (read_u32(buffer[0:4]) << 32)


target_arch = None

def is_target_arch(arch):
	global target_arch
	if target_arch == None:
		target_arch = gdb.execute("show architecture", False, True)
	return target_arch.find(arch) >= 0


GDBSERVER_QEMU = 0
GDBSERVER_KGDB = 1
gdbserver_type = None

def get_gdbserver_type():
	def exit_handler(event):
		global gdbserver_type
		gdbserver_type = None
		gdb.events.exited.disconnect(exit_handler)

	def probe_qemu():
		try:
			return gdb.execute("monitor info version", False,
					   True) != ""
		except:
			return False

	def probe_kgdb():
		try:
			thread_info = gdb.execute("info thread 2", False, True)
			return thread_info.find("shadowCPU0") >= 0
		except:
			return False

	global gdbserver_type
	if gdbserver_type == None:
		if probe_qemu():
			gdbserver_type = GDBSERVER_QEMU
		elif probe_kgdb():
			gdbserver_type = GDBSERVER_KGDB
		if gdbserver_type != None:
			gdb.events.exited.connect(exit_handler)
	return gdbserver_type

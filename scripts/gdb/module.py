#
# gdb helper commands and functions for Linux kernel debugging
#
#  module tools
#
# Copyright (c) Siemens AG, 2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
import os
import string

from percpu import *
from utils import *

module_type = CachedType("struct module")

def for_each_module(func, arg = None):
	global module_type
	module_ptr_type = module_type.get_type().pointer()
	modules = gdb.parse_and_eval("modules")
	entry = modules['next']
	while entry != modules.address:
		module = container_of(entry, module_ptr_type, "list")
		func(module, arg)
		entry = entry['next']

def find_module_by_name(name):
	def match_name(module, arg):
		if module['name'].string() == arg['name']:
			arg['module'] = module

	arg = { 'name': name, 'module': None }
	for_each_module(match_name, arg)

	return arg['module']


class LxModule(gdb.Function):
	__doc__ = "Find module by name and return the module variable.\n" \
		  "\n" \
		  "$lx_module(MODULE): Given the name MODULE, iterate over all loaded modules of\n" \
		  "the target and return that module variable which MODULE matches."

	def __init__(self):
		super(LxModule, self).__init__("lx_module")

	def invoke(self, mod_name):
		mod_name = mod_name.string()
		module = find_module_by_name(mod_name)
		if module:
			return module.dereference()
		else:
			raise gdb.GdbError("Unable to find MODULE " + mod_name)

LxModule()


class LxModvar(gdb.Function):
	__doc__ = "Return global variable of a module.\n" \
		  "\n" \
		  "$lx_modvar(\"VAR\"[, \"MODULE\"]): Return the global variable called VAR that is\n" \
		  "defined by given MODULE. If MODULE is omitted, the current frame is used to\n" \
		  "try finding the corresponding module name."

	def __init__(self):
		super(LxModvar, self).__init__("lx_modvar")

	def _lookup_mod_symbol(self, module, var_name):
		char_ptr_type = get_char_type().pointer()
		for i in range(0, int(module['num_symtab'])):
			symtab_entry = module['symtab'][i]
			idx = int(symtab_entry['st_name'])
			synname_addr = module['strtab'][idx].address
			symname = synname_addr.cast(char_ptr_type)
			if symname.string() == var_name:
				return symtab_entry['st_value']
		return None

	def invoke(self, var_name, mod_name = None):
		if (mod_name == None):
			obj = gdb.selected_frame().function().symtab.objfile
			mod_name = string.replace(
				os.path.basename(obj.filename), ".ko", "")
			module = find_module_by_name(mod_name)
			if module == None:
				raise gdb.GdbError("Current frame does not " \
						   "belong to a module")
		else:
			mod_name = mod_name.string()
			module = find_module_by_name(mod_name)
			if module == None:
				raise gdb.GdbError("Unable to find MODULE " +
						   mod_name)
		var_name = var_name.string()
		var_addr = self._lookup_mod_symbol(module, var_name)
		if var_addr == None:
			raise gdb.GdbError("Unable to find VAR " + var_name)
		var = gdb.parse_and_eval(var_name)
		return var_addr.cast(var.type.pointer()).dereference()

LxModvar()


class LinuxLsmod(gdb.Command):
	__doc__ = "List currently loaded modules."

	_module_use_type = CachedType("struct module_use")

	def __init__(self):
		super(LinuxLsmod, self).__init__("lx-lsmod", gdb.COMMAND_DATA)

	def invoke(self, arg, from_tty):
		def print_module(module, arg):
			def collect_ref(cpu, arg):
				refptr = per_cpu(arg['refptr'], cpu)
				arg['ref'] += refptr['incs']
				arg['ref'] -= refptr['decs']

			arg = { 'refptr': module['refptr'], 'ref': 0 }
			for_each_cpu("cpu_possible_mask", collect_ref, arg)

			print "%s" % module['module_core'] + \
			      " %-19s" % module['name'].string() + \
			      " %8s" % module['core_size'] + \
			      "  %d" % arg['ref'],

			source_list = module['source_list']
			t = self._module_use_type.get_type().pointer()
			entry = source_list['next']
			first = True
			while entry != source_list.address:
				use = container_of(entry, t, "source_list")
				gdb.write((" " if first else ",") + \
					  use['source']['name'].string())
				first = False
				entry = entry['next']
			print

		print "Address%s    Module                  Size  Used by" % \
		      "        " if get_long_type().sizeof == 8 else ""
		for_each_module(print_module)

LinuxLsmod()

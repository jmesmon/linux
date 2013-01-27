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

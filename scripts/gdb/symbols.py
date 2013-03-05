#
# gdb helper commands and functions for Linux kernel debugging
#
#  load kernel and module symbols
#
# Copyright (c) Siemens AG, 2011-2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
import os
import re
import string

from module import for_each_module
from utils import *

class LinuxSymbols(gdb.Command):
	__doc__ = "(Re-)load symbols of Linux kernel and currently loaded modules.\n" \
		  "\n" \
		  "The kernel (vmlinux) is taken from the current working directly. Modules (.ko)\n" \
		  "are scanned recursively, starting in the same directory. Optionally, the module\n" \
		  "search path can be extended by a space separated list of paths passed to the\n" \
		  "lx-symbols command."

	module_files = []
	breakpoint = None

	def __init__(self):
		super(LinuxSymbols, self).__init__("lx-symbols",
						   gdb.COMMAND_FILES,
						   gdb.COMPLETE_FILENAME)

	def _load_module_symbols(self, module, arg = None):
		module_name = module['name'].string()
		module_addr = str(module['module_core']).split()[0]
		module_pattern = ".*/" + \
			string.replace(module_name, "_", r"[_\-]") + r"\.ko$"
		module_path = ""
		for name in self.module_files:
			if re.match(module_pattern, name):
				module_path = name
				break

		if module_path:
			print "loading @" + module_addr + ": " + module_path
			if gdb.VERSION >= "7.2":
				gdb.execute("add-symbol-file " + \
					    module_path + " " + module_addr,
					    to_string = True)
			else:
				gdb.execute("add-symbol-file " + \
					    module_path + " " + module_addr)
		else:
			print "no module object found for '" + \
			      module_name + "'"

	if gdb_version >= "7.3":
		class _LoadModuleBreakpoint(gdb.Breakpoint):
			def __init__(self, spec, gdb_command):
				super(LinuxSymbols._LoadModuleBreakpoint,
				      self).__init__(spec, internal = True)
				self.silent = True
				self.gdb_command = gdb_command

			def stop(self):
				module = gdb.parse_and_eval("mod")
				self.gdb_command._load_module_symbols(module)
				return False

		def _find_breakpoint_location(self):
			breakpoint_match = "^[0-9]*[\t]*err = parse_args\(.*"

			src = gdb.execute("list kernel/module.c:load_module",
					  to_string = True)
			done = False
			lineno = None
			while not done:
				src = gdb.execute("list", to_string = True)
				for line in src.splitlines():
					if re.match(breakpoint_match, line):
						done = True
						lineno = line.split()[0]
						break
					elif re.match("^[0-9]*\t}$", line):
						done = True
						break
			return lineno

	def _load_all_module_symbols(self, arg):
		if gdb.parse_and_eval("modules.next == &modules"):
			print "no modules found"
			return

		self.module_files = []
		paths = arg.split()
		paths.append(os.getcwd())
		for path in paths:
			print "scanning for modules in " + path
			for root, dirs, files in os.walk(path):
				for name in files:
					if re.match(r".*\.ko$", name):
						self.module_files.append(
							root + "/" + name)

		for_each_module(self._load_module_symbols)

	def invoke(self, arg, from_tty):
		print "loading vmlinux"

		saved_states = []
		if (gdb.breakpoints() != None):
			for breakpoint in gdb.breakpoints():
				saved_states.append({
					'breakpoint': breakpoint,
					'enabled': breakpoint.enabled })

		# drop all current symbols and reload vmlinux
		gdb.execute("symbol-file", to_string = True)
		gdb.execute("symbol-file vmlinux")

		self._load_all_module_symbols(arg)

		for saved_state in saved_states:
			saved_state['breakpoint'].enabled = \
				saved_state['enabled']

		if gdb_version < "7.3":
			print "Note: symbol update on module loading not " \
			      "supported with this gdb version"
		else:
			if self.breakpoint != None:
				self.breakpoint.delete()
				self.breakpoint = None
			lineno = self._find_breakpoint_location()
			if lineno != None:
				self.breakpoint = self._LoadModuleBreakpoint(
					lineno, self)
			else:
				print "unable to detect breakpoint location " \
				      "of load module completion"

LinuxSymbols()

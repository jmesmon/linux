#
# gdb helper commands and functions for Linux kernel debugging
#
#  task & thread tools
#
# Copyright (c) Siemens AG, 2011-2013
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb

from utils import *

task_type = CachedType("struct task_struct")

def for_each_task(func, arg = None):
	global task_type
	task_ptr_type = task_type.get_type().pointer()
	init_task = gdb.parse_and_eval("init_task")
	g = init_task.address
	while True:
		g =  container_of(g['tasks']['next'], task_ptr_type, "tasks")
		if g == init_task.address:
			break;
		t = g
		while True:
			func(t, arg)
			t = container_of(t['thread_group']['next'],
					 task_ptr_type, "thread_group")
			if t == g:
				break

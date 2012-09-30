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

def get_task_by_pid(pid):
	def match_pid(t, arg):
		if int(t['pid']) == arg['pid']:
			arg['task'] = t

	arg = { 'pid': pid, 'task': None }
	for_each_task(match_pid, arg)

	return arg['task']


class LxTaskByPidFunc(gdb.Function):
	__doc__ = "Find Linux task by PID and return the task_struct variable.\n" \
		  "\n" \
		  "$lx_task_by_pid(PID): Given PID, iterate over all tasks of the target and\n" \
		  "return that task_struct variable which PID matches."

	def __init__(self):
		super(LxTaskByPidFunc, self).__init__("lx_task_by_pid")

	def invoke(self, pid):
		task = get_task_by_pid(pid)
		if task:
			return task.dereference()
		else:
			raise gdb.GdbError("No task of PID " + str(pid))

LxTaskByPidFunc()


thread_info_type = CachedType("struct thread_info")

ia64_task_size = None

def get_thread_info(task):
	global thread_info_type
	thread_info_ptr_type = thread_info_type.get_type().pointer()
	if is_target_arch("ia64"):
		global ia64_task_size
		if ia64_task_size == None:
			ia64_task_size = gdb.parse_and_eval(
						"sizeof(struct task_struct)")
		thread_info_addr = task.address + ia64_task_size
		thread_info = thread_info_addr.cast(thread_info_ptr_type)
	else:
		thread_info = task['stack'].cast(thread_info_ptr_type)
	return thread_info.dereference()


class LxThreadInfoFunc (gdb.Function):
	# Calculate Linux thread_info from task variable.
	__doc__ = "Calculate Linux thread_info from task variable.\n" \
		  "\n" \
		  "$lx_thread_info(TASK): Given TASK, return the corresponding thread_info\n" \
		  "variable."

	def __init__(self):
		super(LxThreadInfoFunc, self).__init__("lx_thread_info")

	def invoke(self, task):
		return get_thread_info(task)

LxThreadInfoFunc()

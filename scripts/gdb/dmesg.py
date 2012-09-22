#
# gdb helper commands and functions for Linux kernel debugging
#
#  kernel log buffer dump
#
# Copyright (c) Siemens AG, 2011, 2012
#
# Authors:
#  Jan Kiszka <jan.kiszka@siemens.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#

import gdb
import string

from utils import *

class LinuxDmesg(gdb.Command):
	__doc__ = "Print Linux kernel log buffer."

	def __init__(self):
		super(LinuxDmesg, self).__init__("lx-dmesg", gdb.COMMAND_DATA)

	def invoke(self, arg, from_tty):
		log_buf_addr = \
			int(str(gdb.parse_and_eval("log_buf")).split()[0], 16)
		log_first_idx = int(gdb.parse_and_eval("log_first_idx"))
		log_next_idx = int(gdb.parse_and_eval("log_next_idx"))
		log_buf_len = int(gdb.parse_and_eval("log_buf_len"))

		inf = gdb.inferiors()[0]
		start = log_buf_addr + log_first_idx
		if log_first_idx < log_next_idx:
			log_buf_2nd_half = -1
			length = log_next_idx - log_first_idx
			log_buf = inf.read_memory(start, length)
		else:
			log_buf_2nd_half = log_buf_len - log_first_idx
			log_buf = inf.read_memory(start, log_buf_2nd_half) + \
				  inf.read_memory(log_buf_addr, log_next_idx)

		pos = 0
		while pos < log_buf.__len__():
			length = read_u16(log_buf[pos + 8 : pos + 10])
			if length == 0:
				if log_buf_2nd_half == -1:
					print "Corrupted log buffer!"
					break
				pos = log_buf_2nd_half
				continue

			text_len = read_u16(log_buf[pos + 10 : pos + 12])
			time_stamp = read_u64(log_buf[pos : pos + 8])

			for line in log_buf[pos + 16 :
					    pos + 16 + text_len].splitlines():
				print "[%13.6f] " % \
				      (time_stamp / 1000000000.0) + line

			pos += length

LinuxDmesg()

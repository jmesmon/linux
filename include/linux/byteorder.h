#ifndef LINUX_BYTEORDER_H_
#define LINUX_BYTEORDER_H_

#include <asm/byteorder.h>

#define be_to_cpu(v) \
	__builtin_choose_expr(sizeof(v) == sizeof(uint8_t) , v,	\
	__builtin_choose_expr(sizeof(v) == sizeof(uint16_t), be16_to_cpu(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint32_t), be32_to_cpu(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint64_t), be64_to_cpu(v), \
		(void)0))))

#define le_to_cpu(v) \
	__builtin_choose_expr(sizeof(v) == sizeof(uint8_t) , v,	\
	__builtin_choose_expr(sizeof(v) == sizeof(uint16_t), le16_to_cpu(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint32_t), le32_to_cpu(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint64_t), le64_to_cpu(v), \
		(void)0))))

#define cpu_to_le(v) \
	__builtin_choose_expr(sizeof(v) == sizeof(uint8_t) , v,	\
	__builtin_choose_expr(sizeof(v) == sizeof(uint16_t), cpu_to_le16(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint32_t), cpu_to_le32(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint64_t), cpu_to_le64(v), \
		(void)0))))

#define cpu_to_be(v) \
	__builtin_choose_expr(sizeof(v) == sizeof(uint8_t) , v,	\
	__builtin_choose_expr(sizeof(v) == sizeof(uint16_t), cpu_to_be16(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint32_t), cpu_to_be32(v), \
	__builtin_choose_expr(sizeof(v) == sizeof(uint64_t), cpu_to_be64(v), \
		(void)0))))

#endif

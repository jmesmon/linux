#include <linux/log2.h>
#include <linux/bug.h>

int ____ilog2_NaN(void)
{
	BUG();
}

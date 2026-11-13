// SPDX-License-Identifier: GPL-3.0-or-later
// HOST TEST DOUBLE: the one CUtils function MMDVMHost's Golay24128.cpp needs (its Utils.cpp pulls in the logger).
#include "Utils.h"
unsigned int CUtils::countBits(unsigned int v)
{
	unsigned int n = 0U;
	for (; v != 0U; v &= v - 1U) n++;
	return n;
}

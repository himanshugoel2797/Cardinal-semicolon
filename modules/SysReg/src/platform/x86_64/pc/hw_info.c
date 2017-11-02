/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */
#include <types.h>

PRIVATE int add_cpuid();

// TODO: parse and place CPUID information
int add_platform_info() {
  int retVal = add_cpuid();
  if (retVal != 0)
    return retVal;
  return 0;
}
// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSDEBUG_SERIALIO_H
#define CARDINAL_SYSDEBUG_SERIALIO_H

int init_serial_debug();
int debug_shell(char (*input_stream)(), void (*output_stream)(char));

#endif
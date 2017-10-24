#include "serialio.h"

int module_init() {
    init_serial_debug();
    return 0;
}
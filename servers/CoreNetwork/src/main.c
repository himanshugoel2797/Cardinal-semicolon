/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlist.h>
#include "net_priv.h"
#include "CoreNetwork/driver.h"

extern list_t dev_list;
extern list_t interface_list;
extern int devIDs[network_device_type_count];

void corenetwork_register_tests(void);

int module_init() {

    list_init(&dev_list);
    list_init(&interface_list);

    for (int i = 0; i < network_device_type_count; i++)
        devIDs[i] = 0;

    corenetwork_register_tests();

    return 0;
}
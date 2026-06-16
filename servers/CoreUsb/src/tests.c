// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "CoreUsb/usb.h"
#include "SysTest/test.h"

// Registering a host controller with a minimal (null-handler) descriptor should
// succeed and hand back a non-NULL handle.
static void test_register_hostcontroller(test_ctx_t *ctx) {
    static usb_hci_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    strncpy(desc.name, "test", sizeof(desc.name) - 1);
    desc.device_type = usb_device_type_uhci;
    desc.state = NULL;
    desc.lock = 0;

    void *handle = NULL;
    int rc = usb_register_hostcontroller(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_MSG(ctx, handle != NULL, "host-controller handle must be non-NULL");
}

// A probe that always declines (returns -1) is still a valid class driver:
// registration just records the callback and returns 0.
static int decline_probe(usb_enum_device_t *dev) {
    (void)dev;
    return -1;
}
static void mock_remove(usb_enum_device_t *dev) {
    (void)dev;
}

static void test_register_class_driver(test_ctx_t *ctx) {
    // Use a class byte that no real class driver claims (0xFE) so this test
    // cannot clobber a live driver's registration.
    const uint8_t cls = 0xFE;

    int rc = usb_register_class_driver(cls, decline_probe, mock_remove);
    TEST_CHECK_EQ_U(ctx, rc, 0);

    // The probe and remove callbacks must actually be stored in the class table
    // under our class byte -- registration is not a no-op.
    TEST_CHECK_EQ_PTR(ctx, usb_class_driver_probe(cls), decline_probe);
    TEST_CHECK_EQ_PTR(ctx, usb_class_driver_remove(cls), mock_remove);

    // Re-registering the same class overwrites the slot (last writer wins) and
    // does not bleed into a neighbouring class byte.
    rc = usb_register_class_driver(cls, decline_probe, NULL);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_EQ_PTR(ctx, usb_class_driver_probe(cls), decline_probe);
    TEST_CHECK_MSG(ctx, usb_class_driver_remove(cls) == NULL,
                   "remove callback was not overwritten on re-register");
    TEST_CHECK_MSG(ctx, usb_class_driver_probe((uint8_t)(cls - 1)) != decline_probe,
                   "registration bled into a neighbouring class slot");
}

void coreusb_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreUsb",
            .name = "register_hostcontroller",
            .fn = test_register_hostcontroller,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreUsb",
            .name = "register_class_driver",
            .fn = test_register_class_driver,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}

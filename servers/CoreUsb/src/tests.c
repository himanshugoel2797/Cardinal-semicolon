// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "CoreUsb/usb.h"
#include "SysTest/test.h"
// usb_priv.h is on the CoreUsb private include path ("inc"); the handle returned
// by usb_register_hostcontroller is a usb_hci_def_t*, so the test can look
// through it to confirm the registered fields landed where the rest of CoreUsb
// reads them.
#include "usb_priv.h"

// Registering a host controller must succeed, hand back a non-NULL handle, and
// that handle must reflect the descriptor we registered: its device_type, a
// per-type index, and a verbatim copy of the descriptor (name, state, ...).
static void test_register_hostcontroller(test_ctx_t *ctx) {
    static usb_hci_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    strncpy(desc.name, "test-uhci", sizeof(desc.name) - 1);
    desc.device_type = usb_device_type_uhci;
    desc.state = (void *)0xABCD1234;
    desc.lock = 0;

    void *handle = NULL;
    int rc = usb_register_hostcontroller(&desc, &handle);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_MSG(ctx, handle != NULL, "host-controller handle must be non-NULL");

    // The handle is CoreUsb's internal usb_hci_def_t. Verify it carries the
    // type/name/state we registered -- i.e. registration is not a black box that
    // discards descriptor fields.
    usb_hci_def_t *def = (usb_hci_def_t *)handle;
    TEST_CHECK_EQ_U(ctx, def->type, usb_device_type_uhci);
    TEST_CHECK_EQ_U(ctx, def->device.device_type, usb_device_type_uhci);
    TEST_CHECK_EQ_PTR(ctx, def->device.state, desc.state);
    TEST_CHECK_MSG(ctx, strcmp(def->device.name, "test-uhci") == 0,
                   "registered controller name did not survive into the handle");
    // The per-type index is assigned monotonically; the very first controller of
    // any type can't have a negative index.
    TEST_CHECK_MSG(ctx, def->idx >= 0, "controller index must be non-negative");

    // Register a SECOND controller of the same type: its index must advance,
    // proving the controllers are tracked individually (membership in the
    // internal handle list), not collapsed onto one slot.
    static usb_hci_desc_t desc2;
    memset(&desc2, 0, sizeof(desc2));
    strncpy(desc2.name, "test-uhci-2", sizeof(desc2.name) - 1);
    desc2.device_type = usb_device_type_uhci;

    void *handle2 = NULL;
    rc = usb_register_hostcontroller(&desc2, &handle2);
    TEST_CHECK_EQ_U(ctx, rc, 0);
    TEST_CHECK_MSG(ctx, handle2 != NULL && handle2 != handle,
                   "second controller must get its own distinct handle");
    usb_hci_def_t *def2 = (usb_hci_def_t *)handle2;
    TEST_CHECK_MSG(ctx, def2->idx == def->idx + 1,
                   "second same-type controller did not get the next index");
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

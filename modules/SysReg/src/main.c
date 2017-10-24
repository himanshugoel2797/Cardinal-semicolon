#include <types.h>

// Parse tables and store them in a registry
// Load and initialize second stage general purpose registry after memory module
// has been initialized.

// Determine the system's complete topology, add it to the registry
// Get the system's complete information, PCI, NUMA, ACPI etc
// Use this to initialize the entire system in one go

int module_init() {
    return 0;
}
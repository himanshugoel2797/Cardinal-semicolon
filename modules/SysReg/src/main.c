#include <types.h>

// Parse tables and store them in a registry
// Load and initialize second stage general purpose registry after memory module
// has been initialized.

// Determine the system's complete topology, add it to the registry
// Get the system's complete information, PCI, NUMA, ACPI etc
// Use this to initialize the entire system in one go

// First setup physical memory and virtual memory for the primary core, then
// install the local IDT and use it to boot all cores
// Start the other cores, have them obtain a lock on the registry and determine
// their topological parameters
// pass these parameters to initialize their physical and virtual memory
// managers
// BSP behaves like a core and follows the same procedure
// All cores enter scheduler
// Initialization task is executed, loading in drivers and starting services
// User mode handoff

// Registry is a json database

int module_init() { return 0; }
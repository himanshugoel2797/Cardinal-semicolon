Design a functional shader language that is fed straight to drivers.

Drivers receive a command queue, can design this so that the command queue format is irrelevant to the server, allowing the user side api to also be designed to directly output the desired commands.

Graphics API is very minimal, Vulkan like.

cgso <- Object Management - generic object management API
    allocTex - allocate memory for a texture and return its handle, optionally with a pointer to write to
    freeTex - free the specified texture

cds <- Display API - used as a means to address CoreDisplay displays
    requestDisplay - get the read-only texture object for this display
    requestDisplayOverlay - create an overlay target texture object for the specified display

cgs2d <- 2d graphics API (blitter etc)
    bltRect -> transfer source block to destination block with blending modes
    

cgs3d <- 3d graphics API
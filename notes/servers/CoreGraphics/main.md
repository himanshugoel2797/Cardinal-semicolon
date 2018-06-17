Design a functional shader language that is fed straight to drivers.

Drivers receive a command queue, can design this so that the command queue format is irrelevant to the server, allowing the user side api to also be designed to directly output the desired commands.

Graphics API is very minimal, Vulkan like.

cds <- Display API
cgs2d <- 2d graphics API (blitter etc)
cgs3d <- 3d graphics API
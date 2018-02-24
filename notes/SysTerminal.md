<!---
 Copyright (c) 2017 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

Implements Cardinal; terminal device

Supported activities:
child   - create a child terminal
exec    - execute process with arguments under current terminal
exit    - close current terminal

A terminal device is simply a state machine, it inputs/outputs Mana render structures, which the GUI must interpret to present a device

All programs are like modules, they have a initialization function, and can register event handlers

All programs are executed in a terminal mode, they can use a syscall to request a ui renderer
They then submit their structural description to the ui renderer and register event handlers

Terminals form a tree, they possess the following properties:
    - Local User ID
    - Terminal ID

Terminals can each run one program. Drivers are modules of the service providers. Service providers + drivers are kernel objects, they register syscalls which user mode software can call.
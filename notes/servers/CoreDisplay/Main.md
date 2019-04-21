<!---
 Copyright (c) 2018 Himanshu Goel
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

The display system tracks all the displays and their drivers, as well as communicating to the graphics subsystem.

The following display parameters are tracked:
    - Resolution
    - Possible resolutions
    - Display dimensions
    - Color bit depth
    - Refresh rate (if available)
    - Variable refresh rate support (if present)

The display system also works with the audio system to support HDMI audio output.
The system also plugs into the usb system, providing support for usb displays.
# The Core* OS services are all Lisp now (lisp/servers/*.clp, brought up by
# lisp/init.clp): CoreInput, CoreAudio, CorePower, CoreStorage (+ cardfs),
# CoreDisplay, CoreNetwork (+ CoreNetDebug) and CoreUsb. CoreDriver (the C
# PCI->driver binder that read devices.txt) is gone -- lisp/init.clp binds
# devices now. No C server modules remain to build, and the public headers under
# servers/inc/ were deleted once the last C driver that included them was ported.

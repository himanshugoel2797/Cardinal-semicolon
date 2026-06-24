# The Core* OS services are all Lisp now (lisp/servers/*.clp, brought up by
# lisp/init.clp): CoreInput, CoreAudio, CorePower, CoreStorage (+ cardfs),
# CoreDisplay, CoreNetwork (+ CoreNetDebug) and CoreUsb. CoreDriver (the C
# PCI->driver binder that read devices.txt) is gone -- lisp/init.clp binds
# devices now. No C server modules remain to build; servers/inc/ keeps the
# public headers that any surviving C code includes.

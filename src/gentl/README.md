# GenTL producer

Function signatures are defined by EMVA; they are declared by
`src/arvgentlprivate.h`, written from the API documentation, which the GenTL
consumer of Aravis shares. `TODO.md` lists what is not implemented.

Implementation of those functions is split into modules indicated by function
prefixes:

* `GC`: GeniCam (system)
* `TL`: transport layer
* `IF`: interface
* `Dev`: device
* `DS`: data stream
* `Event`

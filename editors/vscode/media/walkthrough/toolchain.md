# The Turf toolchain

Editing needs nothing but this extension: the language server and the standard
library come with it.

*Running* a program needs the Turf compiler, because that is what compiles it.
If it is not installed, **Run**, **Build**, **Check** and **Test** tell you so
and show how to get it; everything else keeps working.

If you have installed it somewhere unusual, set `turf.toolchain.path` to the
`turf` executable (or to the installation directory).

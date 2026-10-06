# Imports are one keystroke

Type the name of a module you have not imported, such as `json`, and accept it
from the completion list: the `import json;` line is added to the top of the
file for you.

If you use a module you forgot to import, the error has a quick fix. Put the
cursor on it and press **Ctrl+.** (**Cmd+.** on macOS): *Add `import io;`*.

Inside a project, `import` also finds your own modules anywhere under `src/` and
the dependencies listed in `turf.toml`. **Ctrl+click** an import to open that
module's file.

**Organize imports** (in the same menu) sorts and de-duplicates them.

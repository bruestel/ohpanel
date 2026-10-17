"""
Archives through the compiler's ar and ranlib wrappers (xtensa-esp32-elf-gcc-ar), which load the LTO plugin.

The firmware's own components are built for link-time optimisation (CMakeLists.txt). PlatformIO packs every
component into an archive with plain ar, whose symbol index knows nothing of the LTO code: the linker then takes the
ordinary code of the (fat) objects and never optimises across them. With the wrappers the index covers it.
"""

import os

Import("env")  # noqa: F821  (injected by PlatformIO)

for tool in ("AR", "RANLIB"):
    path = env.subst("$" + tool)  # noqa: F821
    head, name = os.path.split(path)
    wrapped = os.path.join(head, name.replace("-ar", "-gcc-ar").replace("-ranlib", "-gcc-ranlib"))
    if os.path.exists(wrapped) or env.WhereIs(wrapped):  # noqa: F821
        env.Replace(**{tool: wrapped})  # noqa: F821

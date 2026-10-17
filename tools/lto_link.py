"""
Lets the firmware's link run link-time optimisation: ESP-IDF adds -fno-lto to every link (its CMakeLists.txt), after
the -flto the firmware's own components ask for (CMakeLists.txt), and the last one wins. Taken out here, the linker
optimises across the components built for it -- ESP-IDF's own objects carry no LTO code and stay as they are.
"""

Import("env")  # noqa: F821  (injected by PlatformIO)

flags = env.get("LINKFLAGS", [])  # noqa: F821
env.Replace(LINKFLAGS=[f for f in flags if f != "-fno-lto"])  # noqa: F821

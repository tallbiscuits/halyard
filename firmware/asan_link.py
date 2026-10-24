# SPDX-License-Identifier: GPL-3.0-or-later
# Link the native test binary with the sanitizers it was compiled with.
Import("env")
env.Append(LINKFLAGS=["-fsanitize=address,undefined"])

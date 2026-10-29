# SPDX-License-Identifier: GPL-3.0-or-later
# Writes src/version.h: Halyard's version plus the git commit it was built from, so an OTA
# update can be checked by what the new firmware reports (/api/version, the console's "version").
import datetime
import os
import subprocess

Import("env")

BASE = "0.6.1"
here = env.subst("$PROJECT_DIR")
try:
    rev = subprocess.check_output(["git", "describe", "--always", "--dirty", "--abbrev=7"],
                                  cwd=here, text=True).strip()
except Exception:
    rev = "unknown"
built = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
# A release build names its version (./run.sh release VERSION); otherwise BASE plus the commit.
version = os.environ.get("HALYARD_VERSION") or "%s-g%s" % (BASE, rev)
# The Mainsail this firmware ships with (D13): Halyard's build, named by mainsail/UPSTREAM and
# mainsail/REVISION, "v2.19.0-halyard.1".
fork = os.path.join(here, "..", "mainsail")
with open(os.path.join(fork, "UPSTREAM")) as f, open(os.path.join(fork, "REVISION")) as g:
    mainsail = "%s-halyard.%s" % (f.read().strip(), g.read().strip())
text = ('#pragma once\n#define HALYARD_VERSION "%s"\n#define HALYARD_BUILT "%s"\n'
        '#define HALYARD_MAINSAIL "%s"\n' % (version, built, mainsail))
path = os.path.join(here, "src", "version.h")
old = open(path).read() if os.path.exists(path) else ""
if old != text:
    with open(path, "w") as f:
        f.write(text)

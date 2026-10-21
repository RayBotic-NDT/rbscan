"""Test the package from the CMake build tree when ctest asks for it.

An editable install (pip/uv install -e .) maps `rbscan` to its own build with
an import hook that wins over PYTHONPATH. ctest sets RBSCAN_TEST_PACKAGE_DIR so
the tests run against the build they belong to.
"""

import importlib.machinery
import os
import sys

_package_dir = os.environ.get("RBSCAN_TEST_PACKAGE_DIR")
if _package_dir:
    sys.path.insert(0, _package_dir)
    # Plain sys.path lookup before any editable-install hook.
    sys.meta_path.insert(0, importlib.machinery.PathFinder)

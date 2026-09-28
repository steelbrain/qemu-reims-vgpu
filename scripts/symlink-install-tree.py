#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import shutil
import subprocess
import sys

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

introspect = os.environ.get('MESONINTROSPECT')
out = subprocess.run([*shlex.split(introspect), '--installed'],
                     stdout=subprocess.PIPE, check=True).stdout
for source, dest in json.loads(out).items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except BaseException as e:
        print(f'error making directory {path}', file=sys.stderr)
        raise e
    try:
        os.symlink(source, bundle_dest)
    except BaseException as e:
        # Windows without Developer Mode or elevation cannot create symlinks
        # (ERROR_PRIVILEGE_NOT_HELD). Copy what already exists instead; build
        # outputs that do not exist yet at configure time are skipped.
        if (os.name == 'nt' and isinstance(e, OSError)
                and getattr(e, 'winerror', None) == 1314):
            if os.path.isfile(source):
                shutil.copy2(source, bundle_dest)
            continue
        if not isinstance(e, OSError) or e.errno != errno.EEXIST:
            if os.name == 'nt':
                print('Please enable Developer Mode to support soft link '
                      'without Administrator permission')
            print(f'error making symbolic link {dest}', file=sys.stderr)
            raise e

"""Fetches the third-party sources of the SDK into sdk/thirdparty.

sdk/thirdparty only carries the files this port changed (see THIRD_PARTY_NOTICES.md). Everything else comes from the
ReXGlue SDK release this port is based on and its submodules. This script clones that release with its submodules
into a temporary folder and copies every file that sdk/thirdparty does not have yet; files already there are kept.

Usage: python tools/fetch_thirdparty.py
Needs git.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

UPSTREAM = 'https://github.com/rexglue/rexglue-sdk.git'
COMMIT = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'  # v0.10.0


def run(*args, cwd=None):
    print('>', ' '.join(args))
    subprocess.run(args, cwd=cwd, check=True)


def remove_readonly(func, path, _):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, 'sdk', 'thirdparty')
    work = tempfile.mkdtemp(prefix='rexglue-sdk-')
    try:
        run('git', 'init', '-q', work)
        run('git', 'remote', 'add', 'origin', UPSTREAM, cwd=work)
        run('git', 'fetch', '-q', '--depth', '1', 'origin', COMMIT, cwd=work)
        run('git', 'checkout', '-q', 'FETCH_HEAD', cwd=work)
        run('git', 'submodule', 'update', '--init', '--recursive', cwd=work)
        source = os.path.join(work, 'thirdparty')
        copied = kept = dangling = 0
        for dirpath, dirnames, filenames in os.walk(source):
            dirnames[:] = [d for d in dirnames if d != '.git']
            for name in filenames:
                # Dependencies are local exports, not nested repositories.
                # Their ignore rules can undo our root dependency exclusion.
                if name in ('.git', '.gitignore'):
                    continue
                src = os.path.join(dirpath, name)
                # Some submodules (MoltenVK) contain symlinks into checkouts
                # that are not part of the release. Windows clones them as
                # plain files; on POSIX they dangle and cannot be copied.
                if os.path.islink(src) and not os.path.exists(src):
                    dangling += 1
                    continue
                dst = os.path.join(target, os.path.relpath(src, source))
                if os.path.exists(dst):
                    kept += 1
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
                copied += 1
        # Exported dependencies have no .git metadata. Mark complete exports
        # only after the recursive checkout and copy both succeeded.
        for name in os.listdir(source):
            if os.path.isdir(os.path.join(source, name)):
                marker = os.path.join(target, name, '.rex-dependency-complete')
                with open(marker, 'w', encoding='utf-8') as stream:
                    stream.write(COMMIT + '\n')
        print(f'{copied} files copied into sdk/thirdparty, {kept} files of this port kept, '
              f'{dangling} dangling symlinks skipped')
    finally:
        if sys.version_info >= (3, 12):
            shutil.rmtree(work, onexc=remove_readonly)
        else:
            shutil.rmtree(work, onerror=remove_readonly)


if __name__ == '__main__':
    main()

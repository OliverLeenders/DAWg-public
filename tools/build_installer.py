"""Builds the installer for this platform from a Release build.

    python tools/build_installer.py [--build-dir build] [--no-build]

Builds the UI and the DAWg target in Release first (unless --no-build), so the installer never packages a stale app.

Windows: the setup program from installer/DAWg.iss, after checking that DAWg.exe carries its icon. Needs Inno Setup 6
(winget install JRSoftware.InnoSetup) and Visual Studio's MSVC runtime redistributables.

Linux: a .deb (the app in /opt/dawg, a `dawg` command, a desktop entry and icons; its dependencies read from the
executable through dpkg, so build on the oldest Debian or Ubuntu it should install on) and a .tar.gz to unpack
anywhere. Needs dpkg-deb, and Node.js and npm for the UI.

The packages land in <build dir>/installer.
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_iscc():
    candidates = [
        os.path.expandvars(r'%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe'),
        r'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        r'C:\Program Files\Inno Setup 6\ISCC.exe',
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    sys.exit('Inno Setup 6 not found: winget install JRSoftware.InnoSetup')


def find_cmake(build_dir):
    # The cmake that configured the build (Visual Studio's own is often not on PATH), else the one on PATH.
    cache = open(os.path.join(build_dir, 'CMakeCache.txt'), encoding='utf-8').read()
    match = re.search(r'CMAKE_COMMAND:INTERNAL=(.+)', cache)
    if match and os.path.exists(match.group(1).strip()):
        return match.group(1).strip()
    return shutil.which('cmake') or sys.exit('cmake not found: run this from a Developer PowerShell, or put cmake on PATH')


def find_redist(build_dir):
    # The runtime of the Visual Studio that configured the build, newest version.
    cache = open(os.path.join(build_dir, 'CMakeCache.txt'), encoding='utf-8').read()
    match = re.search(r'CMAKE_GENERATOR_INSTANCE:INTERNAL=(.+)', cache)
    if not match:
        sys.exit('No Visual Studio instance in CMakeCache.txt; is this a Visual Studio build?')
    dirs = sorted(glob.glob(os.path.join(match.group(1).strip(), 'VC', 'Redist', 'MSVC', '*', 'x64', 'Microsoft.VC*.CRT')))
    if not dirs:
        sys.exit('No MSVC runtime redistributables found under ' + match.group(1))
    return os.path.normpath(dirs[-1])


def has_icon(exe):
    """Whether a PE executable has an icon resource (an RT_GROUP_ICON entry in its resource directory)."""
    data = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return False
    sections, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    directories = optional + (112 if struct.unpack_from('<H', data, optional)[0] == 0x20B else 96)
    resource_rva = struct.unpack_from('<I', data, directories + 2 * 8)[0]  # data directory 2: resources
    if resource_rva == 0:
        return False

    for i in range(sections):
        header = optional + optional_size + 40 * i
        size, address, _, raw = struct.unpack_from('<IIII', data, header + 8)
        if address <= resource_rva < address + size:
            root = raw + resource_rva - address
            named, ids = struct.unpack_from('<HH', data, root + 12)
            entries = (struct.unpack_from('<I', data, root + 16 + 8 * e)[0] for e in range(named + ids))
            return any(entry == 14 for entry in entries)  # RT_GROUP_ICON
    return False


def verify_ui(app_dir):
    """Fail before packaging if the Release app's UI bundle is missing or incomplete."""
    ui_dir = os.path.join(app_dir, 'ui')
    index = os.path.join(ui_dir, 'index.html')
    if not os.path.exists(index):
        sys.exit('No bundled UI at ' + ui_dir)

    html = open(index, encoding='utf-8').read()
    scripts = re.findall(r'src="([^"]+\.js)"', html)
    if not scripts:
        sys.exit('Bundled UI has no JavaScript entrypoint: ' + index)

    entry = os.path.join(ui_dir, scripts[0].replace('/', os.sep))
    if not os.path.exists(entry):
        sys.exit('Bundled UI entrypoint is missing: ' + entry)


def run_inno_setup(command, attempts=4):
    """Run ISCC, retrying the resource-update failure that antivirus scanners cause on the output file.

    Inno Setup updates the version resources of the setup executable after writing it. If Windows Defender
    (or another scanner) has the file open, that step fails with EndUpdateResource failed (110). The file is
    usually released after a short wait, so a retry succeeds; the previous setup is left alone until then.
    """
    for attempt in range(1, attempts + 1):
        result = subprocess.run(command, capture_output=True, text=True, errors='replace')
        if result.returncode == 0:
            return

        output = (result.stdout or '') + (result.stderr or '')
        retryable = 'EndUpdateResource failed' in output or 'Resource update error' in output
        if not retryable or attempt == attempts:
            if output.strip():
                print(output.strip(), file=sys.stderr)
            sys.exit('Inno Setup failed with exit code %d' % result.returncode)

        print('Inno Setup could not update the installer resources; retrying (%d/%d)...' % (attempt + 1, attempts))
        # A scanner normally lets go within a second or two. The previous setup stays in place, so a run
        # that still fails leaves the last good installer behind.
        time.sleep(2 ** (attempt - 1))


# What the Linux app loads at run time rather than links: JUCE opens the web view, GTK, libcurl (update checks) and
# JACK with dlopen, so the executable's own list of libraries doesn't name them. Alternatives cover Ubuntu 24.04's
# renamed packages (t64) and older releases.
LINUX_LOADED_DEPENDS = [
    'libwebkit2gtk-4.1-0',
    'libgtk-3-0t64 | libgtk-3-0',
    'libcurl4t64 | libcurl4',
]
LINUX_RECOMMENDS = ['libjack-jackd2-0 | libjack0']

LINUX_DESKTOP_ENTRY = """[Desktop Entry]
Type=Application
Name=DAWg
GenericName=Digital Audio Workstation
Comment=A DAW with an MPE piano roll, node-based routing and Faust scripting
Exec=dawg
Icon=dawg
Terminal=false
Categories=AudioVideo;Audio;Midi;Music;
StartupWMClass=DAWg
"""

# Started through a script rather than a link, so the app finds ui/, faust/ and midi-files/ next to its real path.
LINUX_LAUNCHER = """#!/bin/sh
exec /opt/dawg/DAWg "$@"
"""


def version_of_project():
    cmake = open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8').read()
    return re.search(r'project\(DAWg VERSION ([0-9.]+)', cmake).group(1)


def build_app(build_dir, config_args):
    # Always rebuild the frontend directly. The generated CMake UI stamp can remain up to date when source
    # files are changed while the build tree is shared, which would otherwise package an older bundle.
    # npm.cmd only on Windows: under WSL, Windows' own npm can be on PATH too, and can't run there.
    npm = (shutil.which('npm.cmd') if sys.platform == 'win32' else None) or shutil.which('npm')
    if not npm:
        sys.exit('npm not found: build the UI with Node.js installed')
    ui = os.path.join(ROOT, 'ui')
    # npm writes this lock file last, so a half-finished install doesn't count.
    if not os.path.exists(os.path.join(ui, 'node_modules', '.package-lock.json')):
        subprocess.run([npm, 'ci', '--no-audit', '--no-fund'], cwd=ui, check=True)
    subprocess.run([npm, 'run', 'build'], cwd=ui, check=True)
    # Keep this incremental: forcing a rebuild here recompiles the whole engine on every installer build.
    subprocess.run([find_cmake(build_dir), '--build', build_dir, *config_args, '--target', 'DAWg'], check=True)


def bundle_ui(app_dir):
    packaged_ui = os.path.join(app_dir, 'ui')
    if os.path.isdir(packaged_ui):
        shutil.rmtree(packaged_ui)
    shutil.copytree(os.path.join(ROOT, 'ui', 'dist'), packaged_ui)
    verify_ui(app_dir)


def build_windows(build_dir, no_build):
    if not no_build:
        build_app(build_dir, ['--config', 'Release'])

    app_dir = os.path.join(build_dir, 'app', 'DAWg_artefacts', 'Release')
    exe = os.path.join(app_dir, 'DAWg.exe')
    if not os.path.exists(exe):
        sys.exit('No Release build at ' + app_dir)
    if not has_icon(exe):
        sys.exit(exe + ' has no icon. Configure the build again (cmake -S . -B build) and rebuild Release.')
    bundle_ui(app_dir)

    version = version_of_project()
    out_dir = os.path.join(build_dir, 'installer')
    run_inno_setup([find_iscc(), '/Q', '/DAppVersion=' + version, '/DAppDir=' + app_dir,
                    '/DRedistDir=' + find_redist(build_dir), '/DOutputDir=' + out_dir,
                    os.path.join(ROOT, 'installer', 'DAWg.iss')])
    print(os.path.join(out_dir, 'DAWg-%s-setup.exe' % version))


def linked_packages(exe):
    """The Debian packages holding the libraries an executable links, found with ldd and dpkg -S."""
    output = subprocess.run(['ldd', exe], capture_output=True, text=True, check=True).stdout
    missing = [line.split()[0] for line in output.splitlines() if 'not found' in line]
    if missing:
        sys.exit('Libraries not found for %s: %s' % (exe, ', '.join(missing)))

    packages = set()
    for path in re.findall(r'=> (/\S+)', output):
        real = os.path.realpath(path)
        for candidate in (path, real):
            found = subprocess.run(['dpkg', '-S', candidate], capture_output=True, text=True)
            if found.returncode == 0:
                packages.add(found.stdout.split(':')[0].strip())
                break
        else:
            sys.exit('No package holds %s, which %s links' % (path, exe))
    return sorted(packages)


def maintainer():
    """Whoever builds the package: Debian wants a name and an address, from DEBFULLNAME and DEBEMAIL as Debian's own
    tools take them, else from git."""
    def config(key):
        result = subprocess.run(['git', 'config', key], capture_output=True, text=True, cwd=ROOT)
        return result.stdout.strip()

    name = os.environ.get('DEBFULLNAME') or config('user.name')
    email = os.environ.get('DEBEMAIL') or config('user.email')
    if not name or not email:
        sys.exit('Set DEBFULLNAME and DEBEMAIL, or git user.name and user.email: the .deb names its maintainer')
    return '%s <%s>' % (name, email)


def copy_app(app_dir, dest):
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(app_dir, 'DAWg'), os.path.join(dest, 'DAWg'))
    for folder in ('ui', 'faust', 'midi-files', 'models', 'licenses'):
        shutil.copytree(os.path.join(app_dir, folder), os.path.join(dest, folder))
    shutil.copy2(os.path.join(ROOT, 'LICENSE'), os.path.join(dest, 'LICENSE'))


def normalise_modes(stage, executables):
    """Directories 0755, files 0644, the named executables 0755: whatever the files were where they came from (a
    Windows drive under WSL makes everything 0777)."""
    for folder, dirs, files in os.walk(stage):
        for name in dirs:
            os.chmod(os.path.join(folder, name), 0o755)
        for name in files:
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, stage).replace(os.sep, '/')
            os.chmod(path, 0o755 if relative in executables else 0o644)


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as file:
        file.write(text)
    os.chmod(path, mode)


def build_deb(app_dir, out_dir, version):
    arch = subprocess.run(['dpkg', '--print-architecture'], capture_output=True, text=True, check=True).stdout.strip()
    stage = os.path.join(out_dir, 'deb')
    shutil.rmtree(stage, ignore_errors=True)

    copy_app(app_dir, os.path.join(stage, 'opt', 'dawg'))
    write(os.path.join(stage, 'usr', 'bin', 'dawg'), LINUX_LAUNCHER, 0o755)
    write(os.path.join(stage, 'usr', 'share', 'applications', 'dawg.desktop'), LINUX_DESKTOP_ENTRY)
    icons = os.path.join(stage, 'usr', 'share', 'icons', 'hicolor')
    os.makedirs(os.path.join(icons, '256x256', 'apps'))
    os.makedirs(os.path.join(icons, 'scalable', 'apps'))
    shutil.copy2(os.path.join(ROOT, 'branding', 'dawg-icon-256.png'), os.path.join(icons, '256x256', 'apps', 'dawg.png'))
    shutil.copy2(os.path.join(ROOT, 'branding', 'dawg-icon.svg'), os.path.join(icons, 'scalable', 'apps', 'dawg.svg'))
    doc = os.path.join(stage, 'usr', 'share', 'doc', 'dawg')
    os.makedirs(doc)
    shutil.copy2(os.path.join(ROOT, 'LICENSE'), os.path.join(doc, 'copyright'))

    size_kb = sum(os.path.getsize(os.path.join(folder, name))
                  for folder, _, names in os.walk(stage) for name in names) // 1024
    depends = linked_packages(os.path.join(app_dir, 'DAWg')) + LINUX_LOADED_DEPENDS
    write(os.path.join(stage, 'DEBIAN', 'control'), '\n'.join([
        'Package: dawg',
        'Version: ' + version,
        'Architecture: ' + arch,
        'Maintainer: ' + maintainer(),
        'Installed-Size: %d' % size_kb,
        'Depends: ' + ', '.join(depends),
        'Recommends: ' + ', '.join(LINUX_RECOMMENDS),
        'Section: sound',
        'Priority: optional',
        'Homepage: https://github.com/OliverLeenders/DAWg',
        'Description: Digital audio workstation',
        ' DAWg is a DAW with a visual MPE piano roll, node-based effects routing and',
        ' scriptable effects in Faust. It hosts VST3 and CLAP plugins.',
    ]) + '\n')
    normalise_modes(stage, {'opt/dawg/DAWg', 'usr/bin/dawg'})

    package = os.path.join(out_dir, 'dawg_%s_%s.deb' % (version, arch))
    subprocess.run(['dpkg-deb', '--root-owner-group', '--build', stage, package], check=True,
                   stdout=subprocess.DEVNULL)
    shutil.rmtree(stage)
    return package


def build_tarball(app_dir, out_dir, version):
    name = 'DAWg-%s-linux-%s' % (version, os.uname().machine)
    stage = os.path.join(out_dir, name)
    shutil.rmtree(stage, ignore_errors=True)
    copy_app(app_dir, stage)
    write(os.path.join(stage, 'dawg.desktop'), LINUX_DESKTOP_ENTRY.replace('Exec=dawg', 'Exec=DAWg'))
    shutil.copy2(os.path.join(ROOT, 'branding', 'dawg-icon-256.png'), os.path.join(stage, 'dawg.png'))
    write(os.path.join(stage, 'README.txt'), """DAWg %s for Linux

Run ./DAWg from this folder; it finds ui/, faust/ and midi-files/ next to itself.

It needs WebKitGTK 4.1, GTK 3, ALSA, FreeType and fontconfig, and libcurl for update checks; JACK is used when it's
there. On Debian or Ubuntu:

    sudo apt install libwebkit2gtk-4.1-0 libasound2t64 libcurl4t64 libfreetype6 libfontconfig1

(libasound2 and libcurl4 before Ubuntu 24.04.) The .deb installs all of this by itself, with a menu entry.

To add DAWg to the menu by hand, put dawg.desktop in ~/.local/share/applications with Exec= set to this folder's
DAWg, and dawg.png in ~/.local/share/icons.
""" % version)

    normalise_modes(stage, {'DAWg'})
    archive = os.path.join(out_dir, name + '.tar.gz')
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(stage, arcname=name)
    shutil.rmtree(stage)
    return archive


def build_linux(build_dir, no_build):
    if not no_build:
        build_app(build_dir, [])

    app_dir = os.path.join(build_dir, 'app', 'DAWg_artefacts', 'Release')
    if not os.path.exists(os.path.join(app_dir, 'DAWg')):
        sys.exit('No Release build at %s (configure with -DCMAKE_BUILD_TYPE=Release)' % app_dir)
    bundle_ui(app_dir)

    if not shutil.which('dpkg-deb'):
        sys.exit('dpkg-deb not found: the .deb is built on Debian or Ubuntu')

    version = version_of_project()
    out_dir = os.path.join(build_dir, 'installer')
    os.makedirs(out_dir, exist_ok=True)
    print(build_deb(app_dir, out_dir, version))
    print(build_tarball(app_dir, out_dir, version))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--build-dir', default=os.path.join(ROOT, 'build'))
    parser.add_argument('--no-build', action='store_true', help='package the existing Release build as it is')
    args = parser.parse_args()
    build_dir = os.path.abspath(args.build_dir)

    if sys.platform == 'win32':
        build_windows(build_dir, args.no_build)
    elif sys.platform.startswith('linux'):
        build_linux(build_dir, args.no_build)
    else:
        sys.exit('No installer for this platform yet')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# fetch-fex.py <out>: what the aarch64 core runs the x86-64 games with, for
# system/atarivcs (src/core.c, find_fex):
#   <out>/fex         FEX and FEXServer (Ubuntu 22.04 arm64, from FEX's PPA),
#                     its GL thunks (the game's GL calls go to the host's
#                     driver), ThunksDB.json and thunks.json, which turns the
#                     GL thunk on
#   <out>/fex-rootfs  the x86-64 libraries the store's games expect of a
#                     desktop, from Debian 12 with their dependencies: glibc,
#                     libstdc++, SDL2, OpenAL, ALSA, PulseAudio's client, the
#                     X11 libraries and the GL dispatcher (glvnd) without Mesa
#                     - GL is the thunk's - and dash as /bin/sh for games
#                     started by a script
# Package versions come from the archives' indexes, so an update does not
# break the download. Needs dpkg-deb.
import gzip, os, re, shutil, subprocess, sys, tempfile, urllib.request

PPA = 'https://ppa.launchpadcontent.net/fex-emu/fex/ubuntu'
FEX_PACKAGE = 'fex-emu-armv8.0'  # the build for every ARMv8 CPU
DEBIAN = 'https://deb.debian.org/debian'
ROOTFS = ['libc6', 'libstdc++6', 'libgcc-s1', 'libsdl2-2.0-0', 'libopenal1',
          'libx11-6', 'libxext6', 'libxrandr2', 'libxcursor1', 'libxi6',
          'libxinerama1', 'libxss1', 'libxxf86vm1', 'libxkbcommon0',
          'libasound2', 'libgl1', 'libglvnd0', 'libglx0', 'libegl1',
          'libgbm1', 'libpulse0', 'libsndio7.0', 'dash']
# Not wanted: Mesa (GL is thunked to the host), and packages that only set
# up a real system
SKIP = {'debconf', 'debconf-2.0', 'perl-base', 'dpkg', 'libc-bin',
        'base-files', 'adduser', 'x11-common', 'libx11-data', 'libllvm15',
        'mesa-vulkan-drivers', 'libgl1-mesa-dri', 'libglx-mesa0',
        'libegl-mesa0', 'debianutils'}


def index(base, dist, comp, arch):
    url = f'{base}/dists/{dist}/{comp}/binary-{arch}/Packages.gz'
    text = gzip.decompress(urllib.request.urlopen(url, timeout=120).read()).decode('utf-8', 'replace')
    found = {}
    for block in text.split('\n\n'):
        fields = dict(re.findall(r'^([\w-]+): (.*)$', block, re.M))
        if 'Package' not in fields:
            continue
        found.setdefault(fields['Package'], fields)
        for provided in re.split(r',\s*', fields.get('Provides', '')):
            provided = provided.split(' ')[0]
            if provided:
                found.setdefault(provided, fields)
    return found


def extract(base, filename, dest):
    with tempfile.NamedTemporaryFile(suffix='.deb') as deb:
        deb.write(urllib.request.urlopen(f'{base}/{filename}', timeout=300).read())
        deb.flush()
        subprocess.run(['dpkg-deb', '-x', deb.name, dest], check=True)
    print('fetched', os.path.basename(filename))


out = sys.argv[1]
fex = f'{out}/fex'
os.makedirs(f'{fex}/HostThunks', exist_ok=True)
os.makedirs(f'{fex}/GuestThunks', exist_ok=True)
ppa = index(PPA, 'jammy', 'main', 'arm64')
with tempfile.TemporaryDirectory() as tmp:
    extract(PPA, ppa[FEX_PACKAGE]['Filename'], tmp)
    for name in ('FEX', 'FEXServer'):
        shutil.copy2(f'{tmp}/usr/bin/{name}', f'{fex}/{name}')
    shutil.copy2(f'{tmp}/usr/lib/aarch64-linux-gnu/fex-emu/HostThunks/libGL-host.so', f'{fex}/HostThunks/')
    shutil.copy2(f'{tmp}/usr/share/fex-emu/GuestThunks/libGL-guest.so', f'{fex}/GuestThunks/')
    shutil.copy2(f'{tmp}/usr/share/fex-emu/ThunksDB.json', f'{fex}/ThunksDB.json')
with open(f'{fex}/thunks.json', 'w') as f:
    f.write('{"ThunksDB":{"GL":1}}\n')

rootfs = f'{out}/fex-rootfs'
os.makedirs(rootfs, exist_ok=True)
debian = index(DEBIAN, 'bookworm', 'main', 'amd64')
done, todo, files = set(), list(ROOTFS), set()
while todo:
    name = todo.pop()
    if name in done or name in SKIP:
        continue
    done.add(name)
    fields = debian.get(name)
    if not fields:
        sys.exit(f'no package {name} in Debian 12')
    done.add(fields['Package'])
    files.add(fields['Filename'])
    depends = ', '.join(x for x in (fields.get('Pre-Depends'), fields.get('Depends')) if x)
    for dep in re.split(r',\s*', depends):
        if dep:
            todo.append(dep.split('|')[0].strip().split(' ')[0].split(':')[0])
for filename in sorted(files):
    extract(DEBIAN, filename, rootfs)
# dash's postinst makes /bin/sh, which extracting does not run
if not os.path.lexists(f'{rootfs}/bin/sh'):
    os.makedirs(f'{rootfs}/bin', exist_ok=True)
    os.symlink('dash' if os.path.exists(f'{rootfs}/bin/dash') else '../usr/bin/dash', f'{rootfs}/bin/sh')
for doc in ('usr/share/doc', 'usr/share/man', 'usr/share/locale', 'usr/share/lintian', 'usr/share/info'):
    shutil.rmtree(f'{rootfs}/{doc}', ignore_errors=True)

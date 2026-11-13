#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py MOUNTED_ROOT [--no-bbd] [--net]
  sweet-root-tweaks.py --launchd-plist IN OUT [--net]
  sweet-root-tweaks.py --job-env LABEL KEY VALUE IN OUT   (empty VALUE removes KEY)

The second form only applies the launchd.plist changes to a copy of the file
(IN: /System/Library/xpc/launchd.plist from the IPSW or the phone), for
installing it through SWEETBOX as ROOT/System/Library/xpc/launchd.plist
(docs/guacamole.md, "Autorun").

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
turned the plain mkiosroot.sh output into the images that reach the home
screen (image versions v8-v14 in NOTES.md):

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); CommCenter jobs removed (no baseband: it
                  crash-loops and Setup blocks on its CoreTelephony XPC);
                  com.apple.fairplayd.H2 removed (no FairPlay kext: it exits
                  and KeepAlive restarts it, while Photos' checkpointd, passd
                  and Settings wait forever for its replies; with the job gone
                  their lookups fail at once);
                  job com.sweet.autorun runs /usr/libexec/sweet-autorun
                  (userland/autorun) at boot; com.apple.backboardd runs
                  /usr/libexec/sweet-bbd (userland/bbd), which execs the real
                  backboardd with our library in DYLD_INSERT_LIBRARIES;
                  guacamole: com.sweet.wland (Wi-Fi, /var/local/bin/wland up 0),
                  com.sweet.btd (Bluetooth, userland/bt/btd.c; bluetoothd talks
                  H4 to the kernel control com.sweet.bt and com.apple.BlueTool
                  runs /var/local/bin/bluetoold), com.sweet.adspd (speaker
                  audio: /var/local/bin/adspd serve, script
                  /var/local/adspd.boot) and com.sweet.sshd (the rootless
                  Procursus bootstrap's sshd in /var/jb), all RunAtLoad, so
                  Wi-Fi, Bluetooth, sound and SSH come up on every boot
                  without a SWEETBOX autorun.sh
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark seeded; the actual Activated state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py
"""
import glob, os, plistlib, shutil, sys

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


AUTORUN_KEY = '/System/Library/LaunchDaemons/com.sweet.autorun.plist'
AUTORUN_JOB = {
    'Label': 'com.sweet.autorun',
    'ProgramArguments': ['/usr/libexec/sweet-autorun'],
    'RunAtLoad': True,
    'POSIXSpawnType': 'Interactive',
}


# guacamole: Wi-Fi (userland/wland, copied to /var/local/bin) and SSH (Procursus bootstrap in
# /var/jb) at every boot. wland keeps itself running ("up 0"); sshd is restarted if it exits.
NET_JOBS = {
    '/System/Library/LaunchDaemons/com.sweet.wland.plist': {
        'Label': 'com.sweet.wland',
        'ProgramArguments': ['/var/local/bin/wland', 'up', '0'],
        'RunAtLoad': True,
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/var/local/wland.out',
        'StandardErrorPath': '/var/local/wland.out',
    },
    # 0202: each container's signing identity -> its path, for the kernel's Sandbox stand-in (extensions need it)
    '/System/Library/LaunchDaemons/com.sweet.containers.plist': {
        'Label': 'com.sweet.containers',
        'ProgramArguments': ['/var/local/bin/sweet-containers'],
        'RunAtLoad': True,
        'KeepAlive': True,
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/var/local/containers.out',
        'StandardErrorPath': '/var/local/containers.out',
    },
    # brightness slider / Night Shift through the display controller's colour stage (userland/jb/sweet-displayd.m)
    '/System/Library/LaunchDaemons/com.sweet.displayd.plist': {
        'Label': 'com.sweet.displayd',
        'ProgramArguments': ['/var/local/bin/sweet-displayd'],
        'RunAtLoad': True,
        'KeepAlive': True,
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/var/local/displayd.out',
        'StandardErrorPath': '/var/local/displayd.out',
    },
    # Bluetooth (userland/bt/btd.c, from OtVaL228/ios-sweet guacamole): btd powers the controller, loads its
    # firmware from /var/local/fw/bt and serves it to bluetoothd through the kernel control com.sweet.bt (xnu
    # 0162 equivalent, boot-arg sweetbt=1); it retries until wland has the rails up.
    '/System/Library/LaunchDaemons/com.sweet.btd.plist': {
        'Label': 'com.sweet.btd',
        'ProgramArguments': ['/var/local/bin/btd', 'serve'],
        'RunAtLoad': True,
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/var/local/btd.out',
        'StandardErrorPath': '/var/local/btd.out',
    },
    # Speaker audio (userland/adsp): adspd boots the ADSP once per phone boot (a restart takes the modem and
    # Wi-Fi down), after ADSPD_BOOT seconds, then runs /var/local/adspd.boot (smmu, afe on, tfa init, pcm on)
    # and keeps serving. No KeepAlive, for that reason.
    '/System/Library/LaunchDaemons/com.sweet.adspd.plist': {
        'Label': 'com.sweet.adspd',
        'ProgramArguments': ['/var/local/bin/adspd', 'serve'],
        'EnvironmentVariables': {'ADSPD_BOOT': '30'},
        # the default daemon limit (6 MB) jetsams adspd while it loads the 13 MB ADSP firmware; launchd does
        # not apply this for our job (still 6 MB), adspd raises its own limit (memorystatus_control)
        'JetsamProperties': {'JetsamMemoryLimit': 128},
        'RunAtLoad': True,
        'POSIXSpawnType': 'Interactive',
        'StandardOutPath': '/var/local/adspd.out',
        'StandardErrorPath': '/var/local/adspd.out',
    },
    '/System/Library/LaunchDaemons/com.sweet.sshd.plist': {
        'Label': 'com.sweet.sshd',
        'ProgramArguments': ['/var/jb/usr/sbin/sshd', '-D'],
        'RunAtLoad': True,
        'KeepAlive': True,
        'POSIXSpawnType': 'Interactive',
    },
}

# bluetoothd: H4 over its "POSIX" driver, which is a kernel control socket (PF_SYSTEM) by name, not a tty:
# com.sweet.bt relays it to btd. The address comes from the ADT bluetooth node (tools/mkadt.py). BlueTool
# (Broadcom/Apple chip scripts; nothing for our ProductHash) is replaced by userland/bt/bluetoold, which
# answers bluetoothd's RunBuiltinScript requests ("boot", "init") with result 0.
BLUETOOTHD_ENV = {'BT_HCI_TRANSPORT': 'H4:POSIX:com.sweet.bt'}
BLUETOOL_STANDIN = ['/var/local/bin/bluetoold']

BBD_WRAPPER = '/usr/libexec/sweet-bbd'
REMOVED_JOBS = ('com.apple.CommCenter', 'com.apple.fairplayd.H2')


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    launchd_plist(path, path)


def launchd_plist(src, dst):
    """launchd.plist edits; dst keeps src's owner and mode if it exists."""
    st = os.stat(dst if os.path.exists(dst) else src)
    plist = load(src)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') in REMOVED_JOBS]
    for k in removed:
        del jobs[k]
    jobs[AUTORUN_KEY] = dict(AUTORUN_JOB)
    if '--net' in sys.argv:
        for k, j in NET_JOBS.items():
            jobs[k] = dict(j)
        for job in jobs.values():
            if job.get('Label') == 'com.apple.bluetoothd':
                job.setdefault('EnvironmentVariables', {}).update(BLUETOOTHD_ENV)
            if job.get('Label') == 'com.apple.BlueTool':
                job['ProgramArguments'] = list(BLUETOOL_STANDIN)
    bbd = BBD_WRAPPER
    if '--no-bbd' in sys.argv:
        # A root built from the IPSW has no userland yet: /usr/libexec/sweet-bbd and
        # libsweet-bbd.dylib are installed later through SWEETBOX. Until then the job must
        # point at the real binary, or launchd cannot start the window server at all.
        bbd = '/usr/libexec/backboardd'
    for job in jobs.values():
        if job.get('Label') == 'com.apple.backboardd':
            job['ProgramArguments'] = [bbd]
    save(dst, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, {len(removed)} jobs removed ({", ".join(REMOVED_JOBS)}), '
          f'{AUTORUN_JOB["Label"]} added, backboardd via {bbd}')


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    save(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('data ark', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


def add_job(original, label, src, dst):
    """
    Copy the job LABEL from ORIGINAL (the IPSW's unmodified launchd.plist) into SRC (the phone's current
    launchd.plist), _PanicOnCrash renamed as everywhere else, and write DST (owner/mode of DST if it exists, else SRC).
    2026-11-10: Settings hides Privacy > Location Services when CoreTelephony's subscription query fails
    (PUIPrivacyController refreshLocationServicesAvailable: com.apple.commcenter.coretelephony.xpc "No such process"),
    i.e. without CommCenter. Its old crash (objc block trampolines, 0044) and Setup's wait on it (Setup is skipped
    since v14) are gone, so it is tried again at runtime this way before REMOVED_JOBS changes.
    """
    orig = load(original)['LaunchDaemons']
    found = [(k, j) for k, j in orig.items() if j.get('Label') == label]
    if not found:
        sys.exit(f'{label}: not in {original}')
    st = os.stat(dst if os.path.exists(dst) else src)
    plist = load(src)
    for k, j in found:
        j = dict(j)
        if '_PanicOnCrash' in j:
            j['_PanicOnCrasX'] = j.pop('_PanicOnCrash')
        plist['LaunchDaemons'][k] = j
        print(f'launchd.plist: {label} added as {k}')
    save(dst, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)


def job_env(label, key, value, src, dst):
    """
    Set (or, with an empty VALUE, remove) one EnvironmentVariables entry of launchd job LABEL in SRC, write DST.
    2026-11-10: DYLD_INSERT_LIBRARIES=/usr/lib/libsweet-autrace.dylib on com.apple.mediaserverd traces the CoreAudio
    property calls VirtualAudio makes (userland/jb/autrace.c).
    """
    st = os.stat(dst if os.path.exists(dst) else src)
    plist = load(src)
    found = [j for j in plist['LaunchDaemons'].values() if j.get('Label') == label]
    if not found:
        sys.exit(f'{label}: not in {src}')
    for j in found:
        env = j.setdefault('EnvironmentVariables', {})
        if value:
            env[key] = value
        else:
            env.pop(key, None)
            if not env:
                del j['EnvironmentVariables']
        print(f'launchd.plist: {label} EnvironmentVariables = {j.get("EnvironmentVariables", {})}')
    save(dst, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)


def main():
    if len(sys.argv) == 7 and sys.argv[1] == '--job-env':
        job_env(*sys.argv[2:7])
        return
    if len(sys.argv) == 6 and sys.argv[1] == '--add-job':
        add_job(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
        return
    if len(sys.argv) in (4, 5) and sys.argv[1] == '--launchd-plist':
        launchd_plist(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) < 2 or not os.path.isdir(os.path.join(sys.argv[1], 'System/Library/xpc')):
        sys.exit(__doc__)
    root = sys.argv[1]
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()

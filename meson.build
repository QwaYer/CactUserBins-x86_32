# ==============================================================================
# CactUserBins — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dcactlib=../CactLibc-x86_32 -Dcactsoleinc=../Cactsole-x86_32/include
#   ninja -C build-meson            # all tools
#   ninja -C build-meson stage      # distribute into -Dlr_bin / -Dlr_sbin
#   ninja -C build-meson host-fdisk host-mkfs.ext4 host-mkfs.fat32 host-cact-rootfs
#
# This repo is a flat set of one-file tools rather than a source tree, so the
# tables below mirror the Makefile's APPS/SBIN_APPS/DISK_EXTRA lists and a single
# foreach creates one PIE per tool.  Every tool links CactLibc plus the shared
# common/ objects; the three disk/fs tools link their own format object instead,
# exactly as the Makefile's dedicated link lines did.
# ==============================================================================

project('cact-userbins', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',        # -Wall -Wextra, as in the Makefile
    'buildtype=plain',
    'b_staticpic=false',
  ])

fs = import('fs')
bash = find_program('bash')
ld = find_program('ld')

src_root = meson.project_source_root()

# Path options are relative to this repo's root unless absolute.
_cactlib = get_option('cactlib')
cactlib = _cactlib.startswith('/') ? _cactlib : src_root / _cactlib
_soleinc = get_option('cactsoleinc')
cactsoleinc = _soleinc.startswith('/') ? _soleinc : src_root / _soleinc
_lr_bin = get_option('lr_bin')
lr_bin = _lr_bin.startswith('/') ? _lr_bin : src_root / _lr_bin
_lr_sbin = get_option('lr_sbin')
lr_sbin = _lr_sbin.startswith('/') ? _lr_sbin : src_root / _lr_sbin
_drmuapi = get_option('drmuapi')
drmuapi = _drmuapi.startswith('/') ? _drmuapi : src_root / _drmuapi

start_o = cactlib / 'build-meson' / 'start.o'
clibc_so = cactlib / 'build-meson' / 'clibc.so'

if not fs.exists(start_o) or not fs.exists(clibc_so)
  error('CactLibc is not built: expected ' + start_o +
        '  —  build it first:  ninja -C ' + cactlib / 'build-meson')
endif
if not fs.is_dir(cactsoleinc)
  error('Cactsole headers not found at ' + cactsoleinc +
        '  —  build Cactsole first (its include/ ships shell.h)')
endif
if not fs.is_dir(drmuapi)
  error('DRM uapi headers not found at ' + drmuapi)
endif

# Every object in this repo is position independent: the tools are PIEs.
add_project_arguments('-fPIE', language: 'c')

c_args = [
  '-ffreestanding',
  '-fno-stack-protector',
  '-nostdlib',
  '-ffunction-sections',
  '-fdata-sections',
  '-DCACTOS_TARGET',
  '-I' + src_root,
  '-I' + cactsoleinc,
  '-I' + (cactlib / 'include'),
]

# The Makefile's APPS list.
apps = [
  'pwd', 'ls', 'mkdir', 'rmdir', 'rm', 'cat', 'wrt', 'stat', 'mv', 'ln',
  'readlink', 'ldd', 'clear', 'date', 'uptime', 'kill', 'su', 'sleep', 'free', 'sysinfo',
  'modload', 'modunload', 'run', 'echo', 'true', 'false', 'whoami', 'id',
  'chmod', 'chown', 'version', 'ip', 'ping', 'wget', 'dd', 'df', 'grep', 'fdisk', 'mkfs.ext4',
  'mkfs.fat32', 'cact-rootfs', 'poweroff', 'reboot', 'halt', 'suspend',
  'head', 'tail', 'wc', 'sort', 'uniq', 'cut', 'tr', 'tee', 'seq', 'yes', 'printf',
  'cp', 'touch',
  'basename', 'dirname', 'realpath', 'which', 'mktemp', 'tty',
  'uname', 'hostname', 'nproc', 'sync', 'env', 'printenv', 'du', 'find',
  'ps', 'lsmod', 'lspci', 'lsusb', 'dmesg', 'sha256sum', 'sha384sum', 'mount', 'umount',
  'md5sum', 'sha1sum',
]

# The Makefile's SBIN_APPS list.
sbin_apps = [
  'kill', 'su', 'modload', 'modunload', 'ping', 'ip', 'dd', 'df',
  'fdisk', 'mkfs.ext4', 'mkfs.fat32', 'cact-rootfs',
  'poweroff', 'reboot', 'halt', 'suspend',
  'mount', 'umount',
]

# The Makefile's dedicated link lines: no common/ objects, one extra object.
disk_tools = {
  'fdisk': 'ptab.c',
  'mkfs.ext4': 'ext4_fmt.c',
  'mkfs.fat32': 'fat32_fmt.c',
}

common_lib = static_library('common',
  files(
    'common/ex_copy.c',
    'common/ex_dd.c',
    'common/ex_df.c',
    'common/ex_echo.c',
    'common/ex_files.c',
    'common/ex_grep.c',
    'common/ex_hash.c',
    'common/ex_ldd.c',
    'common/ex_misc.c',
    'common/ex_nav.c',
    'common/ex_net.c',
    'common/ex_path.c',
    'common/ex_proc.c',
    'common/ex_sys.c',
    'common/ex_system.c',
    'common/ex_text.c',
    'common/ex_util.c',
  ),
  # ex_sys.c reads the current KMS mode for sysinfo, so the shared objects
  # compile against the DRM uapi headers.
  c_args: c_args + ['-I' + drmuapi])

app_bins = []
foreach app : apps
  sources = files(app / 'main.c')
  use_common = true
  if disk_tools.has_key(app)
    sources += files(app / disk_tools[app])
    use_common = false
  endif

  app_lib = static_library('app-' + app.underscorify(), sources, c_args: c_args)

  link_cmd = [ld, '-m', 'elf_i386', '-pie', '--dynamic-linker=/lib/ld.so',
              '--hash-style=both', '-nostdlib', '--gc-sections',
              '-T', src_root / 'link.ld', '-o', '@OUTPUT@',
              start_o,
              '--whole-archive', app_lib]
  if use_common
    link_cmd += common_lib
  endif
  link_cmd += ['--no-whole-archive', clibc_so]

  app_bins += custom_target('bin-' + app.underscorify(),
    output: app,
    input: sources,
    command: link_cmd,
    depend_files: files('link.ld'),
    build_by_default: true)
endforeach

# Meson reserves the names "install" and "test" for itself, hence "stage".
# bin tools go to lr_bin, the sbin set to lr_sbin (removing any stale copy of
# the latter from lr_bin first, as the Makefile's install rule did).
_sbin_match = ' ' + ' '.join(sbin_apps) + ' '
_stage_script = (
  'set -e; bindir="$1"; sbindir="$2"; shift 2; ' +
  'mkdir -p "$bindir" "$sbindir"; ' +
  'for f in "$@"; do n="$(basename "$f")"; ' +
  'case "' + _sbin_match + '" in ' +
  '*" $n "*) rm -f "$bindir/$n"; cp -f "$f" "$sbindir/$n" ;; ' +
  '*) cp -f "$f" "$bindir/$n" ;; esac; done'
)

run_target('stage',
  command: [bash, '-c', _stage_script, 'bash', lr_bin, lr_sbin] + app_bins)

# ------------------------------------------------------------------------------
# Host-side disk/fs tools (the Makefile's HOST_TOOLS)
#
# Plain build-machine binaries against the host libc, used to prepare test
# images.  The Makefile's `test` target ran tests/run_tests.sh, which does not
# exist in this checkout, so only the tools are built here.
# ------------------------------------------------------------------------------

host_c_args = ['-m32', '-O2', '-Wall', '-Wno-unused-function', '-Wno-unused-variable']
# The objects are 32-bit (-m32 in host_c_args), so the link step needs -m32 too
# or the native 64-bit linker rejects them.
host_link_args = ['-m32']

executable('host-fdisk',
  files('fdisk/main.c', 'fdisk/ptab.c'),
  native: true, c_args: host_c_args, link_args: host_link_args,
  build_by_default: false)

executable('host-mkfs.ext4',
  files('mkfs.ext4/main.c', 'mkfs.ext4/ext4_fmt.c'),
  native: true, c_args: host_c_args, link_args: host_link_args,
  build_by_default: false)

executable('host-mkfs.fat32',
  files('mkfs.fat32/main.c', 'mkfs.fat32/fat32_fmt.c'),
  native: true, c_args: host_c_args, link_args: host_link_args,
  build_by_default: false)

executable('host-cact-rootfs',
  files('cact-rootfs/main.c'),
  native: true, c_args: host_c_args, link_args: host_link_args,
  build_by_default: false)

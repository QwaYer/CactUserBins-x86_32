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
_zstd = get_option('zstd_dir')
zstd_dir = _zstd.startswith('/') ? _zstd : src_root / _zstd

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
  'chmod', 'chown', 'version', 'ip', 'ping', 'arp', 'wget', 'dd', 'df', 'grep', 'fdisk', 'mkfs.ext4',
  'mkfs.fat32', 'cact-rootfs', 'poweroff', 'reboot', 'halt',
  'head', 'tail', 'wc', 'sort', 'uniq', 'cut', 'tr', 'tee', 'seq', 'yes', 'printf',
  'cp', 'touch',
  'basename', 'dirname', 'realpath', 'which', 'mktemp', 'tty',
  'uname', 'hostname', 'nproc', 'sync', 'env', 'printenv', 'du', 'find',
  'ps', 'lsmod', 'lspci', 'lsusb', 'dmesg', 'sha256sum', 'sha384sum', 'mount', 'umount',
  'md5sum', 'sha1sum', 'wljoin', 'ced',
  'od', 'hexdump', 'strings', 'base64', 'cmp', 'diff', 'nl', 'rev', 'file',
  'tac', 'shuf', 'split', 'fold', 'paste',
  'test', 'expr', 'factor',
  'xargs', 'timeout', 'pidof', 'killall', 'lscpu', 'mountpoint', 'truncate',
  'more', 'cal', 'tree', 'tar', 'nc', 'dns', 'firmextr',
  # cactsole bridge tools: they send requests to the shell over $CACT_CTL_FD
  # (see common/ex_shell.c) instead of being shell builtins.
  'cd', 'export', 'unset', 'exit', 'jobs', 'fg', 'bg', 'help',
]

# The Makefile's SBIN_APPS list.
sbin_apps = [
  'kill', 'su', 'modload', 'modunload', 'ping', 'ip', 'arp', 'dd', 'df',
  'fdisk', 'mkfs.ext4', 'mkfs.fat32', 'cact-rootfs',
  'poweroff', 'reboot', 'halt',
  'mount', 'umount', 'wljoin',
]

# Tools that link no common/ objects and carry one extra object of their own.
standalone_tools = {
  'fdisk': 'ptab.c',
  'mkfs.ext4': 'ext4_fmt.c',
  'mkfs.fat32': 'fat32_fmt.c',
  'wljoin': 'wpa.c',
  'ced': 'editor.c',
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
    'common/ex_fmt.c',
    'common/ex_stream.c',
    'common/ex_math.c',
    'common/ex_admin.c',
    'common/ex_view.c',
    'common/ex_archive.c',
    'common/ex_shell.c',
  ),
  # ex_sys.c reads the current KMS mode for sysinfo, so the shared objects
  # compile against the DRM uapi headers.
  c_args: c_args + ['-I' + drmuapi])

# firmextr streams the compressed localrepo archive, so it links the zstd
# decompressor vendored in the kernel tree (same sources, -w: third-party code).
zstd_lib = static_library('cact-zstd-user',
  files(
    zstd_dir / 'common/debug.c',
    zstd_dir / 'common/entropy_common.c',
    zstd_dir / 'common/error_private.c',
    zstd_dir / 'common/fse_decompress.c',
    zstd_dir / 'common/zstd_common.c',
    zstd_dir / 'decompress/huf_decompress.c',
    zstd_dir / 'decompress/zstd_ddict.c',
    zstd_dir / 'decompress/zstd_decompress.c',
    zstd_dir / 'decompress/zstd_decompress_block.c',
    zstd_dir / 'xxhash.c',
  ),
  c_args: c_args + ['-I' + zstd_dir, '-w'])

app_bins = []
foreach app : apps
  sources = files(app / 'main.c')
  use_common = true
  app_c_args = c_args
  extra_libs = []
  if app == 'firmextr'
    sources += files('firmextr/firmextr.c')
    app_c_args += ['-I' + zstd_dir]
    extra_libs += [zstd_lib]
  endif
  if standalone_tools.has_key(app)
    sources += files(app / standalone_tools[app])
    use_common = false
  endif

  # Meson/ninja reserve the target name "test", so the binary is built as
  # cact-test and the stage step installs it under its real name.
  outname = (app == 'test') ? 'cact-test' : app

  app_lib = static_library('app-' + app.underscorify(), sources, c_args: app_c_args)

  link_cmd = [ld, '-m', 'elf_i386', '-pie', '--dynamic-linker=/lib/ld.so',
              '--hash-style=both', '-nostdlib', '--gc-sections',
              '-T', src_root / 'link.ld', '-o', '@OUTPUT@',
              start_o,
              '--whole-archive', app_lib]
  if use_common
    link_cmd += common_lib
  endif
  link_cmd += extra_libs
  link_cmd += ['--no-whole-archive', clibc_so]

  app_bins += custom_target('bin-' + app.underscorify(),
    output: outname,
    input: sources,
    command: link_cmd,
    # start.o and clibc.so are linked in, so a rebuilt CactLibc must relink every
    # tool; without them here ninja keeps the old binary and the change silently
    # does not take effect.
    depend_files: files('link.ld') + [start_o, clibc_so],
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
  'if [ "$n" = "cact-test" ]; then n=test; fi; ' +
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

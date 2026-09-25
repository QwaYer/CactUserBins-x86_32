# 📦 CactUserBins/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C-orange.svg?style=for-the-badge" alt="Language: C">
  <img src="https://img.shields.io/badge/link-PIE%20%2B%20clibc.so-purple.svg?style=for-the-badge" alt="PIE + clibc.so">
  <img src="https://img.shields.io/badge/layout-one%20ELF%20per%20tool-blue.svg?style=for-the-badge" alt="One ELF per tool">
  <img src="https://img.shields.io/badge/tools-83-green.svg?style=for-the-badge" alt="83 tools">
  <img src="https://img.shields.io/badge/install-LocalRepoCactOS-0369a1.svg?style=for-the-badge" alt="install → LocalRepoCactOS">
</p>

<p align="center">
  A <strong>userspace tool suite</strong> for <strong>Cact OS</strong>: one <strong>small PIE ELF</strong> per command, shared implementations in <strong><code>common/</code></strong>, linked with <strong><code>clibc.so</code></strong> from <strong>CactLib</strong>.<br>
  Built artefacts land under <strong><code>build-meson/</code></strong>; <strong><code>ninja -C build-meson stage</code></strong> distributes them into <strong><code>LocalRepoCactOS-x86_32/lib/bin</code></strong> and <strong><code>lib/sbin</code></strong> for <strong>cctkfs</strong> packing.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Utilities** | **83** standalone programs (see [`meson.build`](meson.build) `apps`) |
| **`/bin` vs `/sbin`** | **`lr_bin`** / **`lr_sbin`** options used by the **`stage`** target (staging dirs under **LocalRepo**) |
| **Shared objects** | **`common/ex_*.c`** compiled once; each link pulls **`start.o`** + **one** `*/main.o` + all **`common/*.o`** with **`--gc-sections`** so unused entrypoints are dropped |
| **Load address** | PIE **ET_DYN** at **`0x08000000`** ([`link.ld`](link.ld)) — same family as **cactsole** / **cgoct** |
| **Headers** | **`CACTSOLEINC`** — path to **Cactsole** `include/` (set by **CactOS** or manually) |

---

## 🔗 Ecosystem

| Component | Role |
|-----------|------|
| **[CactLib-x86_32](https://github.com/QwaYer/CactLibc-x86_32)** | **`clibc.so`** + **`build-meson/start.o`** — required for every link line |
| **[Cactsole-x86_32](https://github.com/QwaYer/Cactsole-x86_32)** | Interactive shell; it does **not** describe these tools — **`help <tool>`** runs the tool with **`--help`** and each tool documents itself |
| **[LocalRepoCactOS-x86_32](../LocalRepoCactOS-x86_32)** | **`ninja -C build-meson stage`** fills **`lib/bin/`** and **`lib/sbin/`** here before **`cctkfs.img`** is packed |
| **[CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)** | **Workspace integrator** — sets **`CACTLIB`**, **`CACTSOLEINC`**, **`LR_*`**, then **`LocalRepo`** + **kernel** + **CactBridge** |
| **[CactKernel-x86_32](https://github.com/QwaYer/CactKernel-x86_32)** | **binfs** / **sbinfs** overlay **`/bin/*`** and **`/sbin/*`** from the **cctkfs** module on top of disk-backed FS |

---

## 🔨 Building

**Recommended — full workspace**

**[CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)** configures this project with **`-Dcactlib`**, **`-Dcactsoleinc`**, **`-Dlr_bin`**, **`-Dlr_sbin`** and runs its **`stage`** target.

**Standalone — this repository**

```sh
meson setup build-meson --cross-file cross/i686-cact-clang.ini \
    -Dcactlib=../CactLibc-x86_32 -Dcactsoleinc=../Cactsole-x86_32/include
ninja -C build-meson        # every tool
ninja -C build-meson stage  # copy ELFs into -Dlr_bin / -Dlr_sbin
ninja -C build-meson clean
```

Override any path if needed: `meson configure build-meson -Dcactlib=/custom/path`.

---

## 📂 Repository layout

```
CactUserBins-x86_32/
├── meson.build           # apps list, bin/sbin split, stage target
├── link.ld               # PIE @ 0x08000000
├── common/
│   ├── ex_files.c        # ls, mkdir, rmdir, rm, cat, wrt, stat, mv, ln, readlink
│   ├── ex_misc.c         # true, false, whoami, id, chmod, chown, version
│   ├── ex_net.c          # ping, ip, wget (+ shared socket/DNS helpers)
│   ├── ex_dd.c           # dd
│   ├── ex_df.c           # df
│   ├── ex_grep.c         # grep
│   ├── ex_ldd.c          # ldd (ELF32 DT_NEEDED reader, GNU-compatible flags)
│   ├── ex_sys.c          # clear, date, uptime, kill, su, sleep, free, sysinfo, run, modload, modunload
│   ├── ex_echo.c         # echo (with -n / -e)
│   ├── ex_nav.c          # pwd (cd stays a cactsole builtin)
│   ├── ex_copy.c         # cp, touch
│   ├── ex_path.c         # basename, dirname, realpath, which, mktemp, tty
│   ├── ex_system.c       # uname, hostname, nproc, sync, env, printenv, du, find
│   ├── ex_text.c         # head, tail, wc, sort, uniq, cut, tr, tee, seq, yes, printf
│   ├── ex_proc.c         # ps, mount, umount, lsmod, lspci, dmesg, sha256sum, sha384sum
│   ├── ex_util.c/.h      # shared helpers (read_all, dir_foreach, mkdir -p, copy_file, ...)
├── build-meson/          # generated ELFs (gitignored)
├── cat/ ls/ …/           # one directory per utility; each holds main.c → main.o
├── fdisk/                # parted-analog: ptab.c (pure) + main.c
├── mkfs.ext4/            # ext4 formatter: ext4_fmt.c (pure) + main.c
├── mkfs.fat32/           # FAT32 formatter: fat32_fmt.c (pure) + main.c
├── cact-rootfs/          # root skeleton + boot/ deploy
├── devtest/              # /dev VT + PTY self-test helper
├── tests/                # (empty in this checkout — the old host test target is gone)
└── README.md
```

**`apps`** (authoritative list in [`meson.build`](meson.build)):  
`pwd` `ls` `mkdir` `rmdir` `rm` `cat` `wrt` `stat` `mv` `ln` `readlink` `ldd` `clear` `date` `uptime` `kill` `su` `sleep` `free` `sysinfo` `modload` `modunload` `run` `echo` `true` `false` `whoami` `id` `chmod` `chown` `version` `ip` `ping` `wget` `dd` `df` `grep` `fdisk` `mkfs.ext4` `mkfs.fat32` `cact-rootfs` `poweroff` `reboot` `halt` `suspend` `head` `tail` `wc` `sort` `uniq` `cut` `tr` `tee` `seq` `yes` `printf` `cp` `touch` `basename` `dirname` `realpath` `which` `mktemp` `tty` `uname` `hostname` `nproc` `sync` `env` `printenv` `du` `find` `ps` `lsmod` `lspci` `lsusb` `dmesg` `sha256sum` `sha384sum` `md5sum` `sha1sum` `mount` `umount`

---

## 🧩 Design notes

| Topic | Detail |
|-------|--------|
| **Why one ELF per tool** | Smaller individual binaries than a busybox-style monolith; **`--gc-sections`** keeps only the **`main`** and **`cact_ub_*`** paths each `main.c` calls |
| **Self-documenting tools** | Every tool handles **`--help`** as its first argument: it prints its own usage to **stdout** and exits **0**. Usage text is defined once per tool and shared with its argument-error path, so the two cannot drift. This is what **cactsole**'s **`help <tool>`** relies on — the shell stores no description of these programs. Only the long form **`--help`** is a help flag; **`-h`** is not, because it already means **`--human-readable`** for **`df`**. |
| **FHS-style paths** | **`sbinfs`** exposes **`/sbin/*`** for privileged-style tools (`kill`, `su`, PCI **`modload`** / **`modunload`**, **`ping`**, **`ip`**, the power tools, disk/fs tools) |
| **Syscall drift** | If **`syscall.h`** / libc numbers change in CactLib, rebuild **libc**, then **re-link** cactsole, **CactUserBins**, cgoct, and any other dynamic ELFs |

---

## 💽 Disk / filesystem utilities

Installer-building tools — the CactOS analogues of `parted`, `mkfs.ext4`,
`mkfs.fat` and a rootfs deploy step.  All are installed into **`/sbin`**.

| Tool | Purpose |
|------|---------|
| **`fdisk`** | MBR/GPT partition-table editor. Reads/writes `/dev/<disk>` nodes (whole-disk CactOS block devices) or raw images; after `w` asks the kernel to re-scan the disk (`/dev/sys` ioctl) so `/dev/sdaN` appears without a reboot. Verbs: `p o g n d t b l w`; pure logic in `fdisk/ptab.c`. |
| **`mkfs.ext4`** | Formats a device/partition/image as ext4 compatible with the CactOS `ext4.cctk` module: 4096-byte blocks, no journal / 64-bit / metadata_csum / flex_bg, inode size ≥ 256. `-b`, `-I`, `-L`. |
| **`mkfs.fat32`** | Formats a device as a standard FAT32 volume (`-n label`); mirrors the FAT and writes FSInfo + backup boot sector. |
| **`cact-rootfs`** | Deploys the root skeleton onto a mounted target: `boot/` (kernel + `cctkfs.img` + generated `grub.cfg`), `/etc`, `/var/{log,run,tmp}`, and optionally copies `/bin /sbin /lib` from the running tree (`-b`). |

Format logic lives in portable C (`fdisk/ptab.c`, `mkfs.ext4/ext4_fmt.c`,
`mkfs.fat32/fat32_fmt.c`) so it can be validated on the host:

```sh
ninja -C build-meson host-fdisk host-mkfs.ext4 host-mkfs.fat32 host-cact-rootfs
```

The host builders compile natively for the build machine, so their output can be
validated offline: partition tables with `fdisk -l`, ext4 with `e2fsck -fn`,
FAT32 with `fsck.fat`/mtools, and the rootfs skeleton by deploying a tree.

---

## 🛰️ Network / block / text utilities

| Tool | Purpose |
|------|---------|
| **`ping`** | ICMP echo client: `ping [-c COUNT] [-i SEC] [-W MS] HOST` (`-c 0` runs until Ctrl-C). Accepts a dotted IPv4 literal or a hostname (resolved through DNS); each reply is matched by id/seq and timed via **`CACT_NETCTL_PING_WAIT`**, and a summary line closes the run. |
| **`ip`** | Link configuration: reads/writes the kernel's IPv4 address/mask/gateway/DNS through **`CACT_NETCTL_NETCFG(_GET)`** on `/dev/net` — the same path **networkd**/**dhcpd** use. |
| **`wget`** | Micro HTTP/1.1 GET client: `wget [-o FILE] http[s]://HOST[:PORT][/PATH]`. Resolves host names and speaks HTTPS through the libc **TLS 1.3** client (certificate chain verified against the system CA bundle); handles `Content-Length`, chunked bodies, close-delimited responses and `Location` redirects; the body lands in a file (default: basename of the path). Used by **cactpkg** to fetch manifests. |
| **`dd`** | Block copier (`if=`/`of=`, `bs=`, `count=`, `skip=`, `seek=`, `conv=notrunc`, `status=none`). `dd if=/dev/zero of=/dev/sda1 bs=1M count=64` exercises the AHCI driver or fills a disk; works against the vfsdev byte-range block nodes. |
| **`df`** | Free-space reporter for mounted ext4. Mount list is read from `/proc/mounts` (fallback: `/etc/mounts`, `/etc/mnts`); total/free come from the on-disk ext4 superblock of the partition node (same path `mkfs.ext4` uses). `df /dev/sda1` queries one device directly. |
| **`grep`** | Line-oriented text search: `grep [-i] [-n] [-v] [-c] [-l] [-q] [-w] [-r] [-F] PATTERN [FILE...]` (plain substring, no regexes), files or stdin, recursive mode via `getdents`. Exit 0 = match, 1 = none, 2 = error. |

`wget`/`grep` install into `/bin`; `ping`/`ip`/`dd`/`df` into `/sbin` next to the disk/fs and power tools.

---

## 🧰 Core utilities, path / process / system tools

A broad set of Linux-style userspace tools built on the kernel interfaces.
Several of them are backed by kernel additions made alongside this suite:
`/proc/mounts`, `/proc/modules`, `/proc/usb`, `/proc/<pid>/comm`, and the
`CACT_SYSCTL_SETHOSTNAME` ioctl.

### Text
`head`/`tail` (`-n`, `-c`; `tail -f` follows a growing file via size polling),
`wc` (`-c -l -w -m`), `sort` (`-n -r -u -f`), `uniq` (`-c -d -u`),
`cut` (`-d -f -c -s`), `tr` (`-d -s`, ranges, escapes), `tee` (`-a`), `seq`,
`yes`, `printf` (`%s %c %d %u %o %x %f`, `\n`/`\t`/`\xHH`).

### Files and paths
`cp` (`-r -f -v`), `touch` (`-c`), `basename`, `dirname`, `realpath`, `which`
(`-a`), `mktemp` (`-d`), `tty`, `du` (`-a -s -h`), `find` (`-name`, `-type`,
`-maxdepth`).

### System / process / devices
`uname` (`-a -s -n -r -m`), `hostname` (print, or set a new name), `nproc`,
`sync`, `env`, `printenv`, `free` (`/proc/meminfo`), `ps` (with a `COMMAND`
column from `/proc/<pid>/comm`), `lsmod` (`/proc/modules`), `lspci`
(`/dev/modinfo`), `lsusb` (`/proc/usb`), `dmesg` (`-r`, `-n N`), `mount`,
`umount` (`/sbin`; listing via `/proc/mounts`).

### Hashes
`sha256sum`, `sha384sum` (kernel `/dev/crypto`, one-shot, 1 MiB per call),
`md5sum`, `sha1sum` (portable C, streamed — no size limit).

### Existing tools, extended
`ls -l/-a/-h`, `mkdir -p`, `rm -r/-f`, `cat -n`, `mv` (several sources, with a
copy+unlink fallback across filesystems), `ln -f`, `stat` (octal mode +
`rwx` string), `echo -n/-e`, `grep -q/-w/-F`, `sleep` (fractions, several
arguments), `id -u/-g/-G/-n`, `chmod -R`, `chown -R`.

### Remaining limit
| Tool | Limit |
|------|-------|
| **`sha256sum`/`sha384sum`** | `/dev/crypto` hashes are one-shot with a 1 MiB per-call cap, so larger files are rejected (MD5/SHA-1 have no such limit) |

---

## 🚀 Runtime

After the kernel starts **`/bin/init`** (**cgoct**), the usual path is **`cactsole`** spawning these programs by **`execve`** when you type a command name. The same binaries are available on **`PATH=/bin:/sbin`** (see **cgoct** / **cactsole** environment defaults).

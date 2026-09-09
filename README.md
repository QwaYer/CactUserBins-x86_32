# 📦 CactUserBins/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C-orange.svg?style=for-the-badge" alt="Language: C">
  <img src="https://img.shields.io/badge/link-PIE%20%2B%20clibc.so-purple.svg?style=for-the-badge" alt="PIE + clibc.so">
  <img src="https://img.shields.io/badge/layout-one%20ELF%20per%20tool-blue.svg?style=for-the-badge" alt="One ELF per tool">
  <img src="https://img.shields.io/badge/tools-45-green.svg?style=for-the-badge" alt="45 tools">
  <img src="https://img.shields.io/badge/install-LocalRepoCactOS-0369a1.svg?style=for-the-badge" alt="install → LocalRepoCactOS">
</p>

<p align="center">
  A <strong>userspace tool suite</strong> for <strong>Cact OS</strong>: one <strong>small PIE ELF</strong> per command, shared implementations in <strong><code>common/</code></strong>, linked with <strong><code>clibc.so</code></strong> from <strong>CactLib</strong>.<br>
  Built artefacts land under <strong><code>build/bin/</code></strong>; <strong><code>make install</code></strong> stages them into <strong><code>LocalRepoCactOS/lib/bin</code></strong> and <strong><code>lib/sbin</code></strong> for <strong>cctkfs</strong> packing.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Utilities** | **45** standalone programs (see [`Makefile`](Makefile) `APPS`) |
| **`/bin` vs `/sbin`** | **`LR_BIN`** / **`LR_SBIN`** passed to **`make install`** (staging dirs under **LocalRepo**) |
| **Shared objects** | **`common/ex_*.c`** compiled once; each link pulls **`start.o`** + **one** `*/main.o` + all **`common/*.o`** with **`--gc-sections`** so unused entrypoints are dropped |
| **Load address** | PIE **ET_DYN** at **`0x08000000`** ([`link.ld`](link.ld)) — same family as **cactsole** / **cgoct** |
| **Headers** | **`CACTSOLEINC`** — path to **Cactsole** `include/` (set by **CactOS** or manually) |

---

## 🔗 Ecosystem

| Component | Role |
|-----------|------|
| **[CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32)** | **`clibc.so`** + **`build/pic/start.o`** — required for every link line |
| **[Cactsole-x86_32](https://github.com/QwaYer/Cactsole-x86_32)** | Interactive shell; heavy builtins live here as **ELFs** under **`/bin`** (see [`builtins/files_help.c`](../Cactsole-x86_32/src/builtins/files_help.c) vs **`common/ex_*.c`**) |
| **[LocalRepoCactOS](../LocalRepoCactOS)** | **`make userbins`** → **`make install`** here before **`cctkfs.img`** is packed |
| **[CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)** | **Workspace integrator** — sets **`CACTLIB`**, **`CACTSOLEINC`**, **`LR_*`**, then **`LocalRepo`** + **kernel** + **CactBridge** |
| **[CactKernel-x86_32](https://github.com/QwaYer/CactKernel-x86_32)** | **binfs** / **sbinfs** overlay **`/bin/*`** and **`/sbin/*`** from the **cctkfs** module on top of disk-backed FS |

---

## 🔨 Building

**Recommended — full workspace**

**[CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)** runs **`make install`** here with **`CACTLIB`**, **`CACTSOLEINC`**, **`LR_BIN`**, **`LR_SBIN`** set.

**Standalone — this repository**

```sh
make -j"$(nproc)" install   # auto-detects all siblings
make install                 # copy ELFs into LR_BIN / LR_SBIN
make clean
```

Override any path if needed: `make CACTLIB=/custom/path install`.

---

## 📂 Repository layout

```
CactUserBins-x86_32/
├── Makefile              # APPS list, BIN_APPS / SBIN_APPS split, install paths
├── link.ld               # PIE @ 0x08000000
├── common/
│   ├── ex_files.c        # ls, mkdir, rmdir, tch, rm, cat, wrt, stat, mv, ln, readlink
│   ├── ex_misc.c         # true, false, whoami, id, chmod, chown, version
│   ├── ex_net.c          # nconn, net, ping, dhcp, dns, ip, nc, wget
│   ├── ex_dd.c           # dd
│   ├── ex_df.c           # df
│   ├── ex_grep.c         # grep
│   ├── ex_sys.c          # clear, date, uptime, kill, su, sleep, free, fetch, run, modload, modunload
│   ├── ex_echo.c         # echo
│   └── ex_nav.c          # pwd (cd stays a cactsole builtin)
├── build/                # generated ELFs (gitignored)
├── cat/ ls/ …/           # one directory per utility; each holds main.c → main.o
├── fdisk/                # parted-analog: ptab.c (pure) + main.c
├── mkfs.ext4/            # ext4 formatter: ext4_fmt.c (pure) + main.c
├── mkfs.fat32/           # FAT32 formatter: fat32_fmt.c (pure) + main.c
├── cact-rootfs/          # root skeleton + boot/ deploy
├── tests/                # host test suite (make test)
└── README.md
```

**`APPS`** (authoritative list in the Makefile):  
`pwd` `ls` `mkdir` `rmdir` `tch` `rm` `cat` `wrt` `stat` `mv` `ln` `readlink` `clear` `date` `uptime` `kill` `su` `sleep` `free` `fetch` `modload` `modunload` `run` `echo` `true` `false` `whoami` `id` `chmod` `chown` `version` `nconn` `net` `ping` `dhcp` `dns` `nc` `wget` `dd` `df` `grep` + `fdisk` `mkfs.ext4` `mkfs.fat32` `cact-rootfs`

---

## 🧩 Design notes

| Topic | Detail |
|-------|--------|
| **Why one ELF per tool** | Smaller individual binaries than a busybox-style monolith; **`--gc-sections`** keeps only the **`main`** and **`cact_ub_*`** paths each `main.c` calls |
| **FHS-style paths** | **`sbinfs`** exposes **`/sbin/*`** for privileged-style tools (`kill`, `su`, PCI **`modload`** / **`modunload`**, **`ping`**, **`dhcp`**, **`dns`**, disk/fs tools) |
| **Syscall drift** | If **`syscall.h`** / libc numbers change in CactLib, rebuild **libc**, then **re-link** cactsole, **CactUserBins**, cgoct, and any other dynamic ELFs |

---

## 💽 Disk / filesystem utilities (Фаза 2)

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
make test      # builds build/host/* + runs tests/run_tests.sh
```

The host suite checks the tables with `fdisk -l`, ext4 with `e2fsck -fn`
(clean across 64M–400M volumes), FAT32 with `fsck.fat`/mtools, and the
rootfs skeleton by deploying a tree.

---

## 🛰️ Network / block / text utilities (Фаза 3)

| Tool | Purpose |
|------|---------|
| **`nc`** | Minimal netcat: TCP client (`nc HOST PORT`) and one-shot listener (`nc -l [-p] PORT`). After connect/accept it forks and relays stdin↔socket, so raw data and files can be pushed/pulled over the network. |
| **`wget`** | Micro HTTP/1.1 GET client: `wget [-o FILE] [http://]HOST[:PORT][/PATH]`. Handles `Content-Length`, chunked bodies, close-delimited responses and simple `Location` redirects; the body lands in a file (default: basename of the path). Enough for **cactpkg** to fetch manifests from a plain-HTTP mirror. |
| **`dd`** | Block copier (`if=`/`of=`, `bs=`, `count=`, `skip=`, `seek=`, `conv=notrunc`, `status=none`). `dd if=/dev/zero of=/dev/sda1 bs=1M count=64` exercises the AHCI driver or fills a disk; works against the vfsdev byte-range block nodes. |
| **`df`** | Free-space reporter for mounted ext4. Mount list is read from `/proc/mounts` (fallback: `/etc/mounts`, `/etc/mnts`); total/free come from the on-disk ext4 superblock of the partition node (same path `mkfs.ext4` uses). `df /dev/sda1` queries one device directly. |
| **`grep`** | Line-oriented text search: `grep [-i] [-n] [-v] [-c] [-l] [-r] PATTERN [FILE...]` (plain substring, no regexes), files or stdin, recursive mode via `getdents`. Exit 0 = match, 1 = none, 2 = error. |

`nc`/`wget`/`grep` install into `/bin`, `dd`/`df` into `/sbin` next to the other disk tools. Reboot/poweroff are postponed until the ACPI/powerd shutdown path is sorted out.

---

## 🚀 Runtime

After the kernel starts **`/bin/init`** (**cgoct**), the usual path is **`cactsole`** spawning these programs by **`execve`** when you type a command name. The same binaries are available on **`PATH=/bin:/sbin`** (see **cgoct** / **cactsole** environment defaults).

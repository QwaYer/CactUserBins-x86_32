# 📦 CactUserBins/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C-orange.svg?style=for-the-badge" alt="Language: C">
  <img src="https://img.shields.io/badge/link-PIE%20%2B%20clibc.so-purple.svg?style=for-the-badge" alt="PIE + clibc.so">
  <img src="https://img.shields.io/badge/layout-one%20ELF%20per%20tool-blue.svg?style=for-the-badge" alt="One ELF per tool">
  <img src="https://img.shields.io/badge/tools-36-green.svg?style=for-the-badge" alt="36 tools">
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
| **Utilities** | **36** standalone programs (see [`Makefile`](Makefile) `APPS`) |
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
│   ├── ex_net.c          # nconn, net, ping, dhcp, dns
│   ├── ex_sys.c          # clear, date, uptime, kill, su, sleep, free, fetch, run, modload, modunload
│   ├── ex_echo.c         # echo
│   └── ex_nav.c          # pwd (cd stays a cactsole builtin)
├── build/bin/            # generated ELFs (gitignored)
├── cat/ ls/ …/           # one directory per utility; each holds main.c → main.o
└── README.md
```

**`APPS`** (authoritative list in the Makefile):  
`pwd` `ls` `mkdir` `rmdir` `tch` `rm` `cat` `wrt` `stat` `mv` `ln` `readlink` `clear` `date` `uptime` `kill` `su` `sleep` `free` `fetch` `modload` `modunload` `run` `echo` `true` `false` `whoami` `id` `chmod` `chown` `version` `nconn` `net` `ping` `dhcp` `dns`

---

## 🧩 Design notes

| Topic | Detail |
|-------|--------|
| **Why one ELF per tool** | Smaller individual binaries than a busybox-style monolith; **`--gc-sections`** keeps only the **`main`** and **`cact_ub_*`** paths each `main.c` calls |
| **FHS-style paths** | **`sbinfs`** exposes **`/sbin/*`** for privileged-style tools (`kill`, `su`, PCI **`modload`** / **`modunload`**, **`ping`**, **`dhcp`**, **`dns`**) |
| **Syscall drift** | If **`syscall.h`** / libc numbers change in CactLib, rebuild **libc**, then **re-link** cactsole, **CactUserBins**, cgoct, and any other dynamic ELFs |

---

## 🚀 Runtime

After the kernel starts **`/bin/init`** (**cgoct**), the usual path is **`cactsole`** spawning these programs by **`execve`** when you type a command name. The same binaries are available on **`PATH=/bin:/sbin`** (see **cgoct** / **cactsole** environment defaults).

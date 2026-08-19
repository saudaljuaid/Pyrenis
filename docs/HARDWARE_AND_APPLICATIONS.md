# Drivers, wireless, and applications

How Seneri gets real hardware and real programs, and what each route actually
costs. This document decides direction; it implements nothing.

The short version: **for applications, copy Linux's interface and none of its
code. For drivers, build the standardised class drivers, which cover most
hardware with one implementation each, and put drivers in userspace so a
driver's licence and a driver's bugs both stay outside the kernel.** Borrowing
Linux driver source is possible more often than it first looks - much of it is
dual-licensed - but almost never for the drivers you would most want it for, and
section 3 explains why.

## 1. The licence, which decides more than the engineering

Every file in this repository carries `SPDX-License-Identifier: GPL-3.0-only`,
and `LICENSE` is GPL version 3.

Linux is **GPL-2.0-only**. Its `COPYING` states version 2 and explicitly does
not offer "or any later version". GPL-2.0-only and GPL-3.0-only are mutually
incompatible: neither licence's conditions can be satisfied while also obeying
the other, so a combined work cannot be distributed under either.

The consequence is concrete, but it is **narrower than it first appears**, and
an earlier draft of this document overstated it. The correct statement is:

> **A GPL-2.0-only file cannot be compiled into the Seneri kernel.** Not
> "difficult" - not permitted, no matter how good the shim layer is.
>
> But **not every file in Linux is GPL-2.0-only.** A great deal of it,
> including much of what hardware vendors contribute, is dual-licensed
> `GPL-2.0 OR BSD-3-Clause`, and the BSD-3-Clause option **is** compatible with
> GPL-3.0. Whether a given driver may be used is a per-file question answered by
> its SPDX line, not a blanket rule.

Section 3 works that correction through on the case where it matters most.

It is worth being precise about why the obvious counter-example does not apply.
FreeBSD runs Linux Wi-Fi drivers through LinuxKPI, and that works because the
FreeBSD kernel is BSD-licensed. A permissive kernel can absorb a GPL-2 driver
and ship the result under GPL-2 terms for those files. A **GPL-3** kernel cannot,
because GPL-2-only forbids adding GPL-3's terms and GPL-3 forbids dropping them.
The direction of the incompatibility is what matters, and it points the wrong
way for Seneri.

Three ways out, and only the third needs no permission:

1. **Relicense Seneri** to GPL-2.0-or-later, or dual-license it. This is a
   decision only the copyright holder can make, it is effectively irreversible,
   and it should not be made to acquire a driver.
2. **Keep the driver out of the kernel's licence domain.** A driver in its own
   address space, talking to the kernel over a documented IPC boundary, is a far
   more defensible separate work than one linked into `seneri.elf`. This is the
   userspace-driver argument in section 5, and it happens to be the right
   architecture regardless of licensing.
3. **Check the SPDX line before assuming either way.** A dual-licensed
   `GPL-2.0 OR BSD-3-Clause` file may be taken under its BSD option; a
   GPL-2.0-only file may not. Then ask the second question, which is usually the
   one that actually decides it: does the file stand alone, or does it call into
   a GPL-2.0-only subsystem? Where neither works, write against the public
   hardware specifications - for a surprising amount of modern hardware that
   costs less than the shim layer would have.

An important distinction that is often blurred: **firmware blobs are not driver
source.** Nearly every modern Wi-Fi and GPU part needs a vendor firmware image
loaded at initialisation. Those images are distributed under separate
redistribution licences (this is what `linux-firmware` is), they are not GPL, and
loading one is not a derived work of anything. Firmware is the tractable half of
the Wi-Fi licence problem. The driver source is the hard half, and section 3
avoids needing it.

## 2. Applications: implement the interface, not the implementation

For programs, the answer is the opposite of the driver answer, and the reason is
the same licence analysis read the other way.

**An ABI is an interface, not code.** Implementing the Linux system-call ABI
copies nothing, links nothing, and derives from nothing. It is what FreeBSD's
Linuxulator, WSL 1, and gVisor all do. The GPL-2/GPL-3 problem does not arise
because no Linux code is involved.

The payoff is disproportionate. A kernel that answers the Linux x86-64 syscall
ABI can run **unmodified statically-linked Linux binaries** — musl and glibc
programs, busybox, compilers, interpreters — with no porting, no recompilation,
and no per-application work. The alternative, inventing a native ABI, means
porting a libc and then every program, forever.

The cost is bounded, and it is bounded in exactly the shape this project already
works in. Linux has upwards of 350 syscalls; a static busybox needs on the order
of 40 to 60 of them, and a large fraction of those are trivially small. Each
syscall is independently specifiable and independently testable — one increment,
one invariant, one executable failure test. A program that hits an unimplemented
syscall gets `ENOSYS`, which is a named refusal rather than undefined behaviour,
and is precisely how this kernel already treats everything it does not
understand.

There is a second, less obvious payoff, and it is the one that matters most for
wireless: **`wpa_supplicant` and `iw` are ordinary Linux userspace programs.**
A kernel that answers the Linux syscall ABI plus enough of the netlink/`nl80211`
shape can run them as-is. That converts "write a WPA3 supplicant" — a six-figure
line count of security-critical cryptography that must not be got wrong — into
"run a program someone else already wrote and audited". Section 3 depends on
this.

What it does not give: a Linux-compatible *kernel module* interface. Nothing here
lets a `.ko` load, and nothing should.

## 3. Wireless, costed honestly

Wi-Fi is the hardest thing on this list and it is worth saying why, because the
naive plan — "write a driver for my laptop's Wi-Fi card" — misjudges the work by
about two orders of magnitude.

Wi-Fi is four problems, not one:

| Layer | What it is | Linux's answer | Rough size |
| --- | --- | --- | --- |
| Bus attachment | Find the device, map its registers, set up DMA and interrupts | per-driver | 2–5k lines |
| Firmware load | Push a vendor image into the device before it does anything | `linux-firmware` | small, needs a filesystem |
| 802.11 MAC (MLME) | Scanning, association, authentication, power save, rate control, aggregation | `mac80211` | ~100k lines |
| Security supplicant | WPA2, WPA3/SAE, enterprise EAP | `wpa_supplicant` | 200k+ lines, userspace |

Only the first is what people picture when they say "Wi-Fi driver". The third
and fourth are where the years go.

### Intel, specifically, and why "open source" is not one question

Intel's Linux wireless driver is genuinely open source, and that deserves to be
stated precisely, because the obvious conclusion from it is wrong.

`drivers/net/wireless/intel/iwlwifi/` carries
`// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause`. Under the BSD-3-Clause
option that source **could** legally be used by a GPL-3.0 kernel. The licence is
not the blocker for the driver.

The blocker is what sits underneath it. `iwlwifi` is a **SoftMAC** driver: it is
the bottom third of a stack, and it calls into `mac80211` and `cfg80211` for
everything above the radio. Those carry
`// SPDX-License-Identifier: GPL-2.0-only`, with no second option. So the
roughly 100k lines that actually implement 802.11 - the part nobody wants to
write - are exactly the part that cannot be taken.

Two further facts complete the picture:

- **Intel does not publish register-level programming documentation for its
  Wi-Fi parts.** For its Ethernet controllers and its GPUs it publishes full
  programmer's reference manuals, and clean-room implementation from those is
  entirely practical. For Wi-Fi the driver source *is* the documentation, which
  makes "read the GPL-2.0-only stack and write your own" the legally riskiest
  route of the three rather than the safest.
- **The firmware is a closed binary either way.** Intel's wireless firmware is
  redistributable as part of an open-source operating system and explicitly may
  not be modified or reverse engineered. That is workable - loading a firmware
  image is not a derived work of it - but the openness stops at the hardware
  boundary regardless of what the driver source says.

So "is Intel open source?" is really four questions, and Intel Wi-Fi answers
them differently:

| Question | Intel Wi-Fi | Intel Ethernet | Intel GPU |
| --- | --- | --- | --- |
| Driver source published | yes | yes | yes |
| Driver licence usable here | **yes**, via the BSD option | yes | yes |
| Usable standalone | **no** - needs GPL-2.0-only `mac80211` | yes | mostly |
| Register documentation published | **no** | yes | yes |

The paradox is real: Intel Wi-Fi is open source and is still the worst target on
this list, because the openness runs out exactly where the difficulty starts.
Intel Ethernet and Intel GPUs are, by contrast, among the best-documented
hardware available, and both are proper clean-room targets.

### The decision that removes most of the work

**FullMAC, not SoftMAC.**

A **SoftMAC** device implements only the radio; the host must run the entire
802.11 state machine. Most Intel parts (`iwlwifi`) are this, and a SoftMAC device
means writing or acquiring the ~100k-line MLME layer before a single packet
moves. There is no shortcut and, given section 1, no borrowing it either.

A **FullMAC** device runs the MLME *in its own firmware*. The host says "connect
to this SSID with this key" through a vendor command interface and the firmware
performs the scan, the association, the authentication and — on most parts — the
WPA2 four-way handshake. Broadcom `brcmfmac` parts, many NXP/Marvell parts, and
a large fraction of USB Wi-Fi adapters are FullMAC.

Choosing a FullMAC part first deletes the third row of that table entirely and
most of the fourth. It turns Wi-Fi from a multi-year subsystem into a
few-thousand-line driver over a command interface — the same order of work as an
Ethernet controller.

The remaining supplicant gap is WPA3/SAE and enterprise EAP, which FullMAC
firmware generally does not do. That is what section 2 is for: run
`wpa_supplicant` as an application rather than reimplementing it.

**USB before PCIe, if the choice is free.** A USB FullMAC adapter needs an xHCI
driver, which is a standardised class driver Seneri wants anyway for keyboards
and storage, and it moves the device outside the machine so a broken driver
cannot wedge the platform. One xHCI driver serves every USB controller ever
made; a PCIe Wi-Fi driver serves one vendor's parts.

## 4. What the standards buy, and the order they should be built in

The single most important economic fact about modern PC hardware: **the
high-value device classes are standardised, so one driver covers every vendor.**

| Class | Standardised | One driver covers | Prerequisites beyond enumeration |
| --- | --- | --- | --- |
| NVMe storage | yes | every NVMe SSD | BAR mapping, MSI-X, DMA |
| xHCI (USB) | yes | every USB controller, and every class device behind it | BAR mapping, MSI-X, DMA, timers |
| AHCI (SATA) | yes | every SATA controller | BAR mapping, MSI/MSI-X, DMA |
| virtio | yes | every virtual machine's disk, network, console | BAR mapping, MSI-X, DMA |
| Ethernet | per-vendor, documented | one vendor's parts | BAR mapping, MSI-X, DMA |
| Wi-Fi | **no** | one vendor's parts, plus firmware | all of the above, plus threads |
| GPU (beyond a framebuffer) | no | nothing | do not attempt |

Four standardised drivers — NVMe, xHCI, AHCI, virtio — cover storage, input and
peripherals on essentially every machine built in the last decade, and every
virtual machine. They should come first, and not only because they are easier:
each one exercises the same substrate Wi-Fi needs (BAR mapping, MSI-X, DMA,
queues), on hardware that is far easier to debug, with a specification that is
public and stable.

`virtio` deserves a specific note: it is the cheapest of all of them, it works in
exactly the QEMU this project already tests in, and it makes a networked,
disk-backed Seneri testable in CI long before any physical hardware is involved.

## 5. Drivers belong in userspace

The licence argument in section 1 and this project's stated values point the same
way, which is a good sign.

A driver linked into `seneri.elf` is part of the kernel's licence domain, runs
with full supervisory privilege, and can take the machine down. A driver running
in ring 3, in its own address space, reaching its device through a mapping the
kernel granted and talking to the rest of the system over a documented IPC
boundary, is:

- **outside the kernel's licence domain**, which makes a third-party or
  differently-licensed driver possible at all;
- **restartable** — a driver fault is a dead process and a re-initialised device,
  not the deterministic panic this kernel currently guarantees for everything;
- **containable**, once there is an IOMMU, even against a driver that programs
  its device to DMA somewhere it should not.

The cost is IPC on the data path. It is real, and it is the reason the design has
to be shared-memory rings with the kernel only involved in interrupt delivery and
mapping — not a message per packet. That is a design constraint to accept up
front rather than a tax to discover later.

The kernel keeps only what cannot be delegated: enumeration, address-space and
BAR mapping, interrupt vector allocation and delivery, DMA-capable memory
allocation, and eventually the IOMMU.

## 6. The route, in increments

Each of these is one change, in this repository's sense: an invariant, an
executable failure test, and a boot that proves it or refuses.

**Substrate — everything else waits on these.**

1. **Base address registers: size, and map them.** The first increment that
   *writes* configuration space, and the first that can take a working device
   away from whoever is using it, so it must disable a function's decode while it
   probes and put it back. Prerequisite for every driver.
2. **Interrupt vectors, and MSI-X.** A vector allocator, then programming a
   device's MSI-X table. This is the increment that routes around
   `docs/IO_APIC.md`'s deferred work entirely: **a message-signalled interrupt is
   a memory write to the local APIC, so it is edge-triggered by construction and
   needs no redirection entry, no trigger-mode decision, and no directed
   end-of-interrupt.** Every PCIe device Seneri would want — including every
   Wi-Fi part — supports it. Level-triggered I/O APIC routing remains worth
   having for legacy devices, but it stops being on the critical path to
   hardware.
3. **DMA-capable memory.** Physically contiguous allocation with an
   address-width bound, and explicit ownership transfer between CPU and device.
   The frame allocator cannot express either today.
4. **Threads and a scheduler.** Firmware loading, link establishment and command
   completion are all long blocking operations. `docs/MONOTONIC_TIME.md` already
   names this as the thing the deadline layer exists to make possible.

**First hardware.**

5. **A virtio driver.** Cheapest possible exercise of steps 1–4, testable in the
   QEMU this project already uses, in CI.
6. **xHCI.** One driver, every USB controller, and the road to input devices,
   mass storage, and a FullMAC Wi-Fi adapter.
7. **NVMe or AHCI**, then a filesystem — which is also how firmware images get
   onto the machine. Before that exists, a Multiboot2 module is the honest
   interim answer, and GRUB already supplies it.

**Programs.**

8. **Ring 3, per-process address spaces, ELF loading.** `docs/VIRTUAL_MEMORY.md`
   deliberately refuses the user bit today; this is where that changes.
9. **The Linux system-call ABI, incrementally.** Target a statically-linked
   busybox first and add syscalls until it runs. `ENOSYS` for everything else.

**Wireless, last, because it needs all of it.**

10. **A FullMAC adapter**, firmware from a file, vendor command interface,
    WPA2-PSK offloaded to firmware.
11. **A network stack** — or, more cheaply at first, a userspace one over a raw
    device interface.
12. **`nl80211` shape and `wpa_supplicant`** for WPA3 and enterprise, running as
    an ordinary application because of step 9.

## 7. What not to do

- **Do not assume a Linux driver is off limits, and do not assume it is
  available.** Read its SPDX line, then ask what it depends on. `iwlwifi` is
  dual-licensed and therefore usable; the `mac80211` stack it cannot work
  without is GPL-2.0-only and therefore is not. The dependency decided it, not
  the driver's own licence.
- **Do not build a Linux kernel module ABI.** It buys nothing the syscall ABI
  does not, and it re-imports the licence problem.
- **Do not start with a SoftMAC Wi-Fi part.** It hides a 100k-line 802.11 stack
  behind a driver that looks the same size as an Ethernet one.
- **Do not attempt a modern GPU.** A framebuffer from firmware is enough for a
  very long time.
- **Do not write drivers before threads.** Every real device has an operation
  that takes milliseconds, and a kernel with one thread of control must busy-wait
  through all of them.
- **Do not size a BAR without disabling the function's decode first.** The
  console is behind a device on the same bus.

## Open questions

- **Relicensing.** Everything above assumes GPL-3.0-only stands. If the goal
  were ever to absorb existing driver ecosystems directly, that assumption is the
  first thing to revisit, and it is not an engineering decision.
- **Which FullMAC part.** This needs a survey of what is actually purchasable
  with documentation and redistributable firmware, and that survey should happen
  before step 6 fixes the bus choice.
- **Whether any FullMAC driver is both dual-licensed and standalone.** The
  `iwlwifi` finding says this is worth checking per driver rather than assumed.
  A FullMAC driver talks to firmware over a vendor command interface rather than
  to `mac80211`, so it is far likelier to stand alone than a SoftMAC one - which
  would leave the licence as the only question, and that one is answerable by
  reading a single line.
- **IOMMU.** Userspace drivers are containable only with one. Until then a
  userspace driver is isolated from the kernel's *instructions* but not from its
  *memory*, and saying so honestly matters more than the isolation does.

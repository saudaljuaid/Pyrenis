# Framebuffer write-combining

The cached surface removed framebuffer reads from scrolling. Its remaining hot
path is the sequential copy from write-back RAM to the linear framebuffer.
Seneri now gives exactly that framebuffer span a write-combining page memory
type. VGA text memory, local APIC, every I/O APIC, and PCI ECAM remain strongly
uncacheable. Kernel RAM and the cached surface remain write-back.

This distinction is part of the address-space model, not a drawing hint.
`paging_translate` reports the memory type selected by the leaf and the installed
`IA32_PAT`; a permissions match without a memory-type match is not proof.

## Architectural basis

The implementation was checked against the current Intel 64 and IA-32
Architectures Software Developer's Manual, volume 3A, chapter 14, and the AMD64
Architecture Programmer's Manual, volume 2, chapter 7. The relevant pieces are
the PAT type encodings, PAT index table, combined PAT/MTRR table, memory-type
transition sequence, and the `SFENCE` instruction definition.

`CPUID.01H:EDX[16]` reports PAT support. `IA32_PAT` is MSR `0x277`; each of its
eight bytes is one type. Seneri accepts only the architectural encodings:

| Encoding | Meaning |
| ---: | --- |
| `0x00` | uncacheable |
| `0x01` | write-combining |
| `0x04` | write-through |
| `0x05` | write-protected |
| `0x06` | write-back |
| `0x07` | uncached-minus |

Any reserved byte makes the inherited PAT layout unsafe. PAT entry 0 must be
write-back and entry 3 must be uncacheable before the kernel will proceed,
because the bootstrap hierarchy and Seneri's register windows rely on those two
meanings.

### Why entry 1

The PAT index is `{PAT, PCD, PWT}`. The PAT bit is bit 7 in a 4 KiB PTE but bit
12 in a 2 MiB or 1 GiB leaf. Entry 1 needs only `PWT=1`, `PCD=0`, and `PAT=0`,
so its encoding is identical at every leaf size and cannot confuse the two PAT
bit positions.

`boot.S` sets none of PAT, PCD, or PWT in its leaves, so it selects entry 0.
Before this change, Seneri's RAM leaves also selected entry 0 and its live
register windows selected entry 3. No bootstrap or live mapping selected entry
1. Seneri may therefore replace only byte 1 without changing the meaning of an
active translation. On the measured machines the exact transition was:

```text
before 0x0007040600070406
after  0x0007040600070106
                       ^ entry 1: write-through (0x04) to write-combining (0x01)
```

All other bytes are preserved exactly. The target is read back before CR3 may
select it.

### Installation order

The pure paging self-test runs before any PAT MSR access. Installation then:

1. checks PAT, NX, PAE, and four-level paging support;
2. reads and validates every inherited PAT byte;
3. constructs a target value changing entry 1 only;
4. builds the inactive hierarchy and walks every intended mapping using that
   target PAT value;
5. writes `IA32_PAT` and requires an exact readback;
6. executes `WBINVD` while the bootstrap hierarchy is still active;
7. loads CR3, which selects WC for the framebuffer and flushes non-global TLB
   translations;
8. executes `WBINVD` again and verifies the live hierarchy and PAT readback.

Interrupts are disabled across this sequence. The first `WBINVD` drains any
cache line touched while the bootstrap hierarchy described the framebuffer
with its old type. The second completes a conservative type transition. On a
post-switch verification failure, rollback performs the cache flushes around
the reverse CR3 transition and restores the old PAT only after entry 1 is no
longer selected.

`paging_protect` deliberately cannot change a live mapping's memory type. It
returns `PAGING_STATUS_MEMORY_TYPE_CHANGE_UNSAFE`; changing access permissions
is not permission to perform an incomplete cache transition.

### MTRRs

Seneri neither changes nor claims ownership of firmware MTRRs. The Intel and AMD
combined-type tables both define a PAT-selected WC page as effective WC for the
architectural MTRR types, including an MTRR UC range. This is why PAT can assign
WC to a framebuffer in a physical range firmware otherwise treats as device
memory.

That table is not a performance measurement. A virtual display device, a host
hypervisor, or a physical display controller can still dominate the cost, and
Seneri does not yet dump or audit MTRR ranges for a general aliasing policy. The
framebuffer is split into 4 KiB leaves, so no single large leaf crosses a
framebuffer boundary while claiming one page type.

## Invariants

- `PAGING_UNCACHED` and `PAGING_WRITE_COMBINING` are incompatible requests.
  Combining them returns `PAGING_STATUS_CONFLICTING_MEMORY_TYPES`; neither wins
  by masking or precedence.
- Only the 4 KiB leaves intersecting framebuffer bytes select PAT entry 1.
- VGA text memory, the local APIC, every I/O APIC, and all mapped PCI ECAM pages
  select uncacheable entry 3.
- Ordinary RAM, page tables, the heap, and every cached-surface page select
  write-back entry 0.
- A translation reports `PAGING_MEMORY_INVALID` for a reserved PAT byte rather
  than guessing.
- Every public status and memory-type name has a table statically sized against
  its final enumerator.
- A write-back framebuffer mapping never satisfies framebuffer verification,
  even if its read/write/execute permissions match.
- No Rust code participates. PAT ownership, page tables, fences, proofs, and
  measurements are C and x86-64 assembly.

## Store completion

WC stores may remain in weakly ordered processor buffers. `cpu_store_fence()` is
one `sfence`. `surface_present()` executes it after the final volatile
framebuffer store and before it increments completion counters, clears damage,
returns, or permits the caller's framebuffer readback. Direct framebuffer proof
and logo batches fence before their own readbacks as well.

`screen_verify_cell` still calls `framebuffer_read_pixel`; it never consults the
surface. A verifier pointed at the source buffer would only prove that drawing
agreed with itself, not that the copy reached the display mapping.

## Proof structure

`paging_self_test()` uses synthetic PAT values and synthetic 4 KiB and large
leaves. It covers all six architectural types, the different PAT-bit positions,
reserved bytes, missing PAT support, unsafe inherited layouts, incompatible
requests, and refusal to change a live page's type. It runs before any real PAT
MSR is touched.

`prove_write_combining()` runs after the new CR3 is installed and before the
first framebuffer store. It checks the exact PAT readback, walks every page of
each named register window, walks every framebuffer page, and samples ordinary
kernel RAM. `framebuffer_verify()` independently repeats the whole framebuffer
walk. `surface_verify()` walks the cached allocation and requires write-back.

The dedicated `write-combining` scenario uses guest exit `0x2C`, host status 89.
It checks PAT ownership, every framebuffer and ECAM page, ordinary RAM, a real
heap surface, the incompatible-policy refusal, and the final framebuffer
verifier. Its stable diagnostic is:

```text
ST WRITE-COMBINING PAT <value> ENTRY 1 FRAMEBUFFER <pages> PAGES
```

Normal boot reports the raw PAT transition, selected framebuffer type and page
count, combined surface cycles, split cached-draw/framebuffer-push cycles, and
the sparse two-corner union. Timing values are deliberately not assertions.

## Measurements

These are TSC cycles at 1024x768x32. Each cell is the median of five successful
boots with the full range in parentheses. Before is the final instrumented
kernel with only the framebuffer mapping and its control verifiers changed back
to uncacheable. After is the production WC mapping. Both used QEMU 11.1.0,
128 MiB, one vCPU, the default PC/i440fx machine and virtual standard VGA,
SeaBIOS, Limine 12.6.0 Multiboot2, and the same Windows 11 host load policy.

The host is an Intel Core i7-1255U. WHPX executes guest instructions on that CPU
but its display controller is still virtual; it is not bare metal.

### Combined operations and sparse union

| Executor | Mapping | Full present | One 16-pixel line | Scroll | Sparse two-corner union |
| --- | --- | ---: | ---: | ---: | ---: |
| TCG | UC | 206,842,672 (153,424,972-350,274,272) | 4,045,438 (3,323,646-6,444,418) | 198,378,030 (162,011,244-304,256,582) | 186,956,480 (160,236,496-310,161,794) |
| TCG | WC | 145,065,876 (116,325,842-197,239,352) | 3,740,730 (2,517,210-5,850,292) | 146,252,504 (114,736,990-202,567,546) | 137,766,350 (112,827,990-167,233,064) |
| WHPX | UC | 168,477,716 (142,906,826-213,276,655) | 2,568,350 (2,516,634-3,023,482) | 143,859,908 (127,206,872-160,028,672) | 141,004,948 (124,750,352-160,401,711) |
| WHPX | WC | 43,694,990 (11,991,240-61,914,368) | 34,470 (19,092-42,070) | 2,173,906 (1,843,504-2,699,520) | 903,106 (620,732-1,261,988) |

### Cached draw/copy portion

| Executor | Mapping | Full draw | One-line draw | Scroll copy/fill | Sparse draw |
| --- | --- | ---: | ---: | ---: | ---: |
| TCG | UC | 9,829,902 (8,512,230-35,186,284) | 229,924 (200,584-413,486) | 10,501,844 (9,831,636-20,557,444) | 755,710 (619,404-921,218) |
| TCG | WC | 6,927,244 (5,171,382-9,255,311) | 204,110 (155,470-1,709,008) | 8,395,672 (6,927,418-10,249,418) | 612,282 (496,196-989,756) |
| WHPX | UC | 22,340,910 (16,855,506-79,194,343) | 28,024 (22,280-31,662) | 1,303,680 (901,494-2,854,944) | 492 (186-2,256) |
| WHPX | WC | 41,908,784 (10,943,070-60,415,574) | 11,030 (8,420-15,158) | 1,120,828 (968,534-1,428,204) | 402 (82-734) |

### Framebuffer-push portion

| Executor | Mapping | Full push | One-line push | Scroll push | Sparse union push |
| --- | --- | ---: | ---: | ---: | ---: |
| TCG | UC | 198,330,442 (143,595,070-315,087,988) | 3,843,804 (3,088,502-6,030,932) | 187,876,186 (152,179,608-283,699,138) | 186,128,488 (159,542,488-309,240,576) |
| TCG | WC | 138,138,632 (111,154,460-189,056,382) | 3,556,480 (2,361,740-4,854,508) | 137,691,476 (107,809,572-192,318,128) | 136,776,594 (112,331,794-166,568,648) |
| WHPX | UC | 141,884,285 (126,051,320-147,720,890) | 2,542,776 (2,494,354-2,991,820) | 141,004,964 (126,305,378-158,131,648) | 141,004,044 (124,749,860-160,401,525) |
| WHPX | WC | 1,273,914 (1,048,170-1,786,206) | 21,790 (10,672-26,974) | 1,039,388 (803,218-1,271,316) | 902,720 (620,650-1,261,254) |

WHPX's median push cost fell 99.1% for full, 99.1% for one line, 99.3%
for scroll, and 99.4% for the sparse union. The combined full result fell
74.1% because cached drawing dominates after the push becomes small. TCG's
medians also fell, but TCG does not model the cache claim; those numbers are a
functional comparison and noisy timing, not evidence of WC hardware behavior.

One WHPX WC measurement boot reached neither paging installation nor the surface
proof before the 30-second harness bound. It was excluded and replaced; five
successful boots remain in the table.

KVM was unavailable on this Windows host, and WSL was not installed, so no KVM
number is reported. No bootable bare-metal target or physical display controller
was available. Those omissions are evidence gaps, not zero-cost results.

## Negative controls

Each control began from a clean build. Affected files were copied with `cp` (or
the PowerShell `Copy-Item` equivalent), one defect was applied, the narrow and
normal/relevant scenarios were rebuilt and run under both TCG and WHPX, and the
files were restored from the copies. No correctness control that was supposed
to fail passed.

| # | Deliberate break | TCG and WHPX result |
| ---: | --- | --- |
| 1 | Leave the framebuffer UC while the proof claims WC. | Both narrow and full boots halted: `PANIC: framebuffer range is not write-combining`. |
| 2 | Map the framebuffer WB. | Both halted with the same installed-range panic; WB never satisfied the framebuffer proof. |
| 3 | Give VGA text memory WC. | Both halted: `PANIC: VGA window is not uncacheable`. |
| 4 | Program entry 1 with the wrong type. | Both halted before framebuffer use: `PANIC: page attribute table readback did not match`. |
| 5 | Give only the first framebuffer page WC. | Both halted: `PANIC: framebuffer range is not write-combining`. |
| 6 | Allow `PAGING_UNCACHED | PAGING_WRITE_COMBINING`. | Both halted in the pure test: `PANIC: page table arithmetic self-test failed`. |
| 7 | Remove the post-store `sfence`. | `surface` and normal passed on both executors. These emulators did not expose the ordering bug; no failure was invented. |
| 8 | Point `screen_verify_cell` at the cached surface. | `screen` and normal passed on both. This is the dangerous blind oracle: source and verifier can agree while the physical display is wrong. Production still reads the framebuffer. |
| 9 | Bypass the no-PAT refusal. | Both narrow and full boots halted in the pure test: `PANIC: page table arithmetic self-test failed`. The production pure paths require `PAT_UNSUPPORTED` and `PAT_LAYOUT_UNSAFE`. |
| 10 | Select the old UC framebuffer and adjust only control verifiers. | Dedicated, surface, and full functional boots passed on both. The before rows above changed timing while framebuffer readback stayed valid. |

During controls, three TCG normal boots in control 7 and one in control 8 hit
`PM timer and local APIC timer disagree on interval` while stale QEMU processes
were consuming the host; the exact task processes were reaped and retries
passed. During the first control-10 normal sampling attempt, one TCG boot hit
`sleep overshot its deadline` and four hit the same PM/APIC interval check.
Dedicated surface sampling replaced those failed boots. None is counted as a
measurement or a proof result.

## Verification sweep

The final 28-scenario count includes every scenario inherited from the base and
the new `write-combining` scenario. One Ubuntu 24.04 workflow job invoked the
exact `make qemu-tests` target twenty separate times after `make verify`:
20/20 invocations and 560/560 scenario boots passed. The log contains twenty
`all deterministic QEMU scenarios passed` lines and no `ST FAIL`, kernel panic,
scenario failure, or timeout. The first marker was `QEMU sweep 1/20` and the
last was `QEMU sweep 20/20`; GitHub Actions run `32289648657` completed
successfully from 18:50:42 to 19:05:29 UTC.

An earlier hosted attempt was cancelled at its ten-minute job limit while the
runner's Azure Ubuntu package mirror was unresponsive, before compilation or
QEMU began. It is not counted as a test run. The workflow now removes that dead
mirror-list entry and allows twenty minutes for the normal single verification
pass; the temporary twenty-run loop used for the sweep is not part of the final
workflow.

## Limitations and deferred work

- Bare-metal framebuffer behavior, the display controller, firmware path, MTRR
  dump, and performance remain unmeasured. KVM is also unmeasured.
- The page-table walk proves the selected PAT type and exact MSR readback. It
  cannot prove that a virtual display path or physical controller realizes a
  particular throughput.
- One damage rectangle still unions far-apart changes. The two-corner case
  therefore copies all 786,432 pixels; multiple rectangles remain deferred.
- The framebuffer is still loader-owned and fixed for the boot. A display
  driver, mode setting, BAR ownership, and remapping above 4 GiB are separate
  work.
- The memory-type layer supports the types it can decode, but only WB, WC, and
  UC are currently requestable policies. A general MMIO registry and MTRR/alias
  audit remain deferred.

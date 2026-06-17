# RTL8168 / RTL8111 gigabit NIC bring-up

How the RealTek `re`/`rtl8169` gigabit MACs come up, and what Cardinal's
`drivers/rtl8169` was getting wrong. Synthesised from the FreeBSD and OpenBSD
drivers (read-only reference trees under `~/src/bsd-net-ref/`):

- FreeBSD: `freebsd/sys/dev/re/if_re.c`, `freebsd/sys/dev/rl/if_rlreg.h`
- OpenBSD: `openbsd/sys/dev/ic/re.c`, `openbsd/sys/dev/ic/rtl81x9reg.h`,
  `openbsd/sys/dev/pci/if_re_pci.c`, `openbsd/sys/dev/pci/pci_map.c`

This is **documentation written from** that code, not a copy of it. Line
citations are `file:line` against those trees so claims are checkable.

The target is the **RTL8111G** on the AtomicPi — PCI `10ec:8168`, hwrev
`0x4C000000` = `RL_HWREV_8168G`. QEMU does not emulate an 8168, so this driver
only ever binds real 8168-family hardware; the bring-up can target the 8168G
path directly.

## The "C+" descriptor MAC

The gigabit parts (8169/8168/8111) are the "C+" generation: DMA via 16-byte
descriptor rings (not the linear ring buffer of the old 8139). One descriptor:

```
u32 cmdstat;     // OWN(0x80000000) | EOR(0x40000000) | SOF/EOF | buf length (low 13b for RX)
u32 vlanctl;
u32 bufaddr_lo;
u32 bufaddr_hi;
```

(`rl/if_rlreg.h:646-694`.) OWN=1 means the NIC owns the descriptor: for RX the
driver hands descriptors to the NIC with OWN set and the NIC clears it after
DMAing a packet in; for TX the driver sets OWN to hand a packet over and the
NIC clears it when the packet is on the wire. EOR marks the last descriptor in
the ring (wrap).

## Init order (the 8168G-correct sequence)

OpenBSD uses one ordering for every chip and it is the 8168G-correct one, so it
is the cleaner model to follow (`ic/re.c` `re_init` / `re_reset` / `re_attach`):

1. **PCI: enable MEM-space decode AND bus-master *before any MMIO access*.**
   FreeBSD does `pci_enable_busmaster` first (`re/if_re.c:1238`), then the
   `RF_ACTIVE` BAR allocation sets `PCI_COMMAND_MEM_ENABLE`. OpenBSD sets both
   command bits inside `pci_mapreg_map` (`pci/pci_map.c:349,351`) before
   returning the mapping. The chip reset is itself an MMIO write, so resetting
   before MEM-decode is on writes into the void **and every register read comes
   back `0xFF`** — which looks exactly like "the reset bit never clears."
   8168-family MMIO is **BAR2**, not BAR1 (`re/if_re.c:1252-1253`).
2. Read `RL_TXCFG (0x40)`, mask the hwrev field, identify the chip
   (`re/if_re.c:1391-1402`, `ic/re.c:673`). 8168G = `0x4C000000`.
3. **Reset**: write `RL_CMD_RESET (0x10)` to `RL_COMMAND (0x37)`; poll until it
   self-clears. Timeout is `RL_TIMEOUT(1000) × DELAY(10us)` = 10 ms
   (`re/if_re.c:739-743`, `ic/re.c:642-648`). The 8168G does **not** set
   `RL_FLAG_MACRESET`, so it needs **no** post-reset `0x82`/`RL_LDPS` write
   (that is for the old 8169/8110 only — `re/if_re.c:1523-1533`).
4. **PHY wake** (needed for link on 8168G — `RL_FLAG_PHYWAKE`): select PHY page
   0 then clear power-down, via the PHYAR register:
   `gmii_write(phy=1, reg=0x1f, 0)` then `gmii_write(1, 0x0e, 0)`
   (`re/if_re.c:1628-1631`, `ic/re.c:1058-1061`). Neither BSD loads any per-chip
   PHY firmware blob — **plain MII autonegotiation is enough for link**; the
   firmware tables only tune EEE/jumbo/signal-integrity.
5. **C+ command first** (`RL_CPLUS_CMD = 0xE0`, 16-bit) — comment in both:
   *"we must configure the C+ register before all others"*
   (`re/if_re.c:3154`, `ic/re.c:1900`). For 8168G (`RL_FLAG_MACSTAT`) the value
   is `PCI_MRW(0x08) | RXCSUM_ENB(0x20) | MACSTAT_DIS(0x80) | 0x0001` = `0x00A9`
   (add `VLANSTRIP 0x40` if VLAN). Do **not** set the C+ RX/TX-enable bits on
   the newer MAC (`re/if_re.c:3157-3168`, `ic/re.c:1904-1915`).
6. MAC address: read from the IDR0..5 registers (offset 0x00) on
   `RL_FLAG_PAR` chips (8168G qualifies — `re/if_re.c:1569-1575`).
7. Build the rings. RX: every descriptor gets bufaddr + buffer length + OWN, EOR
   on the last (`re/if_re.c:1995-2000`, `ic/re.c:1205-1212`). TX: zeroed,
   OWN=0, EOR on the last; OWN is set per-packet at transmit time.
8. Program the ring **base** addresses (256-byte aligned, 64-bit):
   RX → `RL_RXLIST_ADDR_LO/HI = 0xE4/0xE8`; TX → `RL_TXLIST_ADDR_LO/HI =
   0x20/0x24` (`re/if_re.c:3206-3214`, `ic/re.c:1957-1965`). NB: 0xE4/0x20 are
   the *list-base* registers, not a TX "start/kick" — the gigE TX kick is
   `RL_GTXSTART = 0x38` (`re/if_re.c:1594`).
9. **Clear the RXDV gate** on 8168G+ (`RL_FLAG_RXDV_GATED`):
   `RL_MISC (0xF0) &= ~0x00080000` (`re/if_re.c:3217-3219`, `ic/re.c:1967-1969`).
   With the gate set, receive-data-valid is gated off and RX never fires. It is
   re-asserted on stop/WOL.
10. `RL_TXCFG (0x40) = 0x03000700` (`IFG | MAXDMA 2048`).
11. `RL_EARLY_TX_THRESH (0xEC) = 16` (`re/if_re.c:3242`).
12. `RL_RXCFG (0x44) = 0x0000FF00 (FIFO no-thresh | DMA unlimited | bufsz)` plus
    accept bits `RX_INDIV(0x02) | RX_BROAD(0x08)` (+ multicast/promisc), plus
    **`EARLYOFFV2 = 0x00000800`** on 8168G (`re/if_re.c:686-687`,
    `ic/re.c:1981-1982`).
13. **Enable RX/TX LAST**: `RL_COMMAND (0x37) = RL_CMD_TX_ENB(0x04) |
    RL_CMD_RX_ENB(0x08)`. The single biggest 8168G difference: on G+ the MACs
    must be enabled *after* RXCFG/TXCFG, not before (FreeBSD splits this
    explicitly at `re/if_re.c:3222-3260`; OpenBSD always enables last at
    `ic/re.c:1988`). Older chips wanted it before.
14. `RL_MAXRXPKTLEN (0xDA)`; interrupt mask/status; `RL_CFG1 |= DRVLOAD(0x20)`.
    Config-register writes (`CFG1/2/5`, and the IDR MAC write) must be wrapped
    in the EEPROM-unlock: `RL_EECMD (0x50) = 0xC0` … write … `= 0x00`
    (`re/if_re.c:1560-1567,3197-3200`).
15. Kick autoneg (`mii_mediachg`) — link comes up asynchronously.

## PHY register access (PHYAR, 0x60)

Indirect MII access through `RL_PHYAR (0x60)`, a 32-bit register
(`rtl81x9reg.h:528-530`):

```
write: PHYAR = (reg << 16) | (data & 0xFFFF) | BUSY(0x80000000);
       poll until BUSY clears (RL_PHY_TIMEOUT × DELAY(25us)); then DELAY(20).
read : PHYAR = (reg << 16);
       poll until BUSY *sets*; then read low 16 bits.
```

(`re_gmii_writereg`/`re_gmii_readreg`, `ic/re.c`.) Note the asymmetry: write
waits for BUSY to clear, read waits for it to set.

## What Cardinal's driver was getting wrong

Confirmed live on the AtomicPi: `[RTL8169] Reset timed out.`

1. **`module_init` never enabled PCI memory-space decode** — only
   `command.busmaster = 1`. With MEM-decode off, every BAR read is `0xFF`, so
   the reset bit "never clears" and bring-up aborts. **This is the root cause of
   the reset timeout.** (MSI worked because MSI is configured through config
   space, not the BAR — so it did not rule this out.) Fix: also set
   `command.mem_space = 1` before touching `memar`.
2. No chip-version read — flying blind. Added an `RL_TXCFG` hwrev read + debug
   print.
3. C+ command register was `mmio16(0xE0) |= 3` (a read-modify-write that sets
   the *old-chip* RX/TX-enable bits off a possibly-garbage read). 8168G wants a
   direct write of `0x00A9` and the comment is emphatic that it comes first.
4. No PHY wake → on a powered-down PHY there is simply no link. Added the two
   `gmii_write` page-0/power-up pokes.
5. RX config was missing the 8168G `EARLYOFFV2 (0x800)` bit.

The descriptor-enable **ordering** (RXCFG/TXCFG before `CMD_TX_EN|CMD_RX_EN`)
and the RXDV-gate clear were already correct in the Cardinal driver.

## The firmware gaps on the AtomicPi (what actually blocked it)

The items above are real, but on the AtomicPi the NIC is **not used during
boot**, so its firmware leaves it far more unconfigured than a typical
just-needs-a-driver device. Each fix uncovered the next layer:

1. **PCI memory-space decode off** → every BAR read is `0xFF` → "reset timed
   out". (set `command.mem_space`).
2. **Device in D3hot.** A firmware-uninitialised RealTek NIC sits in D3: config
   space is readable (so the device ID is visible) but the memory BARs are dead
   and won't even size. Must transition to **D0** via the PM capability
   (PMCSR, clear PowerState bits) before touching registers.
3. **BARs unassigned.** Firmware never programmed the NIC's BARs — they read
   back as bare type bits (`bar0=0x1` I/O, `bar2=0x4` 64-bit mem, `bar4=0xc`
   prefetch), base 0. The driver must size BAR2 and assign it an address itself.
   Placement: scan the *assigned* neighbour BARs (this runs before APs are
   released, so sizing them is safe) and put ours 1 MiB-aligned just above the
   highest — guaranteed inside the host-bridge MMIO aperture and free. Cardinal
   has no PCI resource allocator, so this is the first BAR-programming path in
   the tree.
4. **The NIC is on a secondary bus behind a root-port bridge whose memory
   window is closed.** Its ECAM offset from the MCFG base decodes to bus > 0.
   With no BARs assigned, firmware also left the bridge's memory-forwarding
   window closed, so **no** MMIO address reaches the NIC's bus regardless of
   the NIC's own BAR — register reads stay `0xff`. The driver finds the bridge
   (the bus-0 type-1 device whose secondary-bus number == the NIC's bus) and
   opens its 1 MiB-granular non-prefetchable memory window over the assigned
   BAR, enabling the bridge's decode + bus-master. **This was the final
   blocker.**

## Verification (real RTL8111G on the AtomicPi)

Confirmed live over the serial bridge using an in-kernel peek/poke debug shell
(no QEMU model exists for the 8168). With the bring-up above:

- config space: vendor/device `10EC:8168`, hwrev (`TXCFG & 0x7C800000`) =
  `0x4C000000` = RTL8168G; PMCSR PowerState = D0; `command = 0x6`
  (mem+busmaster); BAR2 self-assigned (e.g. `0xA1800000`).
- `PHYstatus (0x6C) = 0x8B` → link up, 100 Mbps, full duplex.
- **RX**: `ISR (0x3E)` ROK set; missed-packet counter 0; packets received.
- **TX**: wrote a 60-byte frame to the TX buffer, set descriptor 0
  `OWN|FS|LS|len`, kicked TPPoll (`0x38 ← 0x40 NPQ`) → `ISR` TOK set and the
  descriptor's OWN bit cleared. Frame transmitted.

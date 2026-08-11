# tt-whisper-mmu

This is a convenience class for the MMU test bench. It packages Whisper VirtMem,
PmpManager, PmaManager, and a simple memory model into one class that serves as a
reference model for address translation.

## Cloning
```
git clone --recursive https://github.com/tenstorrent/tt-whisper-mmu.git
```

## Compiling

You need g++ version 11 or later.

Issue the following command to create the libdvmmu.a library:
```
make
```
The above should compile the libdvmmu.a library.

To compile the sample program, issue:
```
make sample
```

## Usage Model

The DvMmu class models a physical memory and a RISC-V address translation engine.  Given a
set of page table entries placed in physical memory, the address translation engine will
translate a virtual address to a physical address or produce an exception code if the
translation is not possible. The page table entries are placed in memory using the
memWrite method.  Here's an example of creating and writing an SV39 to memory:

```
    DvMmu mmu(0x80000000 /*memory size*/);

    // Configure for PTE A/D bits maintained by hardware.
    mmu.setFaultOnFirstAccess(false);

    uint64_t rootAddr = 0x20000;                  // Root page table address.
    uint64_t rootPpn = mmu.pageNumber(rootAddr);  // Root page table number.

    Pte39 pte39{0};
    pte39.bits_.valid_ = true;
    pte39.bits_.read_ = true;
    pte39.bits_.user_ = false;
    pte39.bits_.ppn2_ = 0xaa;
    pte39.bits_.ppn1_ = 0;
    pte39.bits_.ppn0_ = 0;

    if (not mmu.memWrite(rootAddr, sizeof(pte39), pte39.data_))
       assert(0);
```

The configTranslation method should be called whenever the SATP CSR changes or it can be
called before every translation. In this example, we are configuring the engine for SV39
with an ASID of 0 and root page table address of 0x20000 (rootPpn specified in the code
above):

```
    mmu.configTranslation(VirtMem::Mode::Sv39, 0 /*asid*/, rootPpn);
```

Once the engine is configured according to some SATP CSR value, translation can be
attempted for read, write, or fetch (execute) access. Here's an example for read access:

```
    uint64_t va = 0, gpa = 0, pa = 0;  // Gpa used only if twoStage is true.
    auto twoStage = false;
    auto r = true, w = false, x = false;  // Exactly one of these must be true.

    // Translate va into pa for read access and supervisor privilege mode.
    auto ec = mmu.translate(va, PrivilegeMode::Supervisor, twoStage, r, w, x, gpa, pa);
    assert(ec == ExceptionCause::NONE);
```

## Configuring Physical Memory Protection

Physical Memory Protection (PMP) is modeled by mapping the RISC-V `pmpcfg` and
`pmpaddr` CSRs to memory-mapped register addresses and then writing them the
same way the target would. Calling `definePmpRegs` places the registers and
enables PMP enforcement; subsequent translations reject accesses that fall
outside the granted region permissions.

```
// Map 2 pmpcfg registers at 0x1000 and 16 pmpaddr registers at 0x2000.
// Each register is 8 bytes, so register i is at base + i*8.
mmu.definePmpRegs(0x1000 /*pmpcfg base*/, 2 /*count*/,
                  0x2000 /*pmpaddr base*/, 16 /*count*/);

// Program pmpaddr0..2 (written values are legalized as the CSRs would be).
mmu.mmrWrite(0x2000, 0x400);   // pmpaddr0
mmu.mmrWrite(0x2008, 0x400);   // pmpaddr1
mmu.mmrWrite(0x2010, 0x400);   // pmpaddr2

// Program pmpcfg0 (packs eight per-region config bytes: R/W/X, A, L).
mmu.mmrWrite(0x1000, /* pmpcfg value */);
```

Registers are read back with `mmrRead`. Once configured, `translate` returns an
access-fault exception cause for an access that violates the configured PMP
permissions.

## Configuring Physical Memory Attributes

Physical Memory Attributes (PMA) are configured the same way via
`definePmaRegs`, which maps the `pmacfg` (and, optionally, `pmamask`) registers
and enables PMA checking:

```
// Map 16 pmacfg registers at 0x7e0 (no pmamask registers in this example).
// Signature: definePmaRegs(cfgAddr, cfgCount, pmamaskAddr = 0, pmamaskCount = 0)
mmu.definePmaRegs(0x7e0, 16);

// Program pmacfg0 with the region's base/size and permissions.
mmu.mmrWrite(0x7e0, 0xb8000000000000e7);
```

As with PMP, written values are legalized, and a translated physical address
that falls outside every defined PMA region — or lacks the required read, write,
or execute attribute — causes the translation to fail.

## Contributing

Bugs are reported via [GitHub Issues](../../issues); bug fixes and new
functionality are submitted via Pull Requests, which are reviewed on a weekly
cadence. See [CONTRIBUTING.md](CONTRIBUTING.md) for details, and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community expectations. To report a
security vulnerability, follow [SECURITY.md](SECURITY.md) instead of opening a
public issue.

## License

| File | Applies to |
|------|------------|
| [LICENSE](LICENSE) (Apache License 2.0) | Overall license for this project, except where specified |
| [LICENSE_understanding.txt](LICENSE_understanding.txt) | Tenstorrent's clarification of how the Apache 2.0 license applies to this repository |

The `whisper/` submodule is a third-party dependency and retains its own
upstream license; see [NOTICE](NOTICE) for attribution.

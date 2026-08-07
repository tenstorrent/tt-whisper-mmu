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

## Configuring Physical Memory Protection.

## Configuring Physical Memory Attributes.

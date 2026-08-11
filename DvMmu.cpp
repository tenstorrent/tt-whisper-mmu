// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "DvMmu.hpp"


using namespace TT_DV_MMU;


DvMmu::DvMmu(uint64_t memSize)
  : memSize_(memSize),
    mmu_(0 /*hartIx*/, 4096 /*pageSize*/, 0 /*tlbSize*/),
    mem_(memSize),
    pmaMgr_(memSize)
{
  initialize();
}


void
DvMmu::initialize()
{
  // Enable PMP by defining the addresses of its controlling registers.
  unsigned pmpaddrCount = 0;   // Count of PMPADDR regs.  Zero if PMP is disabled.
  uint64_t pmpaddrAddr = 0;    // Address of 1st PMPADDR reg.
  unsigned pmpcfgCount = 0;    // Count of PMPCFG regs (should be pmpaddrCount/8).
  uint64_t pmpcfgAddr = 0;     // Address of 1st PMPCFG reg.
  definePmpRegs(pmpcfgAddr, pmpcfgCount, pmpaddrCount, pmpaddrAddr);

  // Enabe PMA by defining the addresses of its controlling registers.
  unsigned pmacfgCount = 0;    // Count of PMACFG regs. Zero if PMA is disabled.
  uint64_t pmacfgAddr = 0;     // Address of 1st PMACFG reg.
  unsigned pmamaskCount = 0;   // Count of PMAMASK regs. Zero if disabled.
  uint64_t pmamaskAddr = 0;    // Address of 1st PMAMASK reg.
  definePmaRegs(pmacfgAddr, pmacfgCount, pmamaskAddr, pmamaskCount);

  // PMA default no-access: in-range PA with no configured region faults (matches RTL), not RWX.
  pmaMgr_.clearDefaultPma();

  mmu_.enableTrace(true);  // So that we can recover page table walk details.

  // A/D bits are not maintained by hardware. Config mmu_ accordingly.
  mmu_.setFaultOnFirstAccess(true);
  mmu_.setFaultOnFirstAccessStage1(true);
  mmu_.setFaultOnFirstAccessStage2(true);

  // Setup callbacks for VirtMem.

  // Read callback to be used by MMU for explicit reads.
  mmu_.setMemReadCallback([this](uint64_t addr, bool bigEndian, unsigned size, uint64_t &data) -> bool {
    assert(size == sizeof(data));
    if (not memRead(addr, size, data))
      return false;
    if (bigEndian)
      data = __builtin_bswap64(data);
    return true;
  });

  // Write callback to be used by MMU for explicit writes.
  std::function<bool(uint64_t, bool, unsigned, uint64_t)> wcb = [this](uint64_t addr, bool bigEndian, unsigned size, uint64_t data) -> bool {
    assert(size == sizeof(data));
    if (bigEndian)
      data = __builtin_bswap64(data);
    return memWrite(addr, size, data);
  };

  mmu_.setMemWriteCallback(wcb);

  // Read check to be used by MMU to check explcit reads.
  mmu_.setIsReadableCallback([this](uint64_t addr) -> bool {
    auto pm = PrivilegeMode::User;
    if (not isPmpReadable(addr, pm) or not isPmaReadable(addr))
      return false;
    if (isPmaEnabled())
      return isPmaReadable(addr);
    return true;
  });

  // Write check to be used by MMU to check explcit writes.
  mmu_.setIsWritableCallback([this](uint64_t addr) -> bool {
    auto pm = PrivilegeMode::User;
    if (not isPmpWritable(addr, pm) or not isPmaWritable(addr))
      return false;
    if (isPmaEnabled())
      return isPmaWritable(addr);
    return true;
  });

}


bool
DvMmu::definePmpRegs(uint64_t cfgAddr, unsigned cfgCount,
                     uint64_t addrAddr, unsigned addrCount)
{
  if (cfgCount == 0 and addrCount == 0)
    {
      pmpcfgCount_ = cfgCount;
      pmpaddrCount_ = addrCount;
      pmpEnabled_ = false;
      return true;
    }

  if (addrCount != 8 and addrCount != 16 and addrCount != 64)
    {
      std::cerr << "Invalid IOMMU PMPADDR count: " << addrCount << " -- expecting 8, 16, or 64\n";
      return false;
    }

  if ((addrCount / 8) != cfgCount)
    {
      std::cerr << "Invalid IOMMU PMPCFG count: " << cfgCount << " -- expecting "
                << (addrCount / 8) << '\n';
      return false;
    }

  if ((cfgAddr & 7) != 0)
    {
      std::cerr <<  "Invalid IOMMU PMPCFG address: 0x" << std::hex << cfgAddr << std::dec
                << " must be double-word aligned\n";
      return false;
    }

  if ((addrAddr & 7) != 0)
    {
      std::cerr << "Invalid IOMMU PMPADDR address: 0x " << std::hex << addrAddr << std::dec
                << " must be double-word aligned\n";
      return false;
    }

  pmpcfgCount_ = cfgCount;
  pmpaddrCount_ = addrCount;
  pmpcfgStart_ = cfgAddr;
  pmpaddrStart_ = addrAddr;

  pmpcfg_.clear();
  pmpcfg_.resize(pmpcfgCount_);

  pmpaddr_.clear();
  pmpaddr_.resize(pmpaddrCount_);

  pmpEnabled_ = true;
  return true;
}


void
DvMmu::updateMemoryProtection()
{
  pmpMgr_.reset();

  for (unsigned ix = 0; ix < pmpaddrCount_; ++ix)
    {
      uint64_t low = 0, high = 0;

      uint8_t cfgByte = getPmpcfgByte(ix);
      uint64_t val = pmpaddr_.at(ix);
      uint64_t precVal =  (ix == 0) ? 0 : pmpaddr_.at(ix - 1);  // Preceding PMPADDR reg.

      WdRiscv::Pmp pmp;
      pmpMgr_.unpackMemoryProtection(cfgByte, val, precVal, false /*rv32*/, pmp, low, high);

      if (pmp.type() != WdRiscv::Pmp::Type::Off)
        pmpMgr_.defineRegion(low, high, pmp, ix);
    }
}


bool
DvMmu::definePmaRegs(uint64_t cfgAddr, unsigned cfgCount,
                     uint64_t maskAddr, unsigned maskCount)
{
  // PMACFG window.
  if (cfgCount == 0)
    {
      pmacfgCount_ = 0;
      pmaEnabled_ = false;
    }
  else
    {
      if ((cfgAddr & 7) != 0)
        {
          std::cerr << "Invalid IOMMU PMACFG address: " << std::hex << cfgAddr << std::dec
                    << " must be double-word aligned\n";
          return false;
        }
      pmacfgCount_ = cfgCount;
      pmacfgStart_ = cfgAddr;
      pmacfg_.clear();
      pmacfg_.resize(pmacfgCount_);
      pmaEnabled_ = true;
    }

  // PMAMASK window.
  if (maskCount == 0)
    {
      pmamaskCount_ = 0;
      pmamaskEnabled_ = false;
    }
  else
    {
      if ((maskAddr & 7) != 0)
        {
          std::cerr << "Invalid IOMMU PMAMASK address: " << std::hex << maskAddr << std::dec
                    << " must be double-word aligned\n";
          return false;
        }
      pmamaskCount_ = maskCount;
      pmamaskStart_ = maskAddr;
      pmamask_.clear();
      pmamask_.resize(pmamaskCount_, 0);
      pmamaskEnabled_ = true;
    }

  return true;
}


void
DvMmu::updateMemoryAttributes(unsigned pmacfgIx)
{
  uint64_t val = pmacfg_.at(pmacfgIx);

  uint64_t low = 0, high = 0, mask = 0;
  WdRiscv::Pma pma;

  if (pmaMgr_.unpackPmacfg(val, low, high, mask, pma))
    {
      if (not pmaMgr_.defineRegion(pmacfgIx, low, high, pma))
        assert(0);
      pmaMgr_.setAddressMask(pmacfgIx, mask);
    }
  else
    pmaMgr_.invalidateEntry(pmacfgIx);  // (mirrors Hart::processPmacfgChange).

  mmu_.flushPteCache();  // PMA regions changed -> cached PTE access results stale.
}


// Fold the stored PMAMASK don't-care bits into region ix's address mask
// (port of Hart::processPmamaskChange).
void
DvMmu::applyPmaMask(unsigned ix)
{
  if (ix >= pmamask_.size() or ix >= pmacfg_.size())
    return;

  uint64_t m = ~pmamask_.at(ix);   // bit interpretation is reversed in PmaManager
  m = (m >> 12) << 12;             // clear least significant 12 bits
  m = (m << 12) >> 12;             // clear most significant 12 bits

  uint64_t low = 0, high = 0, cfgMask = 0;
  WdRiscv::Pma pma;
  if (pmaMgr_.unpackPmacfg(pmacfg_.at(ix), low, high, cfgMask, pma))
    m &= cfgMask;

  pmaMgr_.setAddressMask(ix, m);
  mmu_.flushPteCache();            // PMA mask change -> cached PTE results stale
}

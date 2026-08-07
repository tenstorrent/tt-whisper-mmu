#pragma once

#include "virtual_memory/VirtMem.hpp"
#include "virtual_memory/trapEnums.hpp"
#include "PmpManager.hpp"
#include "PmaManager.hpp"
#include "MemModel.hpp"


namespace TT_DV_MMU
{

  ///
  /// This is a convenience class that puts together a virtual to physical address
  /// translation object (VirtMem) and a simple physical memory model.
  ///
  /// The virtual-to-physical address translation class, VirtMem, is defined in
  /// whisper/virtual_memory/VirtMem.hpp.
  ///
  class DvMmu
  {
  public:

    using PrivilegeMode = WdRiscv::PrivilegeMode;
    using ExceptionCause = WdRiscv::ExceptionCause;

    /// Constructor.
    DvMmu(uint64_t memSize);

    /// Perform an address translation request. Return ExceptionCause::None on succes;
    /// otherwise, return the exception cause.
    ExceptionCause translate(uint64_t va, PrivilegeMode pm, bool twoStage, bool read,
                             bool write, bool exec, uint64_t& gpa, uint64_t& pa)
    {
      unsigned count = read + write + exec;
      assert(count == 1);
      isFetch_ = exec;
      mmu_.setAccReason(isFetch_);

      mmu_.clearPageTableWalk();

      using EC = ExceptionCause;
      auto ec = EC::NONE;
      finalStageEc_ = EC::NONE;
      if (pm == PrivilegeMode::Machine)
        gpa = pa = va;
      else
        ec = mmu_.translate(va, pm, twoStage, read, write, exec, gpa, pa);

      // If VS stage completed but we encoutered a fault because of permission, then
      // perform the GPA to SPA translation. We do this to match the RTL.
      // Stage-1 (VS) walk: result is a GPA, so run GPA->SPA and check the final SPA.
      // Otherwise (bare-VS/single-stage) the result is already the SPA; check it directly.
      if (ec != EC::NONE)
        {
          auto walks = getPageTableWalks();
          if (not walks.empty() and walks.at(0).complete())
            {
              if (walks.at(0).isStage1())
                {
                  gpa = walks.at(0).result();
                  finalStageEc_ = mmu_.stage2Translate(gpa, pm, read, write, exec, false /*isPteAddr*/, pa);

                  // stage2Translate may fault on a final-G-leaf perm (U/D); its walk still recorded the resolved SPA
                  auto gwalks = getPageTableWalks();
                  if (not gwalks.empty() and gwalks.back().complete())
                    {
                      uint64_t spa = gwalks.back().result();
                      if (spa >= memSize_ or not checkPmaRegion(spa) or not checkPmpRegion(pm, spa))
                        {
                          finalStageEc_ = EC::INST_ACC_FAULT;
                          if (read)
                            finalStageEc_ = EC::LOAD_ACC_FAULT;
                          else if (write)
                            finalStageEc_ = EC::STORE_ACC_FAULT;
                        }
                    }
                }
              else
                {
                  // Bare-VS (stage-2) or single-stage: result is already the SPA.
                  uint64_t spa = walks.at(0).result();
                  if (spa >= memSize_ or not checkPmaRegion(spa) or not checkPmpRegion(pm, spa))
                    {
                      finalStageEc_ = EC::INST_ACC_FAULT;
                      if (read)
                        finalStageEc_ = EC::LOAD_ACC_FAULT;
                      else if (write)
                        finalStageEc_ = EC::STORE_ACC_FAULT;
                    }
                }
            }
        }

      if (ec == EC::NONE and pa >= memSize_)
        {
          ec = EC::INST_ACC_FAULT;
          if (read)
            ec = EC::LOAD_ACC_FAULT;
          else if (write)
            ec = EC::STORE_ACC_FAULT;
        }

      if (ec == EC::NONE and (not checkPmaRegion(pa) or not checkPmpRegion(pm, pa)))
        {
          ec = EC::INST_ACC_FAULT;
          if (read)
            ec = EC::LOAD_ACC_FAULT;
          else if (write)
            ec = EC::STORE_ACC_FAULT;
        }

      return ec;
    }

    /// Check the physical memory protection (PMP) for the given physical address,
    /// privilege mode, and read/write/execute access permissions (typically one of which
    /// would be true but this handles multiple permissions being true). Return true if
    /// the required access permissions are allowed and false otherwise. If no permission
    /// is required (read/write/exec all false) or if PMP is not enabled, then return
    /// true.
    bool checkPmp(PrivilegeMode pm, uint64_t pa, bool read, bool write, bool exec) const
    {
      if (not pmpEnabled_)
        return true;
      
      const auto& pmp = pmpMgr_.getPmp(pm, pa);

      if (read and not pmp.isRead())
        return false;
      if (write and not pmp.isWrite())
        return false;
      if (exec and not pmp.isExec())
        return false;

      return true;

    }

    /// Check the physical memory attributes (PMA) for the given physical address and
    /// read/write/execute access permissions (typically one of which would be true but
    /// this handles multiple permissions being true). Return true if the required access
    /// permissions are allowed and false otherwise. If no permission is required
    /// (read/write/exec all false) or if PMA is not enabled, then return true.
    bool checkPma(uint64_t pa, bool read, bool write, bool exec) const
    {
      if (not pmaEnabled_)
        return true;
      
      auto pma = pmaMgr_.getPma(pa);
   
      if (read and not pma.isRead())
        return false;
      if (write and not pma.isWrite())
        return false;
      if (exec and not pma.isExec())
        return false;

      return true;
    }

    /// Region hit/miss check against PMA for the final physical address: true if the
    /// address lies in a mapped PMA region (ignores R/W/X). True if PMA is disabled.
    bool checkPmaRegion(uint64_t pa) const
    {
      if (not pmaEnabled_)
        return true;

      return pmaMgr_.overlaps(pa);   // region exists (PmaHit), ignores R/W/X
    }

    /// Region hit/miss check against PMP for the final physical address: true if a PMP
    /// region matches the address (ignores R/W/X). Machine mode or disabled PMP return true.
    bool checkPmpRegion(PrivilegeMode pm, uint64_t pa) const
    {
      if (not pmpEnabled_)
        return true;
      if (pm == PrivilegeMode::Machine)
        return true;

      const auto& pmp = pmpMgr_.getPmp(pm, pa);
      return pmp.val() != 0;
    }

    /// Configure stage1 translation. Should be done whenever VSTAP changes. Mode is the
    /// translation mode representing SV39, SV48, or SV57.  Ppn is the guest physical page
    /// number of the root page.  Sum indicates whether or not Supervisor mode can read
    /// User mode pages.
    void configStage1(WdRiscv::VirtMem::Mode mode, unsigned asid, uint64_t ppn, bool sum)
    { mmu_.configStage1(mode, asid, ppn, sum); }

    /// Configure stage2 translation. Should be done whenever HGSTAP changes. Mode is the
    /// translation mode representing SV39, SV48, or SV57. Ppn is the physical page
    /// number of the root page.
    void configStage2(WdRiscv::VirtMem::Mode mode, unsigned asid, uint64_t ppn)
    { mmu_.configStage2(mode, asid, ppn); }

    /// Configure non-hypervisor translation. Should be done whenever SATP changes. Mode
    // is the / translation mode code representing SV39, SV48, or SV57.  Ppn is the
    // physical page / number of the root page.
    void configTranslation(WdRiscv::VirtMem::Mode mode, uint32_t asid, uint64_t ppn)
    { mmu_.configTranslation(mode, asid, ppn); }

    /// Config MMU as spporting A/D PTE bits update by hardware if flag is true;
    /// otherwise, MMU will return a page fault exception on first access or first write
    /// when PTE is clean.
    void setFaultOnFirstAccess(bool flag)
    { mmu_.setFaultOnFirstAccess(flag); }

    /// Same as above but for 1st stage of a 2-stage translation.
    void setFaultOnFirstAccessStage1(bool flag)
    { mmu_.setFaultOnFirstAccessStage1(flag); }

    /// Same as above but for 2st stage of a 2-stage translation.
    void setFaultOnFirstAccessStage2(bool flag)
    { mmu_.setFaultOnFirstAccessStage2(flag); }

    /// Enable/disable page-based-memory types.
    void enablePbmt(bool flag)
    { mmu_.enablePbmt(flag); }

    /// Enable/disable page-based-memory types at VS stage.
    void enableVsPbmt(bool flag)
    { mmu_.enableVsPbmt(flag); }

    /// Enable/disable NAPOT page size (naturally aligned power of 2).
    void enableNapot(bool flag)
    { mmu_.enableNapot(flag); }

    /// Return the nominal memory page size (typically 4096).
    unsigned pageSize() const
    { return mem_.pageSize(); }

    /// Return the number of the page corresponding to the given address (page number is
    /// page address divided by page size).
    unsigned pageNumber(uint64_t addr) const
    { return mem_.pageNumber(addr); }

    /// Read size bytes from system memory at the given address putting results in
    /// data. Size must be less than or equal to 8. Return true on succes and false on
    /// failure.
    bool memRead(uint64_t addr, unsigned size, uint64_t& data)
    {
      if (memRead_)
        return memRead_(addr, size, data);
      return mem_.read(addr, size, data);
    }

    /// Write size bytes to memory at the given address getting the byte values from data.
    /// Size must be less than or equal to 8. Return true on success and false on failure.
    bool memWrite(uint64_t addr, unsigned size, uint64_t data)
    {
      if (memWrite_)
        return memWrite_(addr, size, data);
      return mem_.write(addr, size, data);
    }

    /// Read a PMA/PMP memory mapped register putting the result in data. Address must be
    /// double word aligned and must belong to one of the address ranges specified by
    /// definePmpRegs or definePmaRegs. Return true on success and false on failure.  Note
    /// that the PMA/PMP registers are 8 bytes each, so the address of the ith register is
    /// the base address + i*8, For example, if the PMA registers were defined at base
    /// address 0x1000, the the address of PMACFG2 would be 0x1000 + 2*8.
    bool mmrRead(uint64_t addr, uint64_t& data) const
    {
      if (not isPmpRegAddr(addr) and not isPmaRegAddr(addr))
        return false;

      const unsigned expSize = 8;  // Expected size/alignment.
      if ((addr & (expSize - 1)) != 0)
        return false;

      if (isPmpcfgAddr(addr))
        {
          unsigned ix = (addr - pmpcfgStart_) / expSize;
          data = pmpcfg_.at(ix);
          return true;
        }

      if (isPmpaddrAddr(addr))
        {
          unsigned ix = (addr - pmpaddrStart_) / expSize;
          data = pmpaddr_.at(ix);
          // Adjust PMPADDR value according to type in corresponding PMPCFG.
          unsigned byte = getPmpcfgByte(ix);
          data = pmpMgr_.adjustPmpValue(data, byte, false /*rv32*/);
          return true;
        }

      if (isPmacfgAddr(addr))
        {
          unsigned ix = (addr - pmacfgStart_) / expSize;
          data = pmacfg_.at(ix);
          return true;
        }

      if (isPmamaskAddr(addr))
        {
          unsigned ix = (addr - pmamaskStart_) / expSize;
          data = pmamask_.at(ix);
          return true;
        }

      assert(0);
      return false;
    }

    /// Write a PMA/PMP memory mapped register with the value in data. Address must be
    /// double word aligned and must belong to one of the address ranges specified by
    /// definePmpRegs or definePmaRegs. Return true on success and false on failure. Note
    /// that the PMA/PMP registers are 8 bytes each, so the address of the ith register is
    /// the base address + i*8, For example, if the PMA registers were defined at base
    /// address 0x1000, the the address of PMACFG2 would be 0x1000 + 2*8.
    bool mmrWrite(uint64_t addr, uint64_t data)
    {
      if (not isPmpRegAddr(addr) and not isPmaRegAddr(addr))
        return false;

      const unsigned expSize = 8;  // Expected size/alignment.
      if ((addr & (expSize - 1)) != 0)
        return false;

      if (isPmpcfgAddr(addr))
        {
          unsigned ix = (addr - pmpcfgStart_) / expSize;
          uint64_t prev = pmpcfg_.at(ix);
          data = pmpMgr_.legalizePmpcfg(prev, data);
          pmpcfg_.at(ix) = data;
          updateMemoryProtection();
          return true;
        }

      if (isPmpaddrAddr(addr))
        {
          unsigned ix = (addr - pmpaddrStart_) / expSize;
          pmpaddr_.at(ix) = data;

          uint8_t cfgByte =  getPmpcfgByte(ix);
          if (((cfgByte >> 3) & 3) != 0)   // If type != Off
            updateMemoryProtection();
          return true;
        }

      if (isPmacfgAddr(addr))
        {
          unsigned ix = (addr - pmacfgStart_) / expSize;
          uint64_t prev = pmacfg_.at(ix);
          data = pmaMgr_.legalizePmacfg(prev, data);
          pmacfg_.at(ix) = data;
          updateMemoryAttributes(ix);
          return true;
        }

      if (isPmamaskAddr(addr))
        {
          unsigned ix = (addr - pmamaskStart_) / expSize;
          pmamask_.at(ix) = data;   // no CSR write-mask legalization in DvMmu
          applyPmaMask(ix);
          return true;
        }

      assert(0);
      return false;
    }

    /// Read the value of the ith PMACFG register returning true on success and false if i
    /// is out of bounds.
    bool readPmacfgReg(unsigned i, uint64_t& value) const
    {
      if (not pmaEnabled_)
        return false;
      uint64_t addr = pmacfgStart_ + i*8;
      return mmrRead(addr, value);
    }

    /// Write the ith PMACFG register with the given value returning true on success and
    /// false if i is out of bounds.
    bool writePmacfgReg(unsigned i, uint64_t value)
    {
      if (not pmaEnabled_)
        return false;
      uint64_t addr = pmacfgStart_ + i*8;
      return mmrWrite(addr, value);
    }

    /// Read the value of the ith PMPADDR register returning true on success and false if
    /// i is out of bounds.
    bool readPmpaddrReg(unsigned i, uint64_t& value) const
    {
      if (not pmpEnabled_)
        return false;
      uint64_t addr = pmpaddrStart_ + i*8;
      return mmrRead(addr, value);
    }

    /// Write the ith PMPADDR register with the given value returning true on success and
    /// false if i is out of bounds.
    bool writePmpaddrReg(unsigned i, uint64_t value)
    {
      if (not pmpEnabled_)
        return false;
      uint64_t addr = pmpaddrStart_ + i*8;
      return mmrWrite(addr, value);
    }

    /// Read the value of the ith PMPCFG register returning true on success and false if
    /// i is out of bounds.
    bool readPmpcfgReg(unsigned i, uint64_t& value) const
    {
      if (not pmpEnabled_)
        return false;
      uint64_t addr = pmpcfgStart_ + i*8;
      return mmrRead(addr, value);
    }

    /// Write the ith PMPCFG register with the given value returning true on success and
    /// false if i is out of bounds.
    bool writePmpcfgReg(unsigned i, uint64_t value)
    {
      if (not pmpEnabled_)
        return false;
      uint64_t addr = pmpcfgStart_ + i*8;
      return mmrWrite(addr, value);
    }

    /// By default this object will use its own memory model. The user can redirect memory
    /// read/write to a different memory model by invoking this method (once after
    /// construction) to provide a callback for memory read. The provided callback will be
    /// used by this object to read physical memory. The callback should perform PMA/PMP
    /// checks and return true on success (setting data to the read value) and false on
    /// failure.
    void setMemReadCb(const std::function<bool(uint64_t addr, unsigned size, uint64_t& data)>& cb)
    { memRead_ = cb; }

    /// Similar fo setMemReadCb but for write access. The callback should perform PMA/PMP
    /// checks and return true on success and false on failure.
    void setMemWriteCb(const std::function<bool(uint64_t addr, unsigned size, uint64_t data)>& cb)
    { memWrite_ = cb; }

    /// Define the physical memory protection registers (pmp-config regs and pmp-addr
    /// regs). The registers are memory mapped at the given addresses.
    /// Return true on success and false on failure (addresses not double word aligned,
    /// counts are too large, counts are not consistent...).
    bool definePmpRegs(uint64_t pmpcfgAddr, unsigned pmpcfgCount,
                       uint64_t pmpaddrAddr, unsigned pmpaddrCount);

    /// Define the physical memory attribute registers (PMACFG).  The registers are memory
    /// mapped at the given address.  Return true on success and false on failure (address
    /// is not double word aligned, count too large...).
    bool definePmaRegs(uint64_t pmacfgAddr, unsigned pmacfgCount,
                       uint64_t pmamaskAddr = 0, unsigned pmamaskCount = 0);

    /// Return true if physical memory attributes are enabled.
    bool isPmaEnabled() const
    { return pmaEnabled_; }

    /// Return true if physical memory protection is enabled.
    bool isPmpEnabled() const
    { return pmpEnabled_; }

    /// If physical memory protection is not enabled, return true; otherwise, return true
    /// if the PMP grants read access for the given address and privilege mode.
    bool isPmpReadable(uint64_t addr, PrivilegeMode mode) const
    {
      if (not pmpEnabled_)
        return true;
      auto pmp = pmpMgr_.getPmp(mode, addr);
      return pmp.isRead();
    }

    /// If physical memory protection is not enabled, return true; otherwise, return true
    /// if the PMP grants read access for the given address and privilege mode.
    bool isPmpWritable(uint64_t addr, PrivilegeMode mode) const
    {
      if (not pmpEnabled_)
        return true;
      auto pmp = pmpMgr_.getPmp(mode, addr);
      return pmp.isWrite();
    }

    /// If physical memory attribute is not enabled, return true; otherwise, return true
    /// if the PMA grants read access for the given address.
    bool isPmaReadable(uint64_t addr) const
    {
      if (not pmaEnabled_)
        return true;
      auto pma = pmaMgr_.getPma(addr);
      return pma.isRead();
    }

    /// If physical memory attribute is not enabled, return true; otherwise, return true
    /// if the PMA grants read access for the given address.
    bool isPmaWritable(uint64_t addr) const
    {
      if (not pmaEnabled_)
        return true;
      auto pma = pmaMgr_.getPma(addr);
      return pma.isWrite();
    }

    /// Return true if the given address is in the physical memory protection (PMP) memory
    /// mapped registers associated with this IOMMU.
    bool isPmpRegAddr(uint64_t addr) const
    { return isPmpcfgAddr(addr) or isPmpaddrAddr(addr); }

    /// Return true if given address in the region associated with the physical memory
    /// protection configuration registers (PMPCFG).
    bool isPmpcfgAddr(uint64_t addr) const
    {
      if (pmpEnabled_)
        return addr >= pmpcfgStart_ and addr < pmpcfgStart_ + pmpcfgCount_ * 8;
      return false;
    }

    /// Return true if given address in the region associated with the physical memory
    /// protection address registers (PMPADDR).
    bool isPmpaddrAddr(uint64_t addr) const
    {
      if (pmpEnabled_)
        return addr >= pmpaddrStart_ and addr < pmpaddrStart_ + pmpaddrCount_ * 8;
      return false;
    }

    /// Return true if the given address is in the physical memory attribute (PMA) memory
    /// mapped registers associated with this IOMMU.
    bool isPmaRegAddr(uint64_t addr) const
    { return isPmacfgAddr(addr) or isPmamaskAddr(addr); }

    /// Return true if given address in the region associated with the physical memory
    /// attribute configuration registers (PMACFG).
    bool isPmacfgAddr(uint64_t addr) const
    {
      if (pmaEnabled_)
        return addr >= pmacfgStart_ and addr < pmacfgStart_ + pmacfgCount_ * 8;
      return false;
    }

    /// Return true if given address is in the region associated with the PMAMASK registers.
    bool isPmamaskAddr(uint64_t addr) const
    {
      if (pmamaskEnabled_)
        return addr >= pmamaskStart_ and addr < pmamaskStart_ + pmamaskCount_ * 8;
      return false;
    }

    using Walk = WdRiscv::VirtMem::Walk;

    /// Return the page table walk(s) of the last translation.
    const std::vector<Walk>& getPageTableWalks() const
    { return isFetch_ ? mmu_.getFetchWalks() : mmu_.getDataWalks(); }

    /// Cause of the continued GPA->SPA translation after a VS-stage leaf fault (NONE if it did not run).
    int lastFinalStageCause() const { return static_cast<int>(finalStageEc_); }

  protected:

    /// Return the configuration byte of a PMPCFG register corresponding to the PMPADDR
    /// register having the given index (index 0 corresponds to PMPADDR0). Given index
    /// must not be out of bouds.
    uint8_t getPmpcfgByte(unsigned pmpaddrIx) const
    {
      assert(pmpaddrIx < pmpaddrCount_);
      unsigned cfgIx = pmpaddrIx / 8;  // 1 PMPCFG reg for 8 PMPADDR regs.
      uint64_t cfgVal = pmpcfg_.at(cfgIx);
      unsigned cfgByteIx = pmpaddrIx % 8;
      uint8_t cfgByte = cfgVal >> (8*cfgByteIx);
      return cfgByte;
    }

    void initialize();

    void updateMemoryProtection();

    void updateMemoryAttributes(unsigned pmacfgIx);

    // Fold the stored PMAMASK don't-care bits into region ix's address mask
    // (port of Hart::processPmamaskChange).
    void applyPmaMask(unsigned ix);

  private:

    uint64_t memSize_ = 0;
    WdRiscv::VirtMem mmu_;
    MemModel mem_;

    std::function<bool(uint64_t addr, unsigned size, uint64_t& data)> memRead_ = nullptr;
    std::function<bool(uint64_t addr, unsigned size, uint64_t data)> memWrite_ = nullptr;

    bool pmpEnabled_ = false;        // Physical memory protection (PMP)
    bool isFetch_ = false;           // True if last translate was for exec permission.
    ExceptionCause finalStageEc_ = ExceptionCause::NONE;  // Cause of continued GPA->SPA translation.

    uint64_t pmpcfgCount_ = 0;       // Number of PMPCFG registers
    uint64_t pmpaddrCount_ = 0;      // Number of PMPADDR registers
    uint64_t pmpcfgStart_ = 0;       // Address of first PMPCFG register
    uint64_t pmpaddrStart_ = 0;      // Address of first PMPADDR register

    std::vector<uint64_t> pmpcfg_;   // Cached values of PMPCFG registers
    std::vector<uint64_t> pmpaddr_;  // Cached values of PMPADDR registers

    WdRiscv::PmpManager pmpMgr_;

    bool pmaEnabled_ = false;        // Physical memory attributes (PMA)
    uint64_t pmacfgCount_ = 0;       // Count of PMACFG registers
    uint64_t pmacfgStart_ = 0;       // Address of first PMACFG register

    std::vector<uint64_t> pmacfg_;   // Cached values of PMACFG registers

    bool pmamaskEnabled_ = false;    // PMAMASK registers defined
    uint64_t pmamaskCount_ = 0;      // Count of PMAMASK registers
    uint64_t pmamaskStart_ = 0;      // Address of first PMAMASK register
    std::vector<uint64_t> pmamask_;  // Cached values of PMAMASK registers

    WdRiscv::PmaManager pmaMgr_;
  };

}

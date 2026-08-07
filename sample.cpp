#include "DvMmu.hpp"


using namespace TT_DV_MMU;
using namespace WdRiscv;

int
main(int /*argc*/, char** /*argv*/)
{
  DvMmu mmu(0x2000000000000000LL /*physical mem size*/);

  // Configure for PTE A/D bits maintained by hardware.
  mmu.setFaultOnFirstAccess(false);
  mmu.setFaultOnFirstAccessStage1(false);
  mmu.setFaultOnFirstAccessStage2(false);

  // Test bare mode translation. ASIC and root page number are not applicable.
  mmu.configTranslation(VirtMem::Mode::Bare, 0 /*asid*/, 0 /*ppn*/);

  uint64_t va = 0xdeadbeef, pa = 0, gpa = 0;
  bool r = true, w = false, x = false;  // Exactly one of these must be true.
  bool twoStage = false;
  auto ec = mmu.translate(va, PrivilegeMode::Supervisor, twoStage, r, w, x, gpa, pa);
  assert(ec == ExceptionCause::NONE);
  assert(pa == va);

  // Test SV39 mode.
  uint64_t rootAddr = 0x20000;                  // Root page table address.
  uint64_t rootPpn = mmu.pageNumber(rootAddr);  // Root page number.
  Pte39 pte39{0};                // PTE for a super page at root.
  pte39.bits_.valid_ = true;
  pte39.bits_.read_ = true;
  pte39.bits_.user_ = false;
  pte39.bits_.ppn2_ = 0xaa;
  pte39.bits_.ppn1_ = 0;
  pte39.bits_.ppn0_ = 0;

  // Write PTE to memory.
  if (not mmu.memWrite(rootAddr, sizeof(pte39), pte39.data_))
    assert(0);

  // Config for SV39 translation and translate.
  mmu.configTranslation(VirtMem::Mode::Sv39, 0 /*asid*/, rootPpn);
  va = 0;
  ec = mmu.translate(va, PrivilegeMode::Supervisor, twoStage, r, w, x, gpa, pa);
  assert(ec == ExceptionCause::NONE);
  assert(pa == 0x2a80000000);

  // Translate for read: Should fail.
  r = false; w = true;
  va = 0;
  ec = mmu.translate(va, PrivilegeMode::Supervisor, twoStage, r, w, x, gpa, pa);
  assert(ec == ExceptionCause::STORE_PAGE_FAULT);

  // Translate for read and a different address in the same super page.
  r = true; w = false;
  va = 0x1024;
  ec = mmu.translate(va, PrivilegeMode::Supervisor, twoStage, r, w, x, gpa, pa);
  assert(ec == ExceptionCause::NONE);
  assert(pa == 0x2a80001024);

  const auto& walks = mmu.getPageTableWalks();
  assert(not walks.empty());
  for (const auto& walk : walks)
    {
      assert(walk.complete());
      std::cout << std::hex << "VA=0x" << walk.start() << " PA=0x" << walk.result() << '\n';
      for (size_t i = 0; i < walk.size(); ++i)
        {
          auto addr = walk.ithPteAddr(i);
          auto val = walk.ithPte(i);
          std::cout <<  " PTE: 0x" << addr << "=0x" << val << '\n';
        }
      std::cout << std::dec;
    }

  mmu.definePmpRegs(0x1000, 2, 0x2000, 16);
  mmu.definePmaRegs(0x7e0, 16);
  if (not mmu.mmrWrite(0x7e0, 0xb8000000000000e7))
    assert(0);

  if (not mmu.mmrWrite(0x2000, 0x400))
    assert(0);

  if (not mmu.mmrWrite(0x2008, 0x400))
    assert(0);
  
  if (not mmu.mmrWrite(0x2010, 0x400))
    assert(0);

  return 0;
}

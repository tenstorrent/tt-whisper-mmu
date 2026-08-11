// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <unordered_map>

namespace TT_DV_MMU
{

  ///
  /// Sparse memory model
  ///
  class MemModel
  {
  public:

    explicit MemModel(size_t size)
      : size_(size)
    { }

    /// Read size butes from the given address putting the results in data. Return true
    /// on success and false on failure. Size must be <= 8.
    bool read(uint64_t addr, unsigned size, uint64_t& data)
    {
      assert(size <= 8);
      data = 0;
      if (addr + size > size_)
        return false;

      uint8_t* page = locateOrCreatePage(addr);
      if (pageNumber(addr) == pageNumber(addr + size))
        {
          for (unsigned i = 0; i < size; ++i)
            {
              uint64_t offset = (addr + i) & (pageSize_ - 1);
              uint8_t byte = page[offset];
              data = data | (uint64_t(byte) << (i*8));
            }
          return true;
        }

      uint8_t* page2 = locateOrCreatePage(addr + size);
      unsigned size1 = offsetToNextPage(addr);
      for (unsigned i = 0; i < size; ++i)
        {
          uint8_t byte = 0;
          if (i < size1)
            byte = page[(addr + i) & (pageSize_ - 1)];
          else
            byte = page2[i - size1];
          data = data | (uint64_t(byte) << (i*8));
        }
      return true;
    }
    
    /// Write size butes from data into memory. Return true on success and false on
    /// failure. Size must be <= 8.
    bool write(uint64_t addr, unsigned size, uint64_t data)
    {
      assert(size <= 8);
      if (addr + size > size_)
        return false;

      uint8_t* page = locateOrCreatePage(addr);
      if (pageNumber(addr) == pageNumber(addr + size))
        {
          for (unsigned i = 0; i < size; ++i)
            {
              uint64_t offset = (addr + i) & (pageSize_ - 1);
              page[offset] = uint8_t(data >> i*8);
            }
          return true;
        }

      uint8_t* page2 = locateOrCreatePage(addr + size);
      unsigned size1 = offsetToNextPage(addr);
      for (unsigned i = 0; i < size; ++i)
        {
          if (i < size1)
            page[(addr + i) & (pageSize_ - 1)] = uint8_t(data >> i*8);
          else
            page2[i - size1] = uint8_t(data >> i*8);
        }
      return true;
    }

    /// Return memory size.
    uint64_t size() const
    { return size_; }

    /// Return page size.
    uint64_t pageSize() const
    { return pageSize_; }

    /// Return offset from given page to the next larger or equal page aligned
    /// address.
    unsigned offsetToNextPage(uint64_t addr) const
    { return pageSize_ - (addr & (pageSize_ - 1)); }

    /// Return the page number (address divided by page size) of the page containing the
    /// given page number.
    uint64_t pageNumber(uint64_t addr) const
    { return addr >> pageShift_; }

  protected:

    uint8_t* locateOrCreatePage(uint64_t addr)
    {
      uint64_t num = pageNumber(addr);
      auto iter = pageMap_.find(num);
      if (iter != pageMap_.end())
        return iter->second;
      uint8_t* page = new uint8_t[pageSize_];
      pageMap_[num] = page;
      return page;
    }

  private:

    uint64_t pageSize_ = 4096;
    uint64_t pageShift_ = 12;
    uint64_t size_ = 0;
    std::unordered_map<uint64_t, uint8_t*> pageMap_;
  };

}

/* implementations of byte-at-a-time virtual read/writes for long mode that
   never cause faults/exceptions and maybe do not affect TLB content */

#define NEED_CPU_REG_SHORTCUTS 1
#include "bochs.h"
#include "cpu.h"
#define LOG_THIS BX_CPU_THIS_PTR
#define BX_CR3_PAGING_MASK    (BX_CONST64(0x000ffffffffff000))
#define PAGE_DIRECTORY_NX_BIT (BX_CONST64(0x8000000000000000))
#define BX_PAGING_PHY_ADDRESS_RESERVED_BITS \
              (BX_PHY_ADDRESS_RESERVED_BITS & BX_CONST64(0xfffffffffffff))
#define PAGING_PAE_RESERVED_BITS (BX_PAGING_PHY_ADDRESS_RESERVED_BITS)
#define BX_LEVEL_PML4  3
#define BX_LEVEL_PDPTE 2
#define BX_LEVEL_PDE   1
#define BX_LEVEL_PTE   0

static const char *bx_paging_level[4] = { "PTE", "PDE", "PDPE", "PML4" }; // keep it 4 letters

    Bit8u BX_CPP_AttrRegparmN(2)
BX_CPU_C::read_virtual_byte_64_nofail(unsigned s, Bit64u offset, uint8_t *error)
{
    Bit8u data;
    Bit64u laddr = get_laddr64(s, offset); // this is safe

    if (! IsCanonical(laddr)) {
        *error = 1;
        return 0;
    }

    access_read_linear_nofail(laddr, 1, 0, BX_READ, (void *) &data, error);
    return data;
}

int BX_CPU_C::access_read_linear_nofail(bx_address laddr, unsigned len, unsigned curr_pl, unsigned xlate_rw, void *data, uint8_t *error)
{
    Bit32u combined_access = 0x06;
    Bit32u lpf_mask = 0xfff; // 4K pages
    bx_phy_address paddress, ppf, poffset = PAGE_OFFSET(laddr);

    paddress = translate_linear_long_mode_nofail(laddr, error);
    paddress = A20ADDR(paddress);
    if (*error == 1) {
        return 0;
    }
    access_read_physical(paddress, len, data);

    return 0;
}


bx_phy_address BX_CPU_C::translate_linear_long_mode_nofail(bx_address laddr, uint8_t *error)
{
    bx_phy_address entry_addr[4];
    bx_phy_address ppf = BX_CPU_THIS_PTR cr3 & BX_CR3_PAGING_MASK;
    Bit64u entry[4];
    bx_bool nx_fault = 0;
    int leaf;

    Bit64u offset_mask = BX_CONST64(0x0000ffffffffffff);

    Bit64u reserved = PAGING_PAE_RESERVED_BITS;
    if (! BX_CPU_THIS_PTR efer.get_NXE())
        reserved |= PAGE_DIRECTORY_NX_BIT;

    for (leaf = BX_LEVEL_PML4;; --leaf) {
        entry_addr[leaf] = ppf + ((laddr >> (9 + 9*leaf)) & 0xff8);

        access_read_physical(entry_addr[leaf], 8, &entry[leaf]);
        BX_NOTIFY_PHY_MEMORY_ACCESS(entry_addr[leaf], 8, BX_READ, (BX_PTE_ACCESS + leaf), (Bit8u*)(&entry[leaf]));
        offset_mask >>= 9;

        Bit64u curr_entry = entry[leaf];
        int fault = check_entry_PAE(bx_paging_level[leaf], curr_entry, reserved, 0, &nx_fault);
        if (fault >= 0) {
            *error = 1;
            return 0;
        }

        ppf = curr_entry & BX_CONST64(0x000ffffffffff000);

        if (leaf == BX_LEVEL_PTE) break;

        if (curr_entry & 0x80) {
            if (leaf > (BX_LEVEL_PDE + !!bx_cpuid_support_1g_paging())) {
                BX_DEBUG(("PAE %s: PS bit set !", bx_paging_level[leaf]));
                *error = 1;
                return 0;
            }

            ppf &= BX_CONST64(0x000fffffffffe000);
            if (ppf & offset_mask) {
                BX_DEBUG(("PAE %s: reserved bit is set: 0x" FMT_ADDRX64, bx_paging_level[leaf], curr_entry));
                *error = 1;
                return 0;
            }

            break;
        }
    } /* for (leaf = BX_LEVEL_PML4;; --leaf) */


    *error = 0;
    return ppf | (laddr & offset_mask);
}

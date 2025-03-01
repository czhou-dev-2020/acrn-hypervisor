/*
 * Copyright (C) 2018 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/**
 * @file pgtable.h
 *
 * @brief Address translation and page table operations
 */
#ifndef PGTABLE_H
#define PGTABLE_H

#include <asm/page.h>

#define PAGE_PRESENT		(1UL << 0U)
#define PAGE_RW			(1UL << 1U)
#define PAGE_USER		(1UL << 2U)
#define PAGE_PWT		(1UL << 3U)
#define PAGE_PCD		(1UL << 4U)
#define PAGE_ACCESSED		(1UL << 5U)
#define PAGE_DIRTY		(1UL << 6U)
#define PAGE_PSE		(1UL << 7U)
#define PAGE_GLOBAL		(1UL << 8U)
#define PAGE_PAT_LARGE		(1UL << 12U)
#define PAGE_NX			(1UL << 63U)

#define PAGE_CACHE_MASK		(PAGE_PCD | PAGE_PWT)
#define PAGE_CACHE_WB		0UL
#define PAGE_CACHE_WT		PAGE_PWT
#define PAGE_CACHE_UC_MINUS	PAGE_PCD
#define PAGE_CACHE_UC		(PAGE_PCD | PAGE_PWT)

#define PAGE_ATTR_USER		(PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NX)

/**
 * @defgroup ept_mem_access_right EPT Memory Access Right
 *
 * This is a group that includes EPT Memory Access Right Definitions.
 *
 * @{
 */

/**
 * @brief EPT memory access right is read-only.
 */
#define EPT_RD			(1UL << 0U)

/**
 * @brief EPT memory access right is read/write.
 */
#define EPT_WR			(1UL << 1U)

/**
 * @brief EPT memory access right is executable.
 */
#define EPT_EXE			(1UL << 2U)

/**
 * @brief EPT memory access right is read/write and executable.
 */
#define EPT_RWX			(EPT_RD | EPT_WR | EPT_EXE)

/**
 * @}
 */
/* End of ept_mem_access_right */

/**
 * @defgroup ept_mem_type EPT Memory Type
 *
 * This is a group that includes EPT Memory Type Definitions.
 *
 * @{
 */

/**
 * @brief EPT memory type is specified in bits 5:3 of the EPT paging-structure entry.
 */
#define EPT_MT_SHIFT		3U

/**
 * @brief EPT memory type is uncacheable.
 */
#define EPT_UNCACHED		(0UL << EPT_MT_SHIFT)

/**
 * @brief EPT memory type is write combining.
 */
#define EPT_WC			(1UL << EPT_MT_SHIFT)

/**
 * @brief EPT memory type is write through.
 */
#define EPT_WT			(4UL << EPT_MT_SHIFT)

/**
 * @brief EPT memory type is write protected.
 */
#define EPT_WP			(5UL << EPT_MT_SHIFT)

/**
 * @brief EPT memory type is write back.
 */
#define EPT_WB			(6UL << EPT_MT_SHIFT)

/**
 * @brief Ignore PAT memory type.
 */
#define EPT_IGNORE_PAT		(1UL << 6U)

/**
 * @}
 */
/* End of ept_mem_type */

#define EPT_MT_MASK		(7UL << EPT_MT_SHIFT)
#define EPT_VE			(1UL << 63U)
/* EPT leaf entry bits (bit 52 - bit 63) should be maksed  when calculate PFN */
#define EPT_PFN_HIGH_MASK	0xFFF0000000000000UL

#define PML4E_SHIFT		39U
#define PTRS_PER_PML4E		512UL
#define PML4E_SIZE		(1UL << PML4E_SHIFT)
#define PML4E_MASK		(~(PML4E_SIZE - 1UL))

#define PDPTE_SHIFT		30U
#define PTRS_PER_PDPTE		512UL
#define PDPTE_SIZE		(1UL << PDPTE_SHIFT)
#define PDPTE_MASK		(~(PDPTE_SIZE - 1UL))

#define PDE_SHIFT		21U
#define PTRS_PER_PDE		512UL
#define PDE_SIZE		(1UL << PDE_SHIFT)
#define PDE_MASK		(~(PDE_SIZE - 1UL))

#define PTE_SHIFT		12U
#define PTRS_PER_PTE		512UL
#define PTE_SIZE		(1UL << PTE_SHIFT)
#define PTE_MASK		(~(PTE_SIZE - 1UL))

/* TODO: PAGE_MASK & PHYSICAL_MASK */
#define PML4E_PFN_MASK		0x0000FFFFFFFFF000UL
#define PDPTE_PFN_MASK		0x0000FFFFFFFFF000UL
#define PDE_PFN_MASK		0x0000FFFFFFFFF000UL

#define EPT_ENTRY_PFN_MASK	((~EPT_PFN_HIGH_MASK) & PAGE_MASK)

/**
 * @brief Page tables level in IA32 paging mode
 */
enum _page_table_level {
        /**
         * @brief The PML4 level in the page tables
         */
	IA32E_PML4 = 0,
        /**
         * @brief The Page-Directory-Pointer-Table level in the page tables
         */
	IA32E_PDPT = 1,
        /**
         * @brief The Page-Directory level in the page tables
         */
	IA32E_PD = 2,
        /**
         * @brief The Page-Table level in the page tables
         */
	IA32E_PT = 3,
};

struct pgtable {
	uint64_t default_access_right;
	struct page_pool *pool;
	bool (*large_page_support)(enum _page_table_level level, uint64_t prot);
	uint64_t (*pgentry_present)(uint64_t pte);
	void (*clflush_pagewalk)(const void *p);
	void (*tweak_exe_right)(uint64_t *entry);
	void (*recover_exe_right)(uint64_t *entry);
};

/**
 * @brief Address space translation
 *
 * @addtogroup acrn_mem ACRN Memory Management
 * @{
 */
/* hpa <--> hva, now it is 1:1 mapping */
/**
 * @brief Translate host-physical address to host-virtual address
 *
 * @param[in] x The specified host-physical address
 *
 * @return The translated host-virtual address
 */
static inline void *hpa2hva_early(uint64_t x)
{
	return (void *)x;
}
/**
 * @brief Translate host-virtual address to host-physical address
 *
 * @param[in] x The specified host-virtual address
 *
 * @return The translated host-physical address
 */
static inline uint64_t hva2hpa_early(void *x)
{
	return (uint64_t)x;
}

/**
 * @brief Translate host-physical address to host-virtual address
 *
 * @param[in] x The specified host-physical address
 *
 * @return The translated host-virtual address
 */
static inline void *hpa2hva(uint64_t x)
{
	return (void *)x;
}
/**
 * @brief Translate host-virtual address to host-physical address
 *
 * @param[in] x The specified host-virtual address
 *
 * @return The translated host-physical address
 */
static inline uint64_t hva2hpa(const void *x)
{
	return (uint64_t)x;
}

static inline uint64_t pml4e_index(uint64_t address)
{
	return (address >> PML4E_SHIFT) & (PTRS_PER_PML4E - 1UL);
}

static inline uint64_t pdpte_index(uint64_t address)
{
	return (address >> PDPTE_SHIFT) & (PTRS_PER_PDPTE - 1UL);
}

static inline uint64_t pde_index(uint64_t address)
{
	return (address >> PDE_SHIFT) & (PTRS_PER_PDE - 1UL);
}

static inline uint64_t pte_index(uint64_t address)
{
	return (address >> PTE_SHIFT) & (PTRS_PER_PTE - 1UL);
}

static inline uint64_t *pml4e_page_vaddr(uint64_t pml4e)
{
	return hpa2hva(pml4e & PML4E_PFN_MASK);
}

static inline uint64_t *pdpte_page_vaddr(uint64_t pdpte)
{
	return hpa2hva(pdpte & PDPTE_PFN_MASK);
}

static inline uint64_t *pde_page_vaddr(uint64_t pde)
{
	return hpa2hva(pde & PDE_PFN_MASK);
}

static inline uint64_t *pml4e_offset(uint64_t *pml4_page, uint64_t addr)
{
	return pml4_page + pml4e_index(addr);
}

static inline uint64_t *pdpte_offset(const uint64_t *pml4e, uint64_t addr)
{
	return pml4e_page_vaddr(*pml4e) + pdpte_index(addr);
}

static inline uint64_t *pde_offset(const uint64_t *pdpte, uint64_t addr)
{
	return pdpte_page_vaddr(*pdpte) + pde_index(addr);
}

static inline uint64_t *pte_offset(const uint64_t *pde, uint64_t addr)
{
	return pde_page_vaddr(*pde) + pte_index(addr);
}

/*
 * pgentry may means pml4e/pdpte/pde/pte
 */
static inline uint64_t get_pgentry(const uint64_t *pte)
{
	return *pte;
}

/*
 * pgentry may means pml4e/pdpte/pde/pte
 */
static inline void set_pgentry(uint64_t *ptep, uint64_t pte, const struct pgtable *table)
{
	*ptep = pte;
	table->clflush_pagewalk(ptep);
}

static inline uint64_t pde_large(uint64_t pde)
{
	return pde & PAGE_PSE;
}

static inline uint64_t pdpte_large(uint64_t pdpte)
{
	return pdpte & PAGE_PSE;
}

void init_sanitized_page(uint64_t *sanitized_page, uint64_t hpa);

void *pgtable_create_root(const struct pgtable *table);
void *pgtable_create_trusty_root(const struct pgtable *table,
	void *nworld_pml4_page, uint64_t prot_table_present, uint64_t prot_clr);
/**
 *@pre (pml4_page != NULL) && (pg_size != NULL)
 */
const uint64_t *pgtable_lookup_entry(uint64_t *pml4_page, uint64_t addr,
		uint64_t *pg_size, const struct pgtable *table);

void pgtable_add_map(uint64_t *pml4_page, uint64_t paddr_base,
		uint64_t vaddr_base, uint64_t size,
		uint64_t prot, const struct pgtable *table);
void pgtable_modify_or_del_map(uint64_t *pml4_page, uint64_t vaddr_base,
		uint64_t size, uint64_t prot_set, uint64_t prot_clr,
		const struct pgtable *table, uint32_t type);
/**
 * @}
 */
#endif /* PGTABLE_H */

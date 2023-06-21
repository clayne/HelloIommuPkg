#include <Uefi.h>
#include <Guid/Acpi.h>
#include <IndustryStandard/DmaRemappingReportingTable.h>
#include <IndustryStandard/Vtd.h>       // taken from edk2-platforms
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Protocol/LoadedImage.h>

#define Add2Ptr(Ptr, Value)     ((VOID*)((UINT8*)(Ptr) + (Value)))
#define UEFI_DOMAIN_ID          1
#define V_IOTLB_REG_DR          BIT48
#define V_IOTLB_REG_DW          BIT49

//
// 10.4.6 Root Table Address Register
//
typedef union _VTD_ROOT_TABLE_ADDRESS_REGISTER
{
    struct
    {
        UINT64 Reserved_1 : 10;             // [9:0]
        UINT64 TranslationTableMode : 2;    // [11:10]
        UINT64 RootTable : 52;              // [63:12]
    } Bits;
    UINT64 AsUInt64;
} VTD_ROOT_TABLE_ADDRESS_REGISTER;
STATIC_ASSERT(sizeof(VTD_ROOT_TABLE_ADDRESS_REGISTER) == sizeof(UINT64), "Unexpected size");

//
// Collection of data structures used by hardware to perform DMA-remapping
// translation.
//
typedef struct _DMAR_TRANSLATIONS
{
    //
    // The root table is only for each hardware unit and made up of 256 entries.
    //
    VTD_ROOT_ENTRY RootTable[256];

    //
    // The context table can be multiple but all root entries set up by this
    // project point to the same, single context table, hence this is not
    // ContextTable[256][256]. This table is made up of 256 entries.
    //
    VTD_CONTEXT_ENTRY ContextTable[256];

    //
    // The second-level PML4 can be multiple but all context entries set up by
    // this projects point to the same, single PML4, This table is made up of
    // 512 entries.
    //
    VTD_SECOND_LEVEL_PAGING_ENTRY SlPml4[512];

    //
    // This project only uses PML4[0], hence only one PDPT is used. PDPT is made
    // up of 512 entries.
    //
    VTD_SECOND_LEVEL_PAGING_ENTRY SlPdpt[1][512];

    //
    // Have PD for each PDPT and each PD is made up of 512 entries; hence [512][512].
    //
    VTD_SECOND_LEVEL_PAGING_ENTRY SlPd[1][512][512];
} DMAR_TRANSLATIONS;
STATIC_ASSERT((sizeof(DMAR_TRANSLATIONS) % SIZE_4KB) == 0, "Unexpected size");
STATIC_ASSERT((OFFSET_OF(DMAR_TRANSLATIONS, ContextTable) % SIZE_4KB) == 0, "Unexpected size");
STATIC_ASSERT((OFFSET_OF(DMAR_TRANSLATIONS, SlPml4) % SIZE_4KB) == 0, "Unexpected size");
STATIC_ASSERT((OFFSET_OF(DMAR_TRANSLATIONS, SlPdpt) % SIZE_4KB) == 0, "Unexpected size");
STATIC_ASSERT((OFFSET_OF(DMAR_TRANSLATIONS, SlPd) % SIZE_4KB) == 0, "Unexpected size");

//
// The representation of each DMA-remapping hardware unit.
//
typedef struct _DMAR_UNIT_INFORMATION
{
    UINT64 RegisterBasePa;
    UINT64 RegisterBaseVa;
    VTD_CAP_REG Capability;
    VTD_ECAP_REG ExtendedCapability;
    DMAR_TRANSLATIONS* Translations;
} DMAR_UNIT_INFORMATION;

//
// The helper structure for translating the guest physical address to the
// host physical address.
//
typedef union _ADDRESS_TRANSLATION_HELPER
{
    //
    // Indexes to locate paging-structure entries corresponds to this virtual
    // address.
    //
    struct
    {
        UINT64 Unused : 12;         //< [11:0]
        UINT64 Pt : 9;              //< [20:12]
        UINT64 Pd : 9;              //< [29:21]
        UINT64 Pdpt : 9;            //< [38:30]
        UINT64 Pml4 : 9;            //< [47:39]
    } AsIndex;
    UINT64 AsUInt64;
} ADDRESS_TRANSLATION_HELPER;

/**
 * @brief Collects relevant information of each DMA-remapping hardware units.
 */
static
EFI_STATUS
ProcessDmarTable (
    IN CONST EFI_ACPI_DMAR_HEADER* DmarTable,
    IN OUT DMAR_UNIT_INFORMATION* DmarUnits,
    IN UINT64 MaxDmarUnitCount,
    OUT UINT64* DetectedUnitCount
    )
{
    UINT64 endOfDmar;
    CONST EFI_ACPI_DMAR_STRUCTURE_HEADER* dmarHeader;
    UINT64 discoveredUnitCount;

    ZeroMem(DmarUnits, sizeof(*DmarUnits) * MaxDmarUnitCount);

    //
    // Walk through the DMAR table, find all DMA-remapping hardware unit
    // definition structures in it, and gather relevant information into DmarUnits.
    //
    discoveredUnitCount = 0;
    endOfDmar = (UINT64)Add2Ptr(DmarTable, DmarTable->Header.Length);
    dmarHeader = (CONST EFI_ACPI_DMAR_STRUCTURE_HEADER*)(DmarTable + 1);
    while ((UINT64)dmarHeader < endOfDmar)
    {
        if (dmarHeader->Type == EFI_ACPI_DMAR_TYPE_DRHD)
        {
            if (discoveredUnitCount < MaxDmarUnitCount)
            {
                CONST EFI_ACPI_DMAR_DRHD_HEADER* dmarUnit;

                dmarUnit = (CONST EFI_ACPI_DMAR_DRHD_HEADER*)dmarHeader;
                DmarUnits[discoveredUnitCount].RegisterBasePa = dmarUnit->RegisterBaseAddress;
                DmarUnits[discoveredUnitCount].RegisterBaseVa = dmarUnit->RegisterBaseAddress;
                DmarUnits[discoveredUnitCount].Capability.Uint64 =
                    MmioRead64(DmarUnits[discoveredUnitCount].RegisterBaseVa + R_CAP_REG);
                DmarUnits[discoveredUnitCount].ExtendedCapability.Uint64 =
                    MmioRead64(DmarUnits[discoveredUnitCount].RegisterBaseVa + R_ECAP_REG);
            }
            discoveredUnitCount++;
        }
        dmarHeader = (CONST EFI_ACPI_DMAR_STRUCTURE_HEADER*)Add2Ptr(dmarHeader, dmarHeader->Length);
    }

    //
    // Processed all structures. It is an error if nothing found, or found too many.
    //
    *DetectedUnitCount = discoveredUnitCount;

    for (UINT64 i = 0; i < discoveredUnitCount; ++i)
    {
        DEBUG((DEBUG_VERBOSE, "Unit %d at %p - Cap: %llx, ExCap: %llx\n",
               i,
               DmarUnits[i].RegisterBasePa,
               DmarUnits[i].Capability.Uint64,
               DmarUnits[i].ExtendedCapability.Uint64));
    }
    if (discoveredUnitCount == 0)
    {
        DEBUG((DEBUG_ERROR, "No DMA remapping hardware unit found.\n"));
        return EFI_UNSUPPORTED;
    }
    if (discoveredUnitCount > MaxDmarUnitCount)
    {
        DEBUG((DEBUG_ERROR,
               "Too many DMA remapping hardware units found (%llu).\n",
               discoveredUnitCount));
        return EFI_OUT_OF_RESOURCES;
    }
    return EFI_SUCCESS;
}

/**
 * @brief Splits the PDE to a new PT.
 */
static
VTD_SECOND_LEVEL_PAGING_ENTRY*
Split2MbPage (
    IN OUT VTD_SECOND_LEVEL_PAGING_ENTRY* PageDirectoryEntry
    )
{
    UINT64 baseAddress;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pageTable;
    BOOLEAN readable;
    BOOLEAN writable;

    ASSERT(PageDirectoryEntry->Bits.PageSize == TRUE);

    pageTable = AllocateRuntimePages(1);
    if (pageTable == NULL)
    {
        goto Exit;
    }
    ZeroMem(pageTable, SIZE_4KB);

    //
    // Those fields should inherit from the PDE.
    //
    readable = (PageDirectoryEntry->Bits.Read != FALSE);
    writable = (PageDirectoryEntry->Bits.Write != FALSE);

    //
    // Fill out the page table.
    //
    baseAddress = ((UINT64)PageDirectoryEntry->Bits.AddressLo << 12) |
                  ((UINT64)PageDirectoryEntry->Bits.AddressHi << 32);
    for (UINT64 ptIndex = 0; ptIndex < 512; ++ptIndex)
    {
        pageTable[ptIndex].Uint64 = baseAddress;
        pageTable[ptIndex].Bits.Read = readable;
        pageTable[ptIndex].Bits.Write = writable;
        baseAddress += SIZE_4KB;
    }

    //
    // The PDE should no longer indicates 2MB large page.
    //
    PageDirectoryEntry->Uint64 = (UINT64)pageTable;
    PageDirectoryEntry->Bits.PageSize = FALSE;
    PageDirectoryEntry->Bits.Read = TRUE;
    PageDirectoryEntry->Bits.Write = TRUE;

    //
    // Write back changes to RAM. Also, invalidation of IOTLB would be required
    // if the DMA-remapping is already enabled. Not the case in this project.
    //
    WriteBackDataCacheRange(PageDirectoryEntry, sizeof(*PageDirectoryEntry));
    WriteBackDataCacheRange(pageTable, SIZE_4KB);

Exit:
    return pageTable;
}

/**
 * @brief Updates the access permissions in the translations for the given address.
 *
 * @note As the name suggests, this change is applied for all devices, ie, you
 *       may not specify a source-id (ie, bus:device:function). This is purely
 *       for overall simplicity of this project.
 */
static
EFI_STATUS
ChangePermissionOfPageForAllDevices (
    IN OUT DMAR_TRANSLATIONS* Translations,
    IN UINT64 Address,
    IN BOOLEAN AllowReadWrite,
    OUT VTD_SECOND_LEVEL_PAGING_ENTRY** AllocatedPageTable
    )
{
    EFI_STATUS status;
    ADDRESS_TRANSLATION_HELPER helper;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pde;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pt;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pte;

    *AllocatedPageTable = NULL;

    helper.AsUInt64 = Address;

    //
    // Locate the second-level PDE for the given address. If that entry indicates
    // the page is 2MB large page, split it into 512 PTEs so that the exactly
    // specified page (4KB) only is updated.
    //
    pde = &Translations->SlPd[helper.AsIndex.Pml4][helper.AsIndex.Pdpt][helper.AsIndex.Pd];
    if (pde->Bits.PageSize != FALSE)
    {
        *AllocatedPageTable = Split2MbPage(pde);
        if (*AllocatedPageTable == NULL)
        {
            status = EFI_OUT_OF_RESOURCES;
            goto Exit;
        }
    }

    //
    // Then, update the single PTE that corresponds to the given address.
    //
    pt = (VTD_SECOND_LEVEL_PAGING_ENTRY*)(((UINT64)pde->Bits.AddressLo << 12) |
                                          ((UINT64)pde->Bits.AddressHi << 32));
    pte = &pt[helper.AsIndex.Pt];
    pte->Bits.Read = AllowReadWrite;
    pte->Bits.Write = AllowReadWrite;
    WriteBackDataCacheRange(pte, sizeof(*pte));

    //
    // We are good. Note that any of page table updates would require invalidation
    // of IOTLB if DMA-remapping is already enabled. In our case, not yet.
    //
    status = EFI_SUCCESS;

Exit:
    return status;
}

/**
 * @brief Returns the base address of the current image, or zero on error.
 */
static
UINT64
GetCurrentImageBase (
    VOID
    )
{
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL* loadedImageInfo;

    status = gBS->OpenProtocol(gImageHandle,
                               &gEfiLoadedImageProtocolGuid,
                               (VOID**)&loadedImageInfo,
                               gImageHandle,
                               NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status))
    {
        DEBUG((DEBUG_ERROR, "OpenProtocol failed : %r\n", status));
        return 0;
    }

    return (UINT64)loadedImageInfo->ImageBase;
}

/**
 * @brief Builds identity mapping for all PCI devices, up to 512GB.
 */
static
VOID
BuildPassthroughTranslations (
    OUT DMAR_TRANSLATIONS* Translations
    )
{
    VTD_ROOT_ENTRY defaultRootValue;
    VTD_CONTEXT_ENTRY defaultContextValue;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pdpt;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pd;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pml4e;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pdpte;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pde;
    UINT64 pml4Index;
    UINT64 destinationPa;

    ASSERT(((UINT64)Translations % SIZE_4KB) == 0);

    ZeroMem(Translations, sizeof(*Translations));

    //
    // Fill out the root table. All root entries point to the same context table.
    //
    defaultRootValue.Uint128.Uint64Hi = defaultRootValue.Uint128.Uint64Lo = 0;
    defaultRootValue.Bits.ContextTablePointerLo = (UINT32)((UINT64)Translations->ContextTable >> 12);
    defaultRootValue.Bits.ContextTablePointerHi = (UINT32)((UINT64)Translations->ContextTable >> 32);
    defaultRootValue.Bits.Present = TRUE;
    for (UINT64 bus = 0; bus < ARRAY_SIZE(Translations->RootTable); bus++)
    {
        Translations->RootTable[bus] = defaultRootValue;
    }

    //
    // Fill out the context table. All context entries point to the same
    // second-level PML4.
    //
    // Note that pass-through translations can also be archived by setting 10b to
    // the TT: Translation Type field, instead of using the second-level page
    // tables.
    //
    defaultContextValue.Uint128.Uint64Hi = defaultContextValue.Uint128.Uint64Lo = 0;
    defaultContextValue.Bits.DomainIdentifier = UEFI_DOMAIN_ID;
    defaultContextValue.Bits.AddressWidth = BIT1;  // 010b: 48-bit AGAW (4-level page table)
    defaultContextValue.Bits.SecondLevelPageTranslationPointerLo = (UINT32)((UINT64)Translations->SlPml4 >> 12);
    defaultContextValue.Bits.SecondLevelPageTranslationPointerHi = (UINT32)((UINT64)Translations->SlPml4 >> 32);
    defaultContextValue.Bits.Present = TRUE;
    for (UINT64 i = 0; i < ARRAY_SIZE(Translations->ContextTable); i++)
    {
        Translations->ContextTable[i] = defaultContextValue;
    }

    //
    // Fill out the second level page tables. All entries indicates readable and
    // writable, and translations are identity mapping. No second-level page table
    // is used to save space. All PDEs are configured for 2MB large pages.
    //
    destinationPa = 0;

    //
    // SL-PML4. Only the first entry (ie, translation up to 512GB) is initialized.
    //
    pml4Index = 0;
    pdpt = Translations->SlPdpt[pml4Index];
    pml4e = &Translations->SlPml4[pml4Index];
    pml4e->Uint64 = (UINT64)pdpt;
    pml4e->Bits.Read = TRUE;
    pml4e->Bits.Write = TRUE;

    for (UINT64 pdptIndex = 0; pdptIndex < 512; pdptIndex++)
    {
        //
        // SL-PDPT
        //
        pd = Translations->SlPd[pml4Index][pdptIndex];
        pdpte = &pdpt[pdptIndex];
        pdpte->Uint64 = (UINT64)pd;
        pdpte->Bits.Read = TRUE;
        pdpte->Bits.Write = TRUE;

        for (UINT64 pdIndex = 0; pdIndex < 512; pdIndex++)
        {
            //
            // SL-PD.
            //
            pde = &pd[pdIndex];
            pde->Uint64 = destinationPa;
            pde->Bits.Read = TRUE;
            pde->Bits.Write = TRUE;
            pde->Bits.PageSize = TRUE;
            destinationPa += SIZE_2MB;
        }
    }

    //
    // Write-back the whole range of the translations object to RAM. This flushing
    // cache line is not required if the C: Page-walk Coherency bit is set. Same
    // as other flush in this project. All author's units did not set this bit.
    //
    WriteBackDataCacheRange(Translations, sizeof(*Translations));
}

/**
 * @brief Enables DMA-remapping for the hardware unit using the given translation.
 */
static
VOID
EnableDmaRemapping (
    IN CONST DMAR_UNIT_INFORMATION* DmarUnit,
    IN CONST DMAR_TRANSLATIONS* Translations
    )
{
    VTD_ROOT_TABLE_ADDRESS_REGISTER rootTableAddressReg;
    UINT64 iotlbRegOffset;

    DEBUG((DEBUG_INFO, "Working with the remapping unit at %p\n", DmarUnit->RegisterBasePa));

    //
    // Set the Root Table Pointer. This is equivalent to setting CR3 conceptually.
    // After setting the "SRTP: Set Root Table Pointer" bit, software must wait
    // completion of it. See 10.4.5 Global Status Register.
    //
    DEBUG((DEBUG_INFO, "Setting the root table pointer to %p\n", Translations->RootTable));
    rootTableAddressReg.AsUInt64 = 0;
    rootTableAddressReg.Bits.RootTable = (UINT64)Translations->RootTable >> 12;
    MmioWrite64(DmarUnit->RegisterBaseVa + R_RTADDR_REG, rootTableAddressReg.AsUInt64);
    MmioWrite32(DmarUnit->RegisterBaseVa + R_GCMD_REG, B_GMCD_REG_SRTP);
    for (; (MmioRead32(DmarUnit->RegisterBaseVa + R_GSTS_REG) & B_GSTS_REG_RTPS) == 0;)
    {
        CpuPause();
    }

    //
    // Then, invalidate cache that may exists as requested by the specification.
    //
    // "After a ‘Set Root Table Pointer’ operation, software must perform global
    //  invalidations on the context-cache, pasid-cache, and IOTLB, in that order."
    // See 10.4.4 Global Command Register
    //

    //
    // Invalidate context-cache. See 10.4.7 Context Command Register.
    //
    DEBUG((DEBUG_INFO, "Invalidating context-cache globally\n"));
    MmioWrite64(DmarUnit->RegisterBaseVa + R_CCMD_REG, V_CCMD_REG_CIRG_GLOBAL | B_CCMD_REG_ICC);
    for (; (MmioRead64(DmarUnit->RegisterBaseVa + R_CCMD_REG) & B_CCMD_REG_ICC) != 0;)
    {
        CpuPause();
    }

    //
    // Invalidate IOTLB. See 10.4.8.1 IOTLB Invalidate Register.
    // Also drain all read and write requests.
    // "Hardware implementations supporting DMA draining must drain any inflight
    //  DMA read/write requests"
    //
    DEBUG((DEBUG_INFO, "Invalidating IOTLB globally\n"));
    iotlbRegOffset = (UINT64)DmarUnit->ExtendedCapability.Bits.IRO * 16;
    MmioWrite64(DmarUnit->RegisterBaseVa + iotlbRegOffset + R_IOTLB_REG,
                B_IOTLB_REG_IVT | V_IOTLB_REG_IIRG_GLOBAL | V_IOTLB_REG_DR | V_IOTLB_REG_DW);
    for (; (MmioRead64(DmarUnit->RegisterBaseVa + iotlbRegOffset + R_IOTLB_REG) & B_IOTLB_REG_IVT) != 0;)
    {
        CpuPause();
    }

    //
    // Enabling DMA-remapping. See 10.4.4 Global Command Register.
    //
    DEBUG((DEBUG_INFO, "Enabling DMA-remapping\n"));
    MmioWrite32(DmarUnit->RegisterBaseVa + R_GCMD_REG, B_GMCD_REG_TE);
    for (; (MmioRead32(DmarUnit->RegisterBaseVa + R_GSTS_REG) & B_GSTS_REG_TE) == 0;)
    {
        CpuPause();
    }
}

/**
 * @brief Tests whether all hardware units are compatible with this project.
 */
static
BOOLEAN
AreAllDmaRemappingUnitsCompatible (
    IN CONST DMAR_UNIT_INFORMATION* DmarUnits,
    IN UINT64 DmarUnitsCount
    )
{
    for (UINT64 i = 0; i < DmarUnitsCount; ++i)
    {
        //
        // This project does not handle 3-level page-table for simplicity.
        //
        if ((DmarUnits[i].Capability.Bits.SAGAW & BIT2) == 0)
        {
            DEBUG((DEBUG_ERROR,
                   "Unit %lld does not support 48-bit AGAW (4-level page-table) : %016llx\n",
                   i,
                   DmarUnits[i].Capability.Uint64));
            return FALSE;
        }

        //
        // This project requires 2MB large pages for simple second-level table
        // implementation.
        //
        if ((DmarUnits[i].Capability.Bits.SLLPS & BIT0) == 0)
        {
            DEBUG((DEBUG_ERROR,
                   "Unit %lld does not support 2MB second level large pages : %016llx\n",
                   i,
                   DmarUnits[i].Capability.Uint64));
            return FALSE;
        }

        //
        // Earlier implementation of DMA-remapping required explicit write buffer
        // flushing. The author have not encounter with such implementation. As
        // such, this project does not support it. See 6.8 Write Buffer Flushing.
        //
        if (DmarUnits[i].Capability.Bits.RWBF != FALSE)
        {
            DEBUG((DEBUG_ERROR,
                   "Unit %lld requires explicit write buffer flushing : %016llx\n",
                   i,
                   DmarUnits[i].Capability.Uint64));
            return FALSE;
        }

        //
        // If DMA-remapping is already enabled, do not attempt to mess with this.
        // This is the case when preboot VT-d is enabled, for example.
        //
        if ((MmioRead32(DmarUnits[i].RegisterBaseVa + R_GSTS_REG) & B_GSTS_REG_TE) != 0)
        {
            DEBUG((DEBUG_ERROR,
                   "Unit %lld already enabled DMA remapping : %016llx\n",
                   i,
                   MmioRead32(DmarUnits[i].RegisterBaseVa + R_GSTS_REG)));
            return FALSE;
        }

        //
        // Looks good. Dump physical address of where translation fault logs are saved.
        //
        Print(L"Fault-recording Register at %p\n",
              DmarUnits[i].RegisterBaseVa + (UINT64)DmarUnits[i].Capability.Bits.FRO * 16);
    }
    return TRUE;
}

/**
 * @brief The module entry point.
 */
EFI_STATUS
EFIAPI
HelloIommuDxeInitialize (
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
    )
{
    EFI_STATUS status;
    EFI_ACPI_DMAR_HEADER* dmarTable;
    DMAR_UNIT_INFORMATION dmarUnits[8];
    UINT64 dmarUnitCount;
    UINT64 addressToProtect;
    DMAR_TRANSLATIONS* translations;
    VTD_SECOND_LEVEL_PAGING_ENTRY* pageTable;

    translations = NULL;
    pageTable = NULL;

    DEBUG((DEBUG_VERBOSE, "Loading the driver...\n"));

    //
    // Locate the DMAR ACPI table.
    //
    dmarTable = (EFI_ACPI_DMAR_HEADER*)EfiLocateFirstAcpiTable(
                                    EFI_ACPI_4_0_DMA_REMAPPING_TABLE_SIGNATURE);
    if (dmarTable == NULL)
    {
        DEBUG((DEBUG_ERROR,
               "EfiLocateFirstAcpiTable failed. DMA remapping (VT-d) not supported.\n"));
        status = EFI_UNSUPPORTED;
        goto Exit;
    }

    //
    // Gather DMA remapping hardware units information from the DMAR table. This
    // enumerates all DMA-remapping hardware unit definiton structures and collects
    // relevant information such as the base register address and capability
    // register values.
    //
    status = ProcessDmarTable(dmarTable, dmarUnits, ARRAY_SIZE(dmarUnits), &dmarUnitCount);
    if (EFI_ERROR(status))
    {
        DEBUG((DEBUG_ERROR, "ProcessDmarTable failed : %r\n", status));
        goto Exit;
    }

    //
    // This project requires availability of certain features and expect certain
    // system state for simplicity. Verify that those are satisfied.
    //
    if (AreAllDmaRemappingUnitsCompatible(dmarUnits, dmarUnitCount) == FALSE)
    {
        DEBUG((DEBUG_ERROR, "One of more DMA remapping hardware unit is incompatible.\n"));
        status = EFI_UNSUPPORTED;
        goto Exit;
    }

    //
    // Allocate data structures configuring address translation, that is, the root
    // table, context table, second-level PML4, PDPT and PD. Then, initialize them
    // to set up identity mapping (passthrough translation).
    //
    translations = AllocateRuntimePages(EFI_SIZE_TO_PAGES(sizeof(*translations)));
    if (translations == NULL)
    {
        DEBUG((DEBUG_ERROR,
               "Failed to allocate %llu runtime pages.\n",
               EFI_SIZE_TO_PAGES(sizeof(*translations))));
        status = EFI_OUT_OF_RESOURCES;
        goto Exit;
    }
    BuildPassthroughTranslations(translations);

    //
    // For demonstration, make the first page of this module to be non-readable,
    // non-writable via DMA.
    //
    addressToProtect = GetCurrentImageBase();
    if (addressToProtect == 0)
    {
        DEBUG((DEBUG_ERROR, "Unable to resolve the location to protect.\n"));
        status = EFI_LOAD_ERROR;
        goto Exit;
    }
    status = ChangePermissionOfPageForAllDevices(translations,
                                                 addressToProtect,
                                                 FALSE,
                                                 &pageTable);
    if (EFI_ERROR(status))
    {
        DEBUG((DEBUG_ERROR, "ChangePermissionOfPageForAllDevices failed : %r\n", status));
        goto Exit;
    }

    //
    // Finally, enable DMA-remapping for all hardware units.
    //
    for (UINT64 i = 0; i < dmarUnitCount; ++i)
    {
        EnableDmaRemapping(&dmarUnits[i], translations);
    }

    //
    // Break the signature of the DMAR table so that the operating system does
    // not try to (re)configure DMA-remapping. This obviously is not a production
    // quality approach, as the operating system may not secure the system using
    // DMA-remapping as it would do. There is no agreed interface between the
    // platform and IOMMU-aware OS loaders to hand over already enabled IOMMUs.
    // See "A Tour Beyond BIOS: Using IOMMU for DMA Protection in UEFI Firmware"
    // for other possible options.
    //
    dmarTable->Header.Signature = SIGNATURE_32('?', '?', '?', '?');

    //
    // Anyway, we are good now.
    //
    status = EFI_SUCCESS;
    Print(L"Physical address %p-%p protected from DMA read and write\n",
          addressToProtect,
          addressToProtect + SIZE_4KB);

Exit:
    if (EFI_ERROR(status))
    {
        if (pageTable != NULL)
        {
            FreePages(pageTable, 1);
        }

        if (translations)
        {
            FreePages(translations, EFI_SIZE_TO_PAGES(sizeof(*translations)));
        }
    }
    return status;
}

/*-
 * Copyright (c) 2012 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * dm ACPI table generator.
 *
 * Create the minimal set of ACPI tables required to boot FreeBSD (and
 * hopefully other o/s's) by writing out ASL template files for each of
 * the tables and the compiling them to AML with the Intel iasl compiler.
 * The AML files are then read into guest memory.
 *
 *  The tables are placed in the guest's ROM area just below 1MB physical,
 * above the MPTable.
 *
 *  Layout
 *  ------
 *   RSDP  ->   0xf2400    (36 bytes fixed)
 *     RSDT  ->   0xf2440    (36 bytes + 4*7 table addrs, 4 used)
 *     XSDT  ->   0xf2480    (36 bytes + 8*7 table addrs, 4 used)
 *       MADT  ->   0xf2500  (depends on #CPUs)
 *       FADT  ->   0xf2600  (268 bytes)
 *       HPET  ->   0xf2740  (56 bytes)
 *       MCFG  ->   0xf2780  (60 bytes)
 *         FACS  ->   0xf27C0 (64 bytes)
 *         DSDT  ->   0xf2800 (variable - can go up to 0x100000)
 */

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <paths.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>

#include "dm.h"
#include "acpi.h"
#include "pci_core.h"
#include "tpm.h"
#include "vmmapi.h"
#include "hpet.h"
#include "log.h"
#include "vssram.h"
#include "vmmapi.h"
#include "mmio_dev.h"

/*
 * Define the base address of the ACPI tables, and the offsets to
 * the individual tables
 */
#define ACPI_BASE		0xf2400
#define	ACPI_LENGTH		(0x100000 - ACPI_BASE)
#define RSDT_OFFSET		0x040
#define XSDT_OFFSET		0x080
#define MADT_OFFSET		0x100
#define FADT_OFFSET		0x200
#define	HPET_OFFSET		0x340
#define	MCFG_OFFSET		0x380
#define FACS_OFFSET		0x3C0
#define NHLT_OFFSET		0x400
#define TPM2_OFFSET		0xC00
#define RTCT_OFFSET		0xF00
#define DSDT_OFFSET		0x1100

#define	ASL_TEMPLATE	"dm.XXXXXXX"
#define ASL_SUFFIX	".aml"
#ifndef ASL_COMPILER
#define ASL_COMPILER	"/usr/sbin/iasl"
#endif

uint64_t audio_nhlt_len = 0;

static int basl_keep_temps;
static int basl_verbose_iasl;
static int basl_ncpu;
static uint32_t basl_acpi_base = ACPI_BASE;

/*
 * Contains the full pathname of the template to be passed
 * to mkstemp/mktemps(3)
 */
static char basl_template[MAXPATHLEN];
static char basl_stemplate[MAXPATHLEN];

/*
 * State for dsdt_line(), dsdt_indent(), and dsdt_unindent().
 */
static FILE *dsdt_fp;
static int dsdt_indent_level;
static int dsdt_error;

struct basl_fio {
	int	fd;
	FILE	*fp;
	char	f_name[MAXPATHLEN];
};

static bool acpi_table_is_valid(int num);

static int
basl_fwrite_rsdp(FILE *fp, struct vmctx *ctx)
{
	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm RSDP template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0008]\t\tSignature : \"RSD PTR \"\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 43\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 02\n");
	EFPRINTF(fp, "[0004]\t\tRSDT Address : %08X\n",
	    basl_acpi_base + RSDT_OFFSET);
	EFPRINTF(fp, "[0004]\t\tLength : 00000024\n");
	EFPRINTF(fp, "[0008]\t\tXSDT Address : 00000000%08X\n",
	    basl_acpi_base + XSDT_OFFSET);
	EFPRINTF(fp, "[0001]\t\tExtended Checksum : 00\n");
	EFPRINTF(fp, "[0003]\t\tReserved : 000000\n");

	EFFLUSH(fp);

	return 0;
}

static int
basl_fwrite_rsdt(FILE *fp, struct vmctx *ctx)
{
	uint32_t num = 0U;

	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm RSDT template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"RSDT\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 01\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"DMRSDT  \"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000001\n");
	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");
	EFPRINTF(fp, "\n");

	/* Add in pointers to the MADT, FADT, HPET and TPM2 */
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
	    basl_acpi_base + MADT_OFFSET);
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
	    basl_acpi_base + FADT_OFFSET);
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
	    basl_acpi_base + HPET_OFFSET);
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
	    basl_acpi_base + MCFG_OFFSET);

	if (acpi_table_is_valid(NHLT_ENTRY_NO))
		EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
		    basl_acpi_base + NHLT_OFFSET);

	if (ctx->tpm_dev || pt_tpm2)
		EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
		    basl_acpi_base + TPM2_OFFSET);

	if (ssram) {
		EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : %08X\n", num++,
			    basl_acpi_base + RTCT_OFFSET);
	}

	EFFLUSH(fp);

	return 0;
}

static int
basl_fwrite_xsdt(FILE *fp, struct vmctx *ctx)
{
	uint32_t num = 0U;
	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm XSDT template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"XSDT\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 01\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"DMXSDT  \"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000001\n");
	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");
	EFPRINTF(fp, "\n");

	/* Add in pointers to the MADT, FADT, HPET and TPM2 */
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
	    basl_acpi_base + MADT_OFFSET);
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
	    basl_acpi_base + FADT_OFFSET);
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
	    basl_acpi_base + HPET_OFFSET);
	EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
	    basl_acpi_base + MCFG_OFFSET);

	if (acpi_table_is_valid(NHLT_ENTRY_NO))
		EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
		    basl_acpi_base + NHLT_OFFSET);

	if (ctx->tpm_dev || pt_tpm2)
		EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
		    basl_acpi_base + TPM2_OFFSET);

	if (ssram) {
		EFPRINTF(fp, "[0004]\t\tACPI Table Address %u : 00000000%08X\n", num++,
			    basl_acpi_base + RTCT_OFFSET);
	}

	EFFLUSH(fp);

	return 0;
}

/*
 * Find the nth (least significant) bit set of val
 * and return the index of that bit.
 */
static int find_nth_set_bit_index(uint64_t val, int n)
{
	int idx, set;

	set = 0;
	while (val != 0UL) {
		idx = ffs64(val);
		bitmap_clear_nolock(idx, &val);
		if (set == n) {
			return idx;
		}
		set++;
	}

	return -1;
}

int pcpuid_from_vcpuid(uint64_t guest_pcpu_bitmask, int vcpu_id)
{
	return find_nth_set_bit_index(guest_pcpu_bitmask, vcpu_id);
}

static int
basl_fwrite_madt(FILE *fp, struct vmctx *ctx)
{
	int i;
	uint64_t guest_pcpu_bitmask;

	guest_pcpu_bitmask = vm_get_cpu_affinity_dm();
	if (guest_pcpu_bitmask == 0) {
		pr_err("%s,Err: Invalid guest_pcpu_bitmask.\n", __func__);
		return -1;
	}

	pr_info("%s, guest_cpu_bitmask: 0x%x\n", __func__, guest_pcpu_bitmask);

	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm MADT template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"APIC\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 01\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"DMMADT  \"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000001\n");

	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0004]\t\tLocal Apic Address : FEE00000\n");
	EFPRINTF(fp, "[0004]\t\tFlags (decoded below) : 00000001\n");
	EFPRINTF(fp, "\t\t\tPC-AT Compatibility : 1\n");
	EFPRINTF(fp, "\n");

	/* Add a Processor Local APIC entry for each CPU */
	for (i = 0; i < basl_ncpu; i++) {
		int pcpu_id = pcpuid_from_vcpuid(guest_pcpu_bitmask, i);
		int lapic_id;

		if (pcpu_id < 0) {
			pr_err("%s,Err: pcpu id is not found in guest_pcpu_bitmask.\n", __func__);
			return -1;
		}

		assert(pcpu_id < ACRN_PLATFORM_LAPIC_IDS_MAX);
		if (pcpu_id >= ACRN_PLATFORM_LAPIC_IDS_MAX) {
			pr_err("%s,Err: pcpu id %u should be less than ACRN_PLATFORM_LAPIC_IDS_MAX.\n", __func__, pcpu_id);
			return -1;
		}

		lapic_id = lapicid_from_pcpuid(pcpu_id);
		if (lapic_id == -1) {
			pr_err("Failed to retrieve the local APIC ID for pCPU %u\n", pcpu_id);
			return -1;
		}

		EFPRINTF(fp, "[0001]\t\tSubtable Type : 00\n");
		EFPRINTF(fp, "[0001]\t\tLength : 08\n");
		/* iasl expects hex values for the proc and apic id's */
		EFPRINTF(fp, "[0001]\t\tProcessor ID : %02x\n", i);

		pr_info("%s, vcpu_id=0x%x, pcpu_id=0x%x, lapic_id=0x%x\n", __func__, i, pcpu_id, lapic_id);

		EFPRINTF(fp, "[0001]\t\tLocal Apic ID : %02x\n", lapic_id);

		EFPRINTF(fp, "[0004]\t\tFlags (decoded below) : 00000001\n");
		EFPRINTF(fp, "\t\t\tProcessor Enabled : 1\n");
		EFPRINTF(fp, "\t\t\tRuntime Online Capable : 0\n");
		EFPRINTF(fp, "\n");
	}

	/* Always a single IOAPIC entry, with ID 0 */
	EFPRINTF(fp, "[0001]\t\tSubtable Type : 01\n");
	EFPRINTF(fp, "[0001]\t\tLength : 0C\n");
	/* iasl expects a hex value for the i/o apic id */
	EFPRINTF(fp, "[0001]\t\tI/O Apic ID : %02x\n", 0);
	EFPRINTF(fp, "[0001]\t\tReserved : 00\n");
	EFPRINTF(fp, "[0004]\t\tAddress : fec00000\n");
	EFPRINTF(fp, "[0004]\t\tInterrupt : 00000000\n");
	EFPRINTF(fp, "\n");

	if (!is_rtvm) {
		/* Legacy IRQ0 is connected to pin 2 of the IOAPIC */
		EFPRINTF(fp, "[0001]\t\tSubtable Type : 02\n");
		EFPRINTF(fp, "[0001]\t\tLength : 0A\n");
		EFPRINTF(fp, "[0001]\t\tBus : 00\n");
		EFPRINTF(fp, "[0001]\t\tSource : 00\n");
		EFPRINTF(fp, "[0004]\t\tInterrupt : 00000002\n");
		EFPRINTF(fp, "[0002]\t\tFlags (decoded below) : 0005\n");
		EFPRINTF(fp, "\t\t\tPolarity : 1\n");
		EFPRINTF(fp, "\t\t\tTrigger Mode : 1\n");
		EFPRINTF(fp, "\n");

		EFPRINTF(fp, "[0001]\t\tSubtable Type : 02\n");
		EFPRINTF(fp, "[0001]\t\tLength : 0A\n");
		EFPRINTF(fp, "[0001]\t\tBus : 00\n");
		EFPRINTF(fp, "[0001]\t\tSource : %02X\n", SCI_INT);
		EFPRINTF(fp, "[0004]\t\tInterrupt : %08X\n", SCI_INT);
		EFPRINTF(fp, "[0002]\t\tFlags (decoded below) : 000D\n");
		EFPRINTF(fp, "\t\t\tPolarity : 1\n");
		EFPRINTF(fp, "\t\t\tTrigger Mode : 3\n");
		EFPRINTF(fp, "\n");
	}

	/* Local APIC NMI is connected to LINT 1 on all CPUs */
	EFPRINTF(fp, "[0001]\t\tSubtable Type : 04\n");
	EFPRINTF(fp, "[0001]\t\tLength : 06\n");
	EFPRINTF(fp, "[0001]\t\tProcessor ID : FF\n");
	EFPRINTF(fp, "[0002]\t\tFlags (decoded below) : 0005\n");
	EFPRINTF(fp, "\t\t\tPolarity : 1\n");
	EFPRINTF(fp, "\t\t\tTrigger Mode : 1\n");
	EFPRINTF(fp, "[0001]\t\tInterrupt Input LINT : 01\n");
	EFPRINTF(fp, "\n");

	EFFLUSH(fp);

	return 0;
}

static int
basl_fwrite_fadt(FILE *fp, struct vmctx *ctx)
{
	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm FADT template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"FACP\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 0000010C\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 05\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"DMFACP  \"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000001\n");
	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0004]\t\tFACS Address : %08X\n",
	    basl_acpi_base + FACS_OFFSET);
	EFPRINTF(fp, "[0004]\t\tDSDT Address : %08X\n",
	    basl_acpi_base + DSDT_OFFSET);
	EFPRINTF(fp, "[0001]\t\tModel : 01\n");
	EFPRINTF(fp, "[0001]\t\tPM Profile : 00 [Unspecified]\n");
	EFPRINTF(fp, "[0002]\t\tSCI Interrupt : %04X\n",
	    SCI_INT);
	EFPRINTF(fp, "[0004]\t\tSMI Command Port : %08X\n",
	    SMI_CMD);
	EFPRINTF(fp, "[0001]\t\tACPI Enable Value : %02X\n",
	    ACPI_ENABLE);
	EFPRINTF(fp, "[0001]\t\tACPI Disable Value : %02X\n",
	    ACPI_DISABLE);
	EFPRINTF(fp, "[0001]\t\tS4BIOS Command : 00\n");
	EFPRINTF(fp, "[0001]\t\tP-State Control : 00\n");
	EFPRINTF(fp, "[0004]\t\tPM1A Event Block Address : %08X\n",
	    PM1A_EVT_ADDR);
	EFPRINTF(fp, "[0004]\t\tPM1B Event Block Address : 00000000\n");
	EFPRINTF(fp, "[0004]\t\tPM1A Control Block Address : %08X\n",
	    VIRTUAL_PM1A_CNT_ADDR);
	EFPRINTF(fp, "[0004]\t\tPM1B Control Block Address : 00000000\n");
	EFPRINTF(fp, "[0004]\t\tPM2 Control Block Address : 00000000\n");
	EFPRINTF(fp, "[0004]\t\tPM Timer Block Address : %08X\n",
	    IO_PMTMR);
	EFPRINTF(fp, "[0004]\t\tGPE0 Block Address : 00000000\n");
	EFPRINTF(fp, "[0004]\t\tGPE1 Block Address : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tPM1 Event Block Length : 04\n");
	EFPRINTF(fp, "[0001]\t\tPM1 Control Block Length : 02\n");
	EFPRINTF(fp, "[0001]\t\tPM2 Control Block Length : 00\n");
	EFPRINTF(fp, "[0001]\t\tPM Timer Block Length : 00\n");
	EFPRINTF(fp, "[0001]\t\tGPE0 Block Length : 00\n");
	EFPRINTF(fp, "[0001]\t\tGPE1 Block Length : 00\n");
	EFPRINTF(fp, "[0001]\t\tGPE1 Base Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\t_CST Support : 00\n");
	EFPRINTF(fp, "[0002]\t\tC2 Latency : 0000\n");
	EFPRINTF(fp, "[0002]\t\tC3 Latency : 0000\n");
	EFPRINTF(fp, "[0002]\t\tCPU Cache Size : 0000\n");
	EFPRINTF(fp, "[0002]\t\tCache Flush Stride : 0000\n");
	EFPRINTF(fp, "[0001]\t\tDuty Cycle Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tDuty Cycle Width : 00\n");
	EFPRINTF(fp, "[0001]\t\tRTC Day Alarm Index : 00\n");
	EFPRINTF(fp, "[0001]\t\tRTC Month Alarm Index : 00\n");
	EFPRINTF(fp, "[0001]\t\tRTC Century Index : 32\n");
	EFPRINTF(fp, "[0002]\t\tBoot Flags (decoded below) : 0000\n");
	EFPRINTF(fp, "\t\t\tLegacy Devices Supported (V2) : 0\n");
	EFPRINTF(fp, "\t\t\t8042 Present on ports 60/64 (V2) : 0\n");
	EFPRINTF(fp, "\t\t\tVGA Not Present (V4) : 1\n");
	EFPRINTF(fp, "\t\t\tMSI Not Supported (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tPCIe ASPM Not Supported (V4) : 1\n");
	EFPRINTF(fp, "\t\t\tCMOS RTC Not Present (V5) : 0\n");
	EFPRINTF(fp, "[0001]\t\tReserved : 00\n");
	EFPRINTF(fp, "[0004]\t\tFlags (decoded below) : 00000000\n");
	EFPRINTF(fp, "\t\t\tWBINVD instruction is operational (V1) : 1\n");
	EFPRINTF(fp, "\t\t\tWBINVD flushes all caches (V1) : 0\n");
	EFPRINTF(fp, "\t\t\tAll CPUs support C1 (V1) : 1\n");
	EFPRINTF(fp, "\t\t\tC2 works on MP system (V1) : 0\n");
	EFPRINTF(fp, "\t\t\tControl Method Power Button (V1) : 0\n");
	EFPRINTF(fp, "\t\t\tControl Method Sleep Button (V1) : 1\n");
	EFPRINTF(fp, "\t\t\tRTC wake not in fixed reg space (V1) : 0\n");
	EFPRINTF(fp, "\t\t\tRTC can wake system from S4 (V1) : 0\n");
	EFPRINTF(fp, "\t\t\t32-bit PM Timer (V1) : 1\n");
	EFPRINTF(fp, "\t\t\tDocking Supported (V1) : 0\n");
	EFPRINTF(fp, "\t\t\tReset Register Supported (V2) : 1\n");
	EFPRINTF(fp, "\t\t\tSealed Case (V3) : 0\n");
	EFPRINTF(fp, "\t\t\tHeadless - No Video (V3) : 1\n");
	EFPRINTF(fp, "\t\t\tUse native instr after SLP_TYPx (V3) : 0\n");
	EFPRINTF(fp, "\t\t\tPCIEXP_WAK Bits Supported (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tUse Platform Timer (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tRTC_STS valid on S4 wake (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tRemote Power-on capable (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tUse APIC Cluster Model (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tUse APIC Physical Destination Mode (V4) : 0\n");
	EFPRINTF(fp, "\t\t\tHardware Reduced (V5) : 0\n");
	EFPRINTF(fp, "\t\t\tLow Power S0 Idle (V5) : 0\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	    "[0012]\t\tReset Register : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 08\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tEncoded Access Width : 01 [Byte Access:8]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000CF9\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0001]\t\tValue to cause reset : 0E\n");
	EFPRINTF(fp, "[0002]\t\tARM Flags (decoded below): 0000\n");
	EFPRINTF(fp, "\t\t\tPSCI Compliant : 0\n");
	EFPRINTF(fp, "\t\t\tMust use HVC for PSCI : 0\n");
	EFPRINTF(fp, "[0001]\t\tFADT Minor Revision : 01\n");
	EFPRINTF(fp, "[0008]\t\tFACS Address : 00000000%08X\n",
	    basl_acpi_base + FACS_OFFSET);
	EFPRINTF(fp, "[0008]\t\tDSDT Address : 00000000%08X\n",
	    basl_acpi_base + DSDT_OFFSET);
	EFPRINTF(fp,
	    "[0012]\t\tPM1A Event Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 20\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tEncoded Access Width : 02 [Word Access:16]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 00000000%08X\n",
	    PM1A_EVT_ADDR);
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	    "[0012]\t\tPM1B Event Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 00\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp,
	    "[0001]\t\tEncoded Access Width : 00 [Undefined/Legacy]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	    "[0012]\t\tPM1A Control Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 10\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tEncoded Access Width : 02 [Word Access:16]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 00000000%08X\n",
	    VIRTUAL_PM1A_CNT_ADDR);
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	    "[0012]\t\tPM1B Control Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 00\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp,
	    "[0001]\t\tEncoded Access Width : 00 [Undefined/Legacy]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	    "[0012]\t\tPM2 Control Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 08\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp,
	    "[0001]\t\tEncoded Access Width : 00 [Undefined/Legacy]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");
	EFPRINTF(fp, "\n");

	/* Valid for dm */
	EFPRINTF(fp,
	    "[0012]\t\tPM Timer Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 20\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp,
	    "[0001]\t\tEncoded Access Width : 03 [DWord Access:32]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 00000000%08X\n",
	    IO_PMTMR);
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0012]\t\tGPE0 Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 00\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tEncoded Access Width : 01 [Byte Access:8]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0012]\t\tGPE1 Block : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 00\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp,
	    "[0001]\t\tEncoded Access Width : 00 [Undefined/Legacy]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	   "[0012]\t\tSleep Control Register : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 08\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tEncoded Access Width : 01 [Byte Access:8]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp,
	    "[0012]\t\tSleep Status Register : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 01 [SystemIO]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 08\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp, "[0001]\t\tEncoded Access Width : 01 [Byte Access:8]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : 0000000000000000\n");

	EFFLUSH(fp);

	return 0;
}

static int
basl_fwrite_hpet(FILE *fp, struct vmctx *ctx)
{
	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm HPET template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"HPET\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 01\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"DMHPET  \"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000001\n");

	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0004]\t\tHardware Block ID : %08X\n",
	    (uint32_t)vhpet_capabilities());
	EFPRINTF(fp,
	    "[0012]\t\tTimer Block Register : [Generic Address Structure]\n");
	EFPRINTF(fp, "[0001]\t\tSpace ID : 00 [SystemMemory]\n");
	EFPRINTF(fp, "[0001]\t\tBit Width : 00\n");
	EFPRINTF(fp, "[0001]\t\tBit Offset : 00\n");
	EFPRINTF(fp,
		 "[0001]\t\tEncoded Access Width : 00 [Undefined/Legacy]\n");
	EFPRINTF(fp, "[0008]\t\tAddress : %016X\n", VHPET_BASE);
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0001]\t\tSequence Number : 00\n");
	EFPRINTF(fp, "[0002]\t\tMinimum Clock Ticks : 0000\n");
	EFPRINTF(fp, "[0004]\t\tFlags (decoded below) : 00000001\n");
	EFPRINTF(fp, "\t\t\t4K Page Protect : 1\n");
	EFPRINTF(fp, "\t\t\t64K Page Protect : 0\n");
	EFPRINTF(fp, "\n");

	EFFLUSH(fp);

	return 0;
}

static int
basl_fwrite_mcfg(FILE *fp, struct vmctx *ctx)
{
	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm MCFG template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"MCFG\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 01\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"DM \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"DMMCFG  \"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000001\n");

	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");
	EFPRINTF(fp, "[0008]\t\tReserved : 0\n");
	EFPRINTF(fp, "\n");

	EFPRINTF(fp, "[0008]\t\tBase Address : %016lX\n", PCI_EMUL_ECFG_BASE);
	EFPRINTF(fp, "[0002]\t\tSegment Group Number: 0000\n");
	EFPRINTF(fp, "[0001]\t\tStart Bus Number: 00\n");
	EFPRINTF(fp, "[0001]\t\tEnd Bus Number: FF\n");
	EFPRINTF(fp, "[0004]\t\tReserved : 0\n");
	EFFLUSH(fp);

	return 0;
}

static int
basl_fwrite_nhlt(FILE *fp, struct vmctx *ctx)
{
	int offset, len;
	uint8_t data;
	int fd = open("/sys/firmware/acpi/tables/NHLT", O_RDONLY);

	if (fd < 0) {
		pr_err("Open host NHLT fail! %s\n", strerror(errno));
		return -1;
	}


	audio_nhlt_len = lseek(fd, 0, SEEK_END);
	/* check if file size exceeds reserved room */
	if (audio_nhlt_len > DSDT_OFFSET - NHLT_OFFSET) {
		pr_err("Host NHLT exceeds reserved room!\n");
		close(fd);
		return -1;
	}

	/* skip 36 bytes as NHLT table header */
	offset = lseek(fd, 36, SEEK_SET);
	if (offset != 36) {
		pr_err("Seek NHLT data fail! %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm NHLT template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"NHLT\"\n");
	EFPRINTF(fp, "[0004]\t\tTable Length : 00000000\n");
	EFPRINTF(fp, "[0001]\t\tRevision : 00\n");
	EFPRINTF(fp, "[0001]\t\tChecksum : 00\n");
	EFPRINTF(fp, "[0006]\t\tOem ID : \"INTEL \"\n");
	EFPRINTF(fp, "[0008]\t\tOem Table ID : \"NHLT-GPA\"\n");
	EFPRINTF(fp, "[0004]\t\tOem Revision : 00000003\n");

	/* iasl will fill in the compiler ID/revision fields */
	EFPRINTF(fp, "[0004]\t\tAsl Compiler ID : \"xxxx\"\n");
	EFPRINTF(fp, "[0004]\t\tAsl Compiler Revision : 00000000\n");

	while ((len = read(fd, &data, 1)) == 1)
		EFPRINTF(fp, "UINT8 : %02X\n", data);

	if (len < 0) {
		pr_err("Read host NHLT fail! %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	EFFLUSH(fp);
	close(fd);

	return 0;
}

static int
basl_fwrite_facs(FILE *fp, struct vmctx *ctx)
{
	EFPRINTF(fp, "/*\n");
	EFPRINTF(fp, " * dm FACS template\n");
	EFPRINTF(fp, " */\n");
	EFPRINTF(fp, "[0004]\t\tSignature : \"FACS\"\n");
	EFPRINTF(fp, "[0004]\t\tLength : 00000040\n");
	EFPRINTF(fp, "[0004]\t\tHardware Signature : 00000000\n");
	EFPRINTF(fp, "[0004]\t\t32 Firmware Waking Vector : 00000000\n");
	EFPRINTF(fp, "[0004]\t\tGlobal Lock : 00000000\n");
	EFPRINTF(fp, "[0004]\t\tFlags (decoded below) : 00000000\n");
	EFPRINTF(fp, "\t\t\tS4BIOS Support Present : 0\n");
	EFPRINTF(fp, "\t\t\t64-bit Wake Supported (V2) : 0\n");
	EFPRINTF(fp,
	    "[0008]\t\t64 Firmware Waking Vector : 0000000000000000\n");
	EFPRINTF(fp, "[0001]\t\tVersion : 02\n");
	EFPRINTF(fp, "[0003]\t\tReserved : 000000\n");
	EFPRINTF(fp, "[0004]\t\tOspmFlags (decoded below) : 00000000\n");
	EFPRINTF(fp, "\t\t\t64-bit Wake Env Required (V2) : 0\n");

	EFFLUSH(fp);

	return 0;
}

/*
 * Helper routines for writing to the DSDT from other modules.
 */
void
dsdt_line(const char *fmt, ...)
{
	va_list ap;

	if (dsdt_error != 0)
		return;

	if (strcmp(fmt, "") != 0) {
		if (dsdt_indent_level != 0)
			EFPRINTF(dsdt_fp, "%*c", dsdt_indent_level * 2, ' ');
		va_start(ap, fmt);
		if (vfprintf(dsdt_fp, fmt, ap) < 0)
			goto err_exit;
		va_end(ap);
	}

	EFPRINTF(dsdt_fp, "\n");

	return;

err_exit:
	va_end(ap);
	dsdt_error = -1;
}

void
dsdt_indent(int levels)
{
	dsdt_indent_level += levels;
}

void
dsdt_unindent(int levels)
{
	if (dsdt_indent_level >= levels)
		dsdt_indent_level -= levels;
}

void
dsdt_fixed_ioport(uint16_t iobase, uint16_t length)
{
	dsdt_line("IO (Decode16,");
	dsdt_line("  0x%04X,             // Range Minimum", iobase);
	dsdt_line("  0x%04X,             // Range Maximum", iobase);
	dsdt_line("  0x01,               // Alignment");
	dsdt_line("  0x%02X,               // Length", length);
	dsdt_line("  )");
}

void
dsdt_fixed_irq(uint8_t irq)
{
	dsdt_line("IRQNoFlags ()");
	dsdt_line("  {%d}", irq);
}

void
dsdt_fixed_mem32(uint32_t base, uint32_t length)
{
	dsdt_line("Memory32Fixed (ReadWrite,");
	dsdt_line("  0x%08X,         // Address Base", base);
	dsdt_line("  0x%08X,         // Address Length", length);
	dsdt_line("  )");
}

static int
basl_fwrite_dsdt(FILE *fp, struct vmctx *ctx)
{
	dsdt_fp = fp;
	dsdt_error = 0;
	dsdt_indent_level = 0;

	dsdt_line("/*");
	dsdt_line(" * dm DSDT template");
	dsdt_line(" */");
	dsdt_line("DefinitionBlock (\"dm_dsdt.aml\", \"DSDT\", 2,"
			"\"DM \", \"DMDSDT  \", 0x00000001)");
	dsdt_line("{");
	dsdt_line("  Name (_S3, Package ()");
	dsdt_line("  {");
	dsdt_line("      0x03,");
	dsdt_line("      Zero,");
	dsdt_line("  })");
	dsdt_line("  Name (_S5, Package ()");
	dsdt_line("  {");
	dsdt_line("      0x05,");
	dsdt_line("      Zero,");
	dsdt_line("  })");

	pci_write_dsdt();

	dsdt_line("");
	dsdt_line("  Scope (_SB.PCI0)");
	dsdt_line("  {");
	dsdt_line("    Device (HPET)");
	dsdt_line("    {");
	dsdt_line("      Name (_HID, EISAID(\"PNP0103\"))");
	dsdt_line("      Name (_UID, 0)");
	dsdt_line("      Name (_CRS, ResourceTemplate ()");
	dsdt_line("      {");
	dsdt_indent(4);
	dsdt_fixed_mem32(VHPET_BASE, VHPET_SIZE);
	dsdt_unindent(4);
	dsdt_line("      })");
	dsdt_line("    }");
	dsdt_line("  }");

	acpi_dev_write_dsdt(ctx);

	pm_write_dsdt(ctx, basl_ncpu);

	dsdt_line("}");

	if (dsdt_error != 0)
		return dsdt_error;

	EFFLUSH(fp);

	return 0;
}

static int
basl_open(struct basl_fio *bf, int suffix)
{
	int err;

	err = 0;

	if (suffix) {
		strncpy(bf->f_name, basl_stemplate, MAXPATHLEN);
		bf->fd = mkstemps(bf->f_name, strlen(ASL_SUFFIX));
	} else {
		strncpy(bf->f_name, basl_template, MAXPATHLEN);
		bf->fd = mkstemp(bf->f_name);
	}

	if (bf->fd > 0) {
		bf->fp = fdopen(bf->fd, "w+");
		if (bf->fp == NULL) {
			unlink(bf->f_name);
			close(bf->fd);
		}
	} else
		err = -1;

	return err;
}

static void
basl_close(struct basl_fio *bf)
{
	if (!basl_keep_temps)
		unlink(bf->f_name);
	fclose(bf->fp);
}

static int
basl_start(struct basl_fio *in, struct basl_fio *out)
{
	int err;

	err = basl_open(in, 0);
	if (!err) {
		err = basl_open(out, 1);
		if (err)
			basl_close(in);

	}

	return err;
}

static void
basl_end(struct basl_fio *in, struct basl_fio *out)
{
	basl_close(in);
	basl_close(out);
}

static int
basl_load(struct vmctx *ctx, int fd, uint64_t off)
{
	struct stat sb;
	void *gaddr;

	if (fstat(fd, &sb) < 0)
		return -1;

	gaddr = paddr_guest2host(ctx, basl_acpi_base + off, sb.st_size);
	if (gaddr == NULL)
		return -1;

	if (read(fd, gaddr, sb.st_size) < 0)
		return -1;

	return 0;
}

static int
basl_compile(struct vmctx *ctx,
		int (*fwrite_section)(FILE *, struct vmctx *),
		uint64_t offset)
{
	struct basl_fio io[2];
	static char iaslbuf[3*MAXPATHLEN + 10];
	int err;

	err = basl_start(&io[0], &io[1]);
	if (!err) {
		err = (*fwrite_section)(io[0].fp, ctx);

		if (!err) {
			/*
			 * iasl sends the results of the compilation to
			 * stdout. Shut this down by using the shell to
			 * redirect stdout to /dev/null, unless the user
			 * has requested verbose output for debugging
			 * purposes
			 */
			if (basl_verbose_iasl)
				snprintf(iaslbuf, sizeof(iaslbuf),
					 "%s -p %s %s",
					 ASL_COMPILER,
					 io[1].f_name, io[0].f_name);
			else
				snprintf(iaslbuf, sizeof(iaslbuf),
					 "/bin/sh -c \"%s -p %s %s\" 1> /dev/null",
					 ASL_COMPILER,
					 io[1].f_name, io[0].f_name);

			err = system(iaslbuf);

			if (!err) {
				/*
				 * Copy the aml output file into guest
				 * memory at the specified location
				 */
				err = basl_load(ctx, io[1].fd, offset);
			} else
				err = -1;
		}
		basl_end(&io[0], &io[1]);
	}

	return err;
}

static int
basl_make_templates(void)
{
	const char *tmpdir;
	int err;
	size_t len;

	err = 0;

	tmpdir = getenv("ACPI_TMPDIR");
	if (tmpdir == NULL || *tmpdir == '\0') {
		tmpdir = getenv("TMPDIR");
		if (tmpdir == NULL || *tmpdir == '\0')
			tmpdir = _PATH_TMP;
	}

	len = strnlen(tmpdir, MAXPATHLEN);

	if ((len + sizeof(ASL_TEMPLATE) + 1) < MAXPATHLEN) {
		strncpy(basl_template, tmpdir, len + 1);
		while (len > 0 && basl_template[len - 1] == '/')
			len--;
		basl_template[len] = '/';
		strncpy(&basl_template[len + 1], ASL_TEMPLATE,
					MAXPATHLEN - len - 1);
	} else
		err = -1;

	if (!err) {
		/*
		 * len has been intialized (and maybe adjusted) above
		 */
		if ((len + sizeof(ASL_TEMPLATE) + 1 +
		     sizeof(ASL_SUFFIX)) < MAXPATHLEN) {
			strncpy(basl_stemplate, tmpdir, len + 1);
			basl_stemplate[len] = '/';
			strncpy(&basl_stemplate[len + 1], ASL_TEMPLATE,
					MAXPATHLEN - len - 1);
			len += sizeof(ASL_TEMPLATE);
			strncpy(&basl_stemplate[len], ASL_SUFFIX,
					sizeof(ASL_TEMPLATE));
		} else
			err = -1;
	}

	return err;
}

static struct {
	int	(*wsect)(FILE *fp, struct vmctx *ctx);
	uint64_t  offset;
	bool	valid;
} basl_ftables[] = {
	{ basl_fwrite_rsdp, 0,		 true  },
	{ basl_fwrite_rsdt, RSDT_OFFSET, true  },
	{ basl_fwrite_xsdt, XSDT_OFFSET, true  },
	{ basl_fwrite_madt, MADT_OFFSET, true  },
	{ basl_fwrite_fadt, FADT_OFFSET, true  },
	{ basl_fwrite_hpet, HPET_OFFSET, true  },
	{ basl_fwrite_mcfg, MCFG_OFFSET, true  },
	{ basl_fwrite_facs, FACS_OFFSET, true  },
	{ basl_fwrite_nhlt, NHLT_OFFSET, false }, /*valid with audio ptdev*/
	{ basl_fwrite_tpm2, TPM2_OFFSET, false },
	{ basl_fwrite_dsdt, DSDT_OFFSET, true  }
};

void
acpi_table_enable(int num)
{
	if (num > (ARRAY_SIZE(basl_ftables) - 1)) {
		pr_err("Enable non-existing ACPI tables:%d\n", num);
		return;
	}

	basl_ftables[num].valid = true;
}

static bool
acpi_table_is_valid(int num)
{
	return basl_ftables[num].valid;
}

uint32_t
get_acpi_base(void)
{
	return basl_acpi_base;
}

uint32_t
get_acpi_table_length(void)
{
	return ACPI_LENGTH;
}

int
acpi_build(struct vmctx *ctx, int ncpu)
{
#define RTCT_BUF_LEN	0x200
	int err;
	int i;
	size_t vrtct_len;
	uint8_t *vrtct;

	basl_ncpu = ncpu;

	/*
	 * For debug, allow the user to have iasl compiler output sent
	 * to stdout rather than /dev/null
	 */
	if (getenv("ACPI_VERBOSE_IASL"))
		basl_verbose_iasl = 1;

	/*
	 * Allow the user to keep the generated ASL files for debugging
	 * instead of deleting them following use
	 */
	if (getenv("ACPI_KEEPTMPS"))
		basl_keep_temps = 1;

	i = 0;
	err = basl_make_templates();

	/*
	 * Run through all the ASL files, compiling them and
	 * copying them into guest memory
	 */
	while (!err && (i < ARRAY_SIZE(basl_ftables))) {
		if ((basl_ftables[i].offset == TPM2_OFFSET) &&
			(ctx->tpm_dev != NULL || pt_tpm2)) {
				basl_ftables[i].valid = true;
		}

		if (acpi_table_is_valid(i))
			err = basl_compile(ctx, basl_ftables[i].wsect,
					basl_ftables[i].offset);
		i++;
	}

	if (ssram) {
		vrtct = get_vssram_vrtct();
		if (vrtct == NULL) {
			return -1;
		}

		vrtct_len = ((struct acpi_table_hdr *)vrtct)->length;
		if (vrtct_len > RTCT_BUF_LEN) {
			/* need to modify DSDT_OFFSET corresponding */
			pr_err("Error: Size of vRTCT (%d bytes) overflows.\n", vrtct_len);
			return -1;
		}
		memcpy(vm_map_gpa(ctx, ACPI_BASE + RTCT_OFFSET, vrtct_len), vrtct, vrtct_len);
	}

	return err;
}

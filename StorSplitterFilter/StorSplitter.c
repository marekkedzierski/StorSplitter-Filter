#include <ntddk.h>
#include <wdf.h>
#include <ntddscsi.h>
#include <scsi.h>

#include <wdmsec.h>
#pragma comment(lib, "wdmsec.lib")

#define DEFAULT_MAX_TRANSFER_SIZE (63 * PAGE_SIZE)

// Master Switch: 1 = Enabled (Default), 0 = Disabled
LONG g_SplitterEnabled = 1;

// Point this to the header that is shared between user and kernel
#include "../StorSplitterFilter/SharedIoctls.h"

// Standard SCSI VPD Page 0xB0 (Block Limits)
#pragma pack(push, 1)
typedef struct _CUSTOM_VPD_BLOCK_LIMITS_PAGE
{
	UCHAR DeviceType : 5;
	UCHAR PeripheralQualifier : 3;
	UCHAR PageCode;             // Will be 0xB0
	USHORT PageLength;
	UCHAR WSNZ;
	UCHAR MaxCompareAndWriteLength;
	USHORT OptimalTransferLengthGranularity;
	ULONG MaximumTransferLength; // Blocks
	ULONG OptimalTransferLength;
	ULONG MaximumPrefetchLength;
} CUSTOM_VPD_BLOCK_LIMITS_PAGE, * PCUSTOM_VPD_BLOCK_LIMITS_PAGE;
#pragma pack(pop)

// ---------------------------------------------------------------------------
// 2. Context Spaces
// ---------------------------------------------------------------------------
typedef struct _FILTER_DEVICE_CONTEXT {
	size_t MaxSafeTransferSizeBytes;
} FILTER_DEVICE_CONTEXT, * PFILTER_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_DEVICE_CONTEXT, GetDeviceContext)

typedef struct _PARENT_REQUEST_CONTEXT {
	LONG OutstandingChildren;  // Reference counter
	NTSTATUS FinalStatus;      // Holds the merged result
	WDFSPINLOCK Lock;          // Protects the status overwrite
} PARENT_REQUEST_CONTEXT, * PPARENT_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PARENT_REQUEST_CONTEXT, GetParentContext)

EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
EVT_WDF_IO_QUEUE_IO_READ EvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE EvtIoWrite;
EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtChildRequestCompleted;

VOID EvtControlDeviceIoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(InputBufferLength);

	NTSTATUS status = STATUS_SUCCESS;
	size_t bytesReturned = 0;

	switch (IoControlCode) 
	{
	case IOCTL_SPLITTER_ENABLE:
		InterlockedExchange(&g_SplitterEnabled, 1);
		DbgPrint("[STOR-SPLIT] Splitter is now ENABLED system-wide.\n");
		break;

	case IOCTL_SPLITTER_DISABLE:
		InterlockedExchange(&g_SplitterEnabled, 0);
		DbgPrint("[STOR-SPLIT] Splitter is now DISABLED system-wide.\n");
		break;

	case IOCTL_SPLITTER_QUERY_STATUS:
		if (OutputBufferLength < sizeof(LONG)) 
		{
			status = STATUS_BUFFER_TOO_SMALL;
		}
		else 
		{
			LONG* outBuffer = NULL;
			status = WdfRequestRetrieveOutputBuffer(Request, sizeof(LONG), (PVOID*)&outBuffer, NULL);
			if (NT_SUCCESS(status)) 
			{
				*outBuffer = InterlockedCompareExchange(&g_SplitterEnabled, 0, 0);
				bytesReturned = sizeof(LONG);
			}
		}
		break;

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

NTSTATUS CreateControlDevice(WDFDRIVER Driver)
{
	DECLARE_CONST_UNICODE_STRING(sddlString, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

	PWDFDEVICE_INIT deviceInit = WdfControlDeviceInitAllocate(Driver, &sddlString);
	if (deviceInit == NULL) return STATUS_INSUFFICIENT_RESOURCES;

	WdfDeviceInitSetExclusive(deviceInit, TRUE);

	DECLARE_CONST_UNICODE_STRING(ntDeviceName, L"\\Device\\StorSplitterCtrl");
	DECLARE_CONST_UNICODE_STRING(symbolicLinkName, L"\\DosDevices\\StorSplitterCtrl");

	NTSTATUS status = WdfDeviceInitAssignName(deviceInit, &ntDeviceName);
	if (!NT_SUCCESS(status)) 
	{
		WdfDeviceInitFree(deviceInit);
		return status;
	}

	WDFDEVICE controlDevice;
	status = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &controlDevice);
	if (!NT_SUCCESS(status)) 
	{
		WdfDeviceInitFree(deviceInit);
		return status;
	}

	status = WdfDeviceCreateSymbolicLink(controlDevice, &symbolicLinkName);
	if (!NT_SUCCESS(status)) 
	{
		WdfObjectDelete(controlDevice);
		return status;
	}

	WDF_IO_QUEUE_CONFIG ioQueueConfig;
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchSequential);
	ioQueueConfig.EvtIoDeviceControl = EvtControlDeviceIoControl;

	status = WdfIoQueueCreate(controlDevice, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status)) 
	{
		WdfObjectDelete(controlDevice);
		return status;
	}

	WdfControlFinishInitializing(controlDevice);
	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

	WDFDRIVER driver;
	NTSTATUS status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);

	if (NT_SUCCESS(status)) 
	{
		// Create the Control Device endpoint for user-mode apps
		CreateControlDevice(driver);
	}

	return status;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
	UNREFERENCED_PARAMETER(Driver);
	NTSTATUS status;

	WdfFdoInitSetFilter(DeviceInit);

	WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
	pnpCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

	WDF_OBJECT_ATTRIBUTES deviceAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, FILTER_DEVICE_CONTEXT);

	WDFDEVICE device;
	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
	if (!NT_SUCCESS(status)) return status;

	WDF_IO_QUEUE_CONFIG queueConfig;
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
	queueConfig.EvtIoRead = EvtIoRead;
	queueConfig.EvtIoWrite = EvtIoWrite;

	return WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS EvtDevicePrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
	UNREFERENCED_PARAMETER(ResourcesRaw);
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	PFILTER_DEVICE_CONTEXT devCtx = GetDeviceContext(Device);
	WDFIOTARGET target = WdfDeviceGetIoTarget(Device);

	devCtx->MaxSafeTransferSizeBytes = DEFAULT_MAX_TRANSFER_SIZE;

	SCSI_PASS_THROUGH spt = { 0 };
	spt.Length = sizeof(SCSI_PASS_THROUGH);
	spt.CdbLength = 6;
	spt.DataIn = SCSI_IOCTL_DATA_IN;
	spt.DataTransferLength = sizeof(CUSTOM_VPD_BLOCK_LIMITS_PAGE);
	spt.TimeOutValue = 2;
	spt.DataBufferOffset = sizeof(SCSI_PASS_THROUGH);

	spt.Cdb[0] = SCSIOP_INQUIRY;
	spt.Cdb[1] = 0x01;
	spt.Cdb[2] = 0xB0;
	spt.Cdb[4] = sizeof(CUSTOM_VPD_BLOCK_LIMITS_PAGE);

	size_t totalBufferSize = sizeof(SCSI_PASS_THROUGH) + sizeof(CUSTOM_VPD_BLOCK_LIMITS_PAGE);

	PUCHAR buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, totalBufferSize, 'IPDS');
	if (!buffer) return STATUS_SUCCESS;

	RtlCopyMemory(buffer, &spt, sizeof(SCSI_PASS_THROUGH));

	WDF_MEMORY_DESCRIPTOR inputDescriptor;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, buffer, (ULONG)totalBufferSize);

	WDF_MEMORY_DESCRIPTOR outputDescriptor;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, buffer, (ULONG)totalBufferSize);

	NTSTATUS status = WdfIoTargetSendIoctlSynchronously(
		target, NULL, IOCTL_SCSI_PASS_THROUGH,
		&inputDescriptor, &outputDescriptor, NULL, NULL
	);

	if (NT_SUCCESS(status)) {
		PSCSI_PASS_THROUGH pSpt = (PSCSI_PASS_THROUGH)buffer;
		if (pSpt->ScsiStatus == SCSISTAT_GOOD) 
		{
			PCUSTOM_VPD_BLOCK_LIMITS_PAGE pVpd = (PCUSTOM_VPD_BLOCK_LIMITS_PAGE)(buffer + sizeof(SCSI_PASS_THROUGH));

			if (pVpd->PageCode == 0xB0) 
			{
				ULONG maxTransferBlocks = _byteswap_ulong(pVpd->MaximumTransferLength);

				if (maxTransferBlocks > 0) 
				{
					ULONG maxBytes = maxTransferBlocks * 512;
					DbgPrint("[SCSI INQUIRY] HW Max Transfer: %u blocks (%u bytes)\n", maxTransferBlocks, maxBytes);
					devCtx->MaxSafeTransferSizeBytes = maxBytes;
				}
			}
		}
	}

	ExFreePoolWithTag(buffer, 'IPDS');
	return STATUS_SUCCESS;
}

VOID EvtChildRequestCompleted(WDFREQUEST ChildRequest, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
	UNREFERENCED_PARAMETER(Target);
	WDFREQUEST parentRequest = (WDFREQUEST)Context;
	PPARENT_REQUEST_CONTEXT parentCtx = GetParentContext(parentRequest);
	NTSTATUS childStatus = Params->IoStatus.Status;

	if (!NT_SUCCESS(childStatus)) 
	{
		WdfSpinLockAcquire(parentCtx->Lock);
		parentCtx->FinalStatus = childStatus;
		WdfSpinLockRelease(parentCtx->Lock);
	}

	WdfObjectDelete(ChildRequest);
	if (InterlockedDecrement(&parentCtx->OutstandingChildren) == 0) 
	{
		NTSTATUS finalStatus = parentCtx->FinalStatus;

		WDF_REQUEST_PARAMETERS parentParams;
		WDF_REQUEST_PARAMETERS_INIT(&parentParams);
		WdfRequestGetParameters(parentRequest, &parentParams);

		size_t bytesCompleted = NT_SUCCESS(finalStatus) ?
			(parentParams.Type == WdfRequestTypeRead ? parentParams.Parameters.Read.Length : parentParams.Parameters.Write.Length) : 0;

		WdfRequestCompleteWithInformation(parentRequest, finalStatus, bytesCompleted);
	}
}

VOID EvtIoRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	WDFIOTARGET target = WdfDeviceGetIoTarget(device);
	PFILTER_DEVICE_CONTEXT devCtx = GetDeviceContext(device);
	size_t activeSafeSize = devCtx->MaxSafeTransferSizeBytes;

	// Fast Path: Pass-through if Disabled OR if the request is small enough
	if (g_SplitterEnabled == 0 || Length <= activeSafeSize) 
	{
		WDF_REQUEST_SEND_OPTIONS options;
		WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
		if (!WdfRequestSend(Request, target, &options)) {
			WdfRequestComplete(Request, WdfRequestGetStatus(Request));
		}
		return;
	}

	WDF_REQUEST_PARAMETERS params;
	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);
	LONGLONG deviceOffset = params.Parameters.Read.DeviceOffset;

	WDFMEMORY parentMemory;
	NTSTATUS status = WdfRequestRetrieveOutputMemory(Request, &parentMemory);
	if (!NT_SUCCESS(status)) {
		WdfRequestComplete(Request, status);
		return;
	}

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PARENT_REQUEST_CONTEXT);
	PPARENT_REQUEST_CONTEXT parentCtx;
	status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&parentCtx);
	if (!NT_SUCCESS(status)) {
		WdfRequestComplete(Request, status);
		return;
	}

	parentCtx->FinalStatus = STATUS_SUCCESS;

	WDF_OBJECT_ATTRIBUTES lockAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
	lockAttributes.ParentObject = Request;
	WdfSpinLockCreate(&lockAttributes, &parentCtx->Lock);

	parentCtx->OutstandingChildren = 1;

	size_t memoryOffset = 0;

	while (memoryOffset < Length) 
	{
		size_t chunkSize = Length - memoryOffset;
		if (chunkSize > activeSafeSize) chunkSize = activeSafeSize;

		WDFREQUEST childRequest;
		WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, target, &childRequest);

		WDFMEMORY_OFFSET memOffset;
		memOffset.BufferOffset = memoryOffset;
		memOffset.BufferLength = chunkSize;

		status = WdfIoTargetFormatRequestForRead(target, childRequest, parentMemory, &memOffset, &deviceOffset);

		if (!NT_SUCCESS(status)) {
			WdfObjectDelete(childRequest);
			WdfSpinLockAcquire(parentCtx->Lock);
			parentCtx->FinalStatus = status;
			WdfSpinLockRelease(parentCtx->Lock);
			break;
		}

		WdfRequestSetCompletionRoutine(childRequest, EvtChildRequestCompleted, Request);
		InterlockedIncrement(&parentCtx->OutstandingChildren);

		if (!WdfRequestSend(childRequest, target, WDF_NO_SEND_OPTIONS)) {
			status = WdfRequestGetStatus(childRequest);
			WdfSpinLockAcquire(parentCtx->Lock);
			parentCtx->FinalStatus = status;
			WdfSpinLockRelease(parentCtx->Lock);
			WdfObjectDelete(childRequest);
			InterlockedDecrement(&parentCtx->OutstandingChildren);
		}

		memoryOffset += chunkSize;
		deviceOffset += chunkSize;
	}

	if (InterlockedDecrement(&parentCtx->OutstandingChildren) == 0) {
		status = parentCtx->FinalStatus;
		WdfRequestComplete(Request, status);
	}
}

VOID EvtIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	WDFIOTARGET target = WdfDeviceGetIoTarget(device);
	PFILTER_DEVICE_CONTEXT devCtx = GetDeviceContext(device);
	size_t activeSafeSize = devCtx->MaxSafeTransferSizeBytes;

	// Fast Path: Pass-through if Disabled OR if the request is small enough
	if (g_SplitterEnabled == 0 || Length <= activeSafeSize) 
	{
		WDF_REQUEST_SEND_OPTIONS options;
		WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
		if (!WdfRequestSend(Request, target, &options)) {
			WdfRequestComplete(Request, WdfRequestGetStatus(Request));
		}
		return;
	}

	WDF_REQUEST_PARAMETERS params;
	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);
	LONGLONG deviceOffset = params.Parameters.Write.DeviceOffset;

	WDFMEMORY parentMemory;
	NTSTATUS status = WdfRequestRetrieveInputMemory(Request, &parentMemory);
	if (!NT_SUCCESS(status)) {
		WdfRequestComplete(Request, status);
		return;
	}

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PARENT_REQUEST_CONTEXT);
	PPARENT_REQUEST_CONTEXT parentCtx;
	status = WdfObjectAllocateContext(Request, &attributes, (PVOID*)&parentCtx);
	if (!NT_SUCCESS(status)) 
	{
		WdfRequestComplete(Request, status);
		return;
	}

	parentCtx->FinalStatus = STATUS_SUCCESS;

	WDF_OBJECT_ATTRIBUTES lockAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
	lockAttributes.ParentObject = Request;
	WdfSpinLockCreate(&lockAttributes, &parentCtx->Lock);

	parentCtx->OutstandingChildren = 1;

	size_t memoryOffset = 0;

	while (memoryOffset < Length) 
	{
		size_t chunkSize = Length - memoryOffset;
		if (chunkSize > activeSafeSize) chunkSize = activeSafeSize;

		WDFREQUEST childRequest;
		WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, target, &childRequest);

		WDFMEMORY_OFFSET memOffset;
		memOffset.BufferOffset = memoryOffset;
		memOffset.BufferLength = chunkSize;

		status = WdfIoTargetFormatRequestForWrite(target, childRequest, parentMemory, &memOffset, &deviceOffset);

		if (!NT_SUCCESS(status)) 
		{
			WdfObjectDelete(childRequest);
			WdfSpinLockAcquire(parentCtx->Lock);
			parentCtx->FinalStatus = status;
			WdfSpinLockRelease(parentCtx->Lock);
			break;
		}

		WdfRequestSetCompletionRoutine(childRequest, EvtChildRequestCompleted, Request);
		InterlockedIncrement(&parentCtx->OutstandingChildren);

		if (!WdfRequestSend(childRequest, target, WDF_NO_SEND_OPTIONS)) 
		{
			status = WdfRequestGetStatus(childRequest);
			WdfSpinLockAcquire(parentCtx->Lock);
			parentCtx->FinalStatus = status;
			WdfSpinLockRelease(parentCtx->Lock);
			WdfObjectDelete(childRequest);
			InterlockedDecrement(&parentCtx->OutstandingChildren);
		}

		memoryOffset += chunkSize;
		deviceOffset += chunkSize;
	}

	if (InterlockedDecrement(&parentCtx->OutstandingChildren) == 0) 
	{
		status = parentCtx->FinalStatus;
		WdfRequestComplete(Request, status);
	}
}
#include "DriverMain.h"

#include "FilteringEngine.h"
#include "InjectionEngine.h"
#include "UserModeBufferHandler.h"

//DECLARE_CONST_UNICODE_STRING(
//	SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_R_RES_R,
//	L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GR;;;WD)(A;;GR;;;RC)"
//	);

#define PACKET_IPV4_DESTINATION_OFFSET 0x10
	
KKDRV_QUEUE_DATA gPacketQueue;
UINT64 gActiveFilterRangeInbound;
UINT64 gActiveFilterRangeOutbound;
UINT64 gActiveFilterLocal;
UINT32 gCalloutID;
WDFREQUEST gPendingRequest;
HANDLE gFilteringEngineHandle;
HANDLE gInjectionEngineHandle;
NDIS_HANDLE gPoolHandle;

VOID
kkdrvUnload(
	_In_ PDRIVER_OBJECT pDriverObject
)
{
	UNREFERENCED_PARAMETER(pDriverObject);

	EnginesCleanup();
}
	
VOID
EnginesCleanup()
{
	StopInjectionEngine(
		&gInjectionEngineHandle
		);

	StopFilterEngine(
		&gFilteringEngineHandle,
		&gCalloutID,
		&gActiveFilterRangeInbound,
		&gActiveFilterRangeOutbound,
		&gActiveFilterLocal
		);

	NdisFreeNetBufferListPool(gPoolHandle);
}

NTSTATUS
DriverEntry(
_In_ PDRIVER_OBJECT  pDriverObject,
_In_ PUNICODE_STRING pRegistryPath
)
{
	NTSTATUS status;
	WDFDRIVER driver;
	WDFDEVICE device;
	WDF_DRIVER_CONFIG config;
	//WDF_FILEOBJECT_CONFIG fileConfig;
	PWDFDEVICE_INIT deviceInit;
	WDF_OBJECT_ATTRIBUTES deviceAttributes = { 0 };

	WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK); // kkdrvDriverDeviceAdd);
	config.DriverInitFlags |= WdfDriverInitNonPnpDriver;

	status = WdfDriverCreate(
		pDriverObject,
		pRegistryPath,
		WDF_NO_OBJECT_ATTRIBUTES,
		&config,
		&driver
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfDriverCreate, status);
		goto Exit;
	}

	pDriverObject->DriverUnload = kkdrvUnload;

	// Device init start

	deviceInit = WdfControlDeviceInitAllocate(driver,
		&SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
	if (deviceInit == NULL)
	{
		REPORT_ERROR(WdfControlDeviceInitAllocate, status);
		goto Exit;
	}
	WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_NETWORK);
	WdfDeviceInitSetCharacteristics(deviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);

	DECLARE_CONST_UNICODE_STRING(deviceName, DEVICE_NAME);
	status = WdfDeviceInitAssignName(deviceInit, &deviceName);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfDeviceInitAssignName, status);
		goto Exit;
	}
	//WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, kkdrvDeviceFileCreate, NULL, kkdrvFileCleanup);
	//WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

	WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
	deviceAttributes.EvtCleanupCallback = kkdrvCleanupCallback;
	deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
	deviceAttributes.SynchronizationScope = WdfSynchronizationScopeQueue;

	status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfDeviceCreate, status);
		goto Exit;
	}

	status = CreateQueue(&device);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(CreateQueue, status);
		goto Exit;
	}

	DECLARE_CONST_UNICODE_STRING(dosDeviceName, DOS_DEVICE_NAME);
	status = WdfDeviceCreateSymbolicLink(
		device,
		&dosDeviceName
		);
	if (!NT_SUCCESS(status)) {
		REPORT_ERROR(WdfDeviceCreateSymbolicLink, status);
		goto Exit;
	}

	WdfControlFinishInitializing(device);

	// Device init end

	NET_BUFFER_LIST_POOL_PARAMETERS poolParams;

	RtlZeroMemory(&poolParams, sizeof(poolParams));
	poolParams.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
	poolParams.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
	poolParams.Header.Size = sizeof(poolParams);
	poolParams.fAllocateNetBuffer = TRUE;
	poolParams.PoolTag = KKDRV_TAG;
	poolParams.DataSize = 0;
	gPoolHandle = NdisAllocateNetBufferListPool(NULL, &poolParams);
	if (gPoolHandle == NULL)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		REPORT_ERROR(NdisAllocateNetBufferListPool, status);
		goto Exit;
	}

	InitializePacketQueue(&gPacketQueue);

	status = StartFilterEngine(
		&gFilteringEngineHandle,
		&gCalloutID,
		device
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(StartFilterEngine, status);
		goto Exit;
	}
	
	status = StartInjectionEngine(
		&gInjectionEngineHandle
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(StartInjectionEngine, status);
		goto Exit;
	}
	

Exit:
	return status;
}

VOID 
InitializePacketQueue(
	_Inout_ KKDRV_QUEUE_DATA *packetQueue
	)
{
	InitializeListHead(&packetQueue->queue);
	KeInitializeSpinLock(&packetQueue->queueLock);

	packetQueue->queueLength = 0;
	packetQueue->queueLengthMax = KKDRV_MAX_PACKET_QUEUE_LENGTH;
}

VOID
ClearPacketQueue(
	_Inout_ KKDRV_QUEUE_DATA *packetQueue
	)
{
	while (packetQueue->queueLength > 0)
	{
		PLIST_ENTRY entry = RemoveHeadList(&packetQueue->queue);
		ExFreePoolWithTag(entry, KKDRV_TAG);
		packetQueue->queueLength--;
	}
}

NTSTATUS 
CreateQueue(
	_In_ WDFDEVICE *hDevice
	)
{
	WDFQUEUE writeQueue = NULL;
	WDFQUEUE readQueue = NULL;

	NTSTATUS status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG ioWriteQueueConfig;
	WDF_IO_QUEUE_CONFIG ioReadQueueConfig;
	WDF_OBJECT_ATTRIBUTES ioWriteQueueAttributes = { 0 };
	WDF_OBJECT_ATTRIBUTES ioReadQueueAttributes = { 0 };

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
		&ioWriteQueueConfig,
		WdfIoQueueDispatchParallel
		);

	ioWriteQueueConfig.PowerManaged = FALSE;
	ioWriteQueueConfig.EvtIoDeviceControl = kkdrvIoDeviceControl;
	ioWriteQueueConfig.EvtIoWrite = kkdrvIoWrite;

	WDF_OBJECT_ATTRIBUTES_INIT(&ioWriteQueueAttributes);
	ioWriteQueueAttributes.SynchronizationScope = WdfSynchronizationScopeQueue;

	status = WdfIoQueueCreate(
		*hDevice,
		&ioWriteQueueConfig,
		&ioWriteQueueAttributes,
		&writeQueue
		);
	if (!NT_SUCCESS(status)) 
	{
		REPORT_ERROR(WdfIoQueueCreate(Write), status);
		goto Exit;
	}

	WDF_IO_QUEUE_CONFIG_INIT(
		&ioReadQueueConfig,
		WdfIoQueueDispatchSequential
		);

	ioReadQueueConfig.PowerManaged = FALSE;
	ioReadQueueConfig.EvtIoRead = kkdrvIoRead;

	WDF_OBJECT_ATTRIBUTES_INIT(&ioReadQueueAttributes);
	ioReadQueueAttributes.SynchronizationScope = WdfSynchronizationScopeQueue;

	status = WdfIoQueueCreate(
		*hDevice,
		&ioReadQueueConfig,
		&ioReadQueueAttributes,
		&readQueue
		);
	if (!NT_SUCCESS(status)) 
	{
		REPORT_ERROR(WdfIoQueueCreate(Read), status);
		goto Exit;
	}

	status = WdfDeviceConfigureRequestDispatching(
		*hDevice,
		readQueue,
		WdfRequestTypeRead
		);
	if (!NT_SUCCESS(status)) 
	{
		REPORT_ERROR(WdfDeviceConfigureRequestDispatching(Read), status);
		goto Exit;
	}

Exit:

	if (!NT_SUCCESS(status))
	{
		if (writeQueue != NULL)
		{
			WdfObjectDelete(writeQueue);
		}

		if (readQueue != NULL)
		{
			WdfObjectDelete(readQueue);
		}
	}

	return status;
}

VOID 
kkdrvIoDeviceControl(
	_In_  WDFQUEUE Queue,
	_In_  WDFREQUEST Request,
	_In_  size_t OutputBufferLength,
	_In_  size_t InputBufferLength,
	_In_  ULONG IoControlCode
	)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	NTSTATUS status = STATUS_SUCCESS;
	KKDRV_FILTER_DATA *data = NULL;
	size_t bytesRead = 0;

	switch (IoControlCode) 
	{
		case IOCTL_REGISTER:			
			status = WdfRequestRetrieveInputBuffer(
				Request,
				1,
				(void*)&data,
				&bytesRead
				);
			if (!NT_SUCCESS(status))
			{
				REPORT_ERROR(WdfRequestRetrieveInputBuffer, status);
				goto Complete;
			}

			ClearFilters(
				gFilteringEngineHandle,
				&gActiveFilterRangeInbound,
				&gActiveFilterRangeOutbound,
				&gActiveFilterLocal
				);
			
			ClearPacketQueue(&gPacketQueue);

			status = RegisterFilter(
				data,
				gFilteringEngineHandle,
				&gActiveFilterRangeInbound,
				&gActiveFilterRangeOutbound,
				&gActiveFilterLocal
				);
			if (!NT_SUCCESS(status))
			{
				REPORT_ERROR(RegisterFilter, status);
				goto Complete;
			}
			break;

		case IOCTL_RESTART:
			ClearFilters(
				gFilteringEngineHandle,
				&gActiveFilterRangeInbound,
				&gActiveFilterRangeOutbound,
				&gActiveFilterLocal
				);

			ClearPacketQueue(&gPacketQueue);
			break;

		default:
			status = STATUS_INVALID_DEVICE_REQUEST;
			break;
	}

Complete:
	WdfRequestCompleteWithInformation(Request, status, 0);
}

VOID 
kkdrvCleanupCallback(
	_In_  WDFOBJECT Object
	)
{
	UNREFERENCED_PARAMETER(Object);
}

VOID 
kkdrvIoWrite(
	_In_  WDFQUEUE Queue,
	_In_  WDFREQUEST Request,
	_In_  size_t Length
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	PVOID data = NULL;
	size_t bytesRead = 0;

	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(Length);

	status = WdfRequestRetrieveInputBuffer(
		Request,
		1,
		&data,
		&bytesRead
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfRequestRetrieveInputBuffer, status);
		WdfRequestCompleteWithInformation(Request, status, (ULONG_PTR)0);
		return;
	}

	InjectPacketReceive(
		gInjectionEngineHandle,
		data,
		bytesRead,
		&Request
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(InjectPacketReceive, status);
		WdfRequestCompleteWithInformation(Request, status, (ULONG_PTR)0);
		return;
	}
}

VOID
kkdrvIoRead(
	_In_  WDFQUEUE Queue,
	_In_  WDFREQUEST Request,
	_In_  size_t Length
	)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(Length);

	ULONG queueLength;
	KLOCK_QUEUE_HANDLE lockHandle;

	KeAcquireInStackQueuedSpinLock(
		&gPacketQueue.queueLock,
		&lockHandle
		);

	queueLength = gPacketQueue.queueLength;

	KeReleaseInStackQueuedSpinLock(
		&lockHandle
		);

	WdfRequestMarkCancelable(Request, kkdrvRequestCancel);

	if (gPacketQueue.queueLength > 0)
	{
		CompleteRequest(Request);
	}
	else
	{
		gPendingRequest = Request;
	}
}

VOID kkdrvRequestCancel(
	_In_  WDFREQUEST Request
	)
{
	gPendingRequest = NULL;
	ClearPacketQueue(&gPacketQueue);
	WdfRequestCompleteWithInformation(Request, STATUS_CANCELLED, 0);
}

VOID
CompleteRequest(
	_In_ WDFREQUEST Request
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	BYTE *data = NULL;
	size_t bytesToWrite = 0;
	size_t bytesWritten = 0;
	ULONG packetsToRead = 0;
	size_t packetsToReadSize = 0;
	KLOCK_QUEUE_HANDLE lockHandle;
	PKKDRV_PACKET packet = NULL;
	PLIST_ENTRY packets[KKDRV_MAX_READ_PACKET_COUNT];

	status = WdfRequestUnmarkCancelable(Request);

	status = WdfRequestRetrieveOutputBuffer(
		Request,
		1,
		&data,
		&bytesToWrite
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfRequestRetrieveOutputBuffer, status);
		goto Exit;
	}

	KeAcquireInStackQueuedSpinLock(
		&gPacketQueue.queueLock,
		&lockHandle
		);

	/*
		Nast�puj�cy blok ��czy kolejne pakiety w kolejce, aby sprawniej przekazywa� dane do klienta. Blok jest zamykany w stuacji, gdy:
		1. nie ma wi�cej pakiet�w w buforze
		2. rozmiar bloku przekroczy� limit KKDRV_MAX_READ_PACKET_COUNT
		3. rozmiar bloku przekroczy� limit rozmiaru bufora wyj�ciowego
		4. kolejny napotkany pakiet jest adresowany do innego hosta, ni� poprzednie
	*/

	if (gPacketQueue.queueLength > 0)
	{
		PLIST_ENTRY entry = RemoveHeadList(&gPacketQueue.queue);
		PKKDRV_PACKET packet = CONTAINING_RECORD(entry, KKDRV_PACKET, entry);
		size_t packetSize = packet->dataLength;
		CHAR *currentDestinationAddr = (CHAR*)packet + PACKET_IPV4_DESTINATION_OFFSET;
		UINT currentDestination = *((UINT*)currentDestinationAddr);
		while ((packetsToReadSize + packetSize <= bytesToWrite)
			&& (packetsToRead < KKDRV_MAX_READ_PACKET_COUNT)
			&& (gPacketQueue.queueLength > 0))
		{
			packets[packetsToRead] = entry;

			gPacketQueue.queueLength--;
			packetsToReadSize += packetSize;
			packetsToRead++;

			entry = RemoveHeadList(&gPacketQueue.queue);
			packet = CONTAINING_RECORD(entry, KKDRV_PACKET, entry);
			packetSize = packet->dataLength;

			currentDestinationAddr = (CHAR*)packet + PACKET_IPV4_DESTINATION_OFFSET;
			if (*((UINT*)currentDestinationAddr) != currentDestination)
			{
				break;
			}
			currentDestination = *((UINT*)currentDestinationAddr);
		}

		if (packetsToRead != KKDRV_MAX_READ_PACKET_COUNT && gPacketQueue.queueLength > 0)
		{
			InsertHeadList(&gPacketQueue.queue, entry);
		}
	}

	KeReleaseInStackQueuedSpinLock(
		&lockHandle
		);

	if (packetsToRead > 0)
	{
		for (ULONG i = 0; i < packetsToRead; i++)
		{
			packet = CONTAINING_RECORD(packets[i], KKDRV_PACKET, entry);
			RtlCopyBytes(&data[bytesWritten], &packet->data, packet->dataLength);
			bytesWritten += packet->dataLength;

			ExFreePoolWithTag(packets[i], KKDRV_TAG);
		}
	}

Exit:
	WdfRequestCompleteWithInformation(Request, status, bytesWritten);
}
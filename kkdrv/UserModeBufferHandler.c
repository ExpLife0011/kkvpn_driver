#include "UserModeBufferHandler.h"
#include "DriverInit.h"

VOID 
WorkerRoutine(
	_In_ PVOID params
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	KKDRV_WORKER_DATA* threadParams = (KKDRV_WORKER_DATA*)params;

	for (;;)
	{
		KLOCK_QUEUE_HANDLE lockHandle;
		PKKDRV_PACKET packet = NULL;
		size_t length = 0;
		KKDRV_NET_BUFFER_FLAT *nbflat = NULL;

		KeWaitForSingleObject(
			(PKEVENT)&threadParams->event,
			Executive,
			KernelMode,
			FALSE,
			NULL
			);

		if (*(BOOLEAN*)(threadParams->stoppingThread))
		{
			break;
		}

		nbflat = (KKDRV_NET_BUFFER_FLAT*)threadParams->mem;

		threadParams->queue->awake = TRUE;

		KeAcquireInStackQueuedSpinLock(&(threadParams->queue->lock), &lockHandle);

		packet = threadParams->queue->nblHead;
		length = threadParams->queue->length;

		threadParams->queue->nblHead = NULL;
		threadParams->queue->nblTail = NULL;
		threadParams->queue->length = 0;

		KeReleaseInStackQueuedSpinLock(&lockHandle);

		KeClearEvent(&threadParams->event);

		if (!packet)
		{
			continue;
		}

		KeWaitForSingleObject(
			(PKEVENT)threadParams->userevent_complete,
			Executive,
			KernelMode,
			FALSE,
			NULL
			);

		nbflat->length = (UINT32)length;
		status = CopyPacketDataToBuffer(
			packet,
			length,
			&(nbflat->buffer)
			);
		if (!NT_SUCCESS(status))
		{
			REPORT_ERROR(CopyPacketDataToBuffer, status);
			continue;
		}

		KeSetEvent(threadParams->userevent_receive, IO_NO_INCREMENT, FALSE);

		threadParams->queue->awake = FALSE;
	}

	PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
CopyPacketDataToBuffer(
	_Inout_ PKKDRV_PACKET head,
	_In_ size_t length,
	_Out_ PVOID buffer
	)
{
	NTSTATUS status = STATUS_SUCCESS;

	if (length > (UM_BUFFER_PAGE_SIZE))
	{
		REPORT_ERROR(CopyNblDataToBuffer, 0);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	size_t bytesCopied = 0;
	PKKDRV_PACKET packet = head;

	while (packet)
	{
		RtlCopyMemory(
			(PVOID)((size_t)buffer + bytesCopied),
			&(packet->data),
			packet->dataLength
			);

		bytesCopied += packet->dataLength;
		
		PKKDRV_PACKET temp = packet;
		packet = packet->Next;

		ExFreePoolWithTag(temp, KKDRV_TAG);
	}
	
	return status;
}
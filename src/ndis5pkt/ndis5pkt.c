#include <ntddk.h>
#include <ndis.h>
#include <stdio.h>
#define NT_DEVICE_NAME	L"\\Device\\ndis5pkt"
#define DOS_DEVICE_NAME L"\\DosDevices\\ndis5pkt"
#include "ndis5pkt.h"
#include "packet.h"
NDISUIO_GLOBALS         Globals = {0};
NTSTATUS DriverEntry(IN PDRIVER_OBJECT pDriverObject,IN PUNICODE_STRING pRegistryPath)
{
    NDIS_PROTOCOL_CHARACTERISTICS   protocolChar;
    NTSTATUS                        status = STATUS_SUCCESS;
    NDIS_STRING                     protoName = NDIS_STRING_CONST("NDIS5PKT");     
    UNICODE_STRING                  ntDeviceName;
    UNICODE_STRING                  win32DeviceName;
    BOOLEAN                         fSymbolicLink = FALSE;
    PDEVICE_OBJECT                  deviceObject;
    DEBUGP(DL_LOUD, ("\n\nDriverEntry\n"));
    Globals.pDriverObject = pDriverObject;
    NUIO_INIT_EVENT(&Globals.BindsComplete);
    do
    {
        RtlInitUnicodeString(&ntDeviceName, NT_DEVICE_NAME);
        status = IoCreateDevice(
			pDriverObject, 
			0, 
			&ntDeviceName,
			FILE_DEVICE_NDIS5PKT,
			FILE_DEVICE_SECURE_OPEN,
			FALSE,
			&deviceObject);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        RtlInitUnicodeString(&win32DeviceName, DOS_DEVICE_NAME);
        status = IoCreateSymbolicLink(&win32DeviceName, &ntDeviceName);
        if (!NT_SUCCESS(status))
            break;
        fSymbolicLink = TRUE;
        deviceObject->Flags |= DO_DIRECT_IO;
        Globals.ControlDeviceObject = deviceObject;
        NUIO_INIT_LIST_HEAD(&Globals.OpenList);
        NUIO_INIT_LOCK(&Globals.GlobalLock);
        NdisZeroMemory(&protocolChar, sizeof(NDIS_PROTOCOL_CHARACTERISTICS));
        protocolChar.MajorNdisVersion            = 5;
        protocolChar.MinorNdisVersion            = 0;
        protocolChar.Name                        = protoName;
        protocolChar.OpenAdapterCompleteHandler  = NdisuioOpenAdapterComplete;
        protocolChar.CloseAdapterCompleteHandler = NdisuioCloseAdapterComplete;
        protocolChar.SendCompleteHandler         = NdisuioSendComplete;
        protocolChar.TransferDataCompleteHandler = NdisuioTransferDataComplete;
        protocolChar.ResetCompleteHandler        = NdisuioResetComplete;
        protocolChar.RequestCompleteHandler      = NdisuioRequestComplete;
        protocolChar.ReceiveHandler              = NdisuioReceive;
        protocolChar.ReceiveCompleteHandler      = NdisuioReceiveComplete;
        protocolChar.StatusHandler               = NdisuioStatus;
        protocolChar.StatusCompleteHandler       = NdisuioStatusComplete;
        protocolChar.BindAdapterHandler          = NdisuioBindAdapter;
        protocolChar.UnbindAdapterHandler        = NdisuioUnbindAdapter;
        protocolChar.UnloadHandler               = NULL;
        protocolChar.ReceivePacketHandler        = NdisuioReceivePacket;
        protocolChar.PnPEventHandler             = NdisuioPnPEventHandler;
        NdisRegisterProtocol(
            &status,
            &Globals.NdisProtocolHandle,
            &protocolChar,
            sizeof(NDIS_PROTOCOL_CHARACTERISTICS));
        if (status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("Failed to register protocol with NDIS: 0x%X\n", status));
            status = STATUS_UNSUCCESSFUL;
            break;
        }

#ifdef NDIS51
        Globals.PartialCancelId = NdisGeneratePartialCancelId();
        Globals.PartialCancelId <<= ((sizeof(PVOID) - 1) * 8);
        DEBUGP(DL_LOUD, ("DriverEntry: CancelId %lx\n", Globals.PartialCancelId));
#endif
        pDriverObject->MajorFunction[IRP_MJ_CREATE]	= NdisuioOpen;
        pDriverObject->MajorFunction[IRP_MJ_CLOSE]	= NdisuioClose;
        pDriverObject->MajorFunction[IRP_MJ_READ]	= NdisuioRead;
        pDriverObject->MajorFunction[IRP_MJ_WRITE]	= NdisuioWrite;
        pDriverObject->MajorFunction[IRP_MJ_CLEANUP] = NdisuioCleanup;
        pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NdisuioIoControl;
        pDriverObject->DriverUnload = NdisuioUnload;
        status = STATUS_SUCCESS;
    }while (FALSE);
    if (!NT_SUCCESS(status))
    {
        if (deviceObject)
        {
            IoDeleteDevice(deviceObject);
            Globals.ControlDeviceObject = NULL;
        }
        if (fSymbolicLink)
        {
            IoDeleteSymbolicLink(&win32DeviceName);
        }
    }
    return status;
}
VOID NdisuioUnload(IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING     win32DeviceName;
    DEBUGP(DL_LOUD, ("Unload Enter\n"));
    RtlInitUnicodeString(&win32DeviceName, DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&win32DeviceName);           
    if (Globals.ControlDeviceObject)
    {
        IoDeleteDevice(Globals.ControlDeviceObject);
        Globals.ControlDeviceObject = NULL;
    }
    ndisuioDoProtocolUnload();
#if DBG
    ndisuioAuditShutdown();
#endif
    DEBUGP(DL_LOUD, ("Unload Exit\n"));
}
NTSTATUS NdisuioIoControl(IN PDEVICE_OBJECT pDeviceObject,IN PIRP pIrp)
{
    PIO_STACK_LOCATION      pIrpSp;
    ULONG                   FunctionCode;
    NTSTATUS                NtStatus;
    NDIS_STATUS             Status;
    PNDISUIO_OPEN_CONTEXT   pOpenContext;
    ULONG                   BytesReturned;
    pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    FunctionCode = pIrpSp->Parameters.DeviceIoControl.IoControlCode;
    pOpenContext = (PNDISUIO_OPEN_CONTEXT)pIrpSp->FileObject->FsContext;
    BytesReturned = 0;
    DEBUGP(DL_LOUD, ("IoControl: DevObj=%p, Func=0x%X, Irp=%p\n",
		pDeviceObject, FunctionCode, pIrp));
    switch (FunctionCode)
    {
	case IOCTL_NDIS5PKT_READ:
		return NdisuioRead(pDeviceObject, pIrp);
	case IOCTL_NDIS5PKT_BIND_WAIT:
		if (NUIO_WAIT_EVENT(&Globals.BindsComplete, 5000))
		{
			NtStatus = STATUS_SUCCESS;
		}
		else
		{
			NtStatus = STATUS_TIMEOUT;
		}
		DEBUGP(DL_INFO, ("IoControl: BindWait returning 0x%X\n", NtStatus));
		break;
	case IOCTL_NDIS5PKT_BIND_ADAPTER:
		if (pOpenContext != NULL)
		{
			NUIO_STRUCT_ASSERT(pOpenContext, oc);
			DEBUGP(DL_WARN, ("IoControl: OPEN_DEVICE: FileObj %p already"
				" associated with open %p\n", pIrpSp->FileObject, pOpenContext));
			NtStatus = STATUS_DEVICE_BUSY;
			break;
		}
		{
			ANSI_STRING AdapterNameA;
			NDIS_STRING AdapterNameW;
			RtlInitAnsiString(&AdapterNameA, (PCSZ)pIrp->AssociatedIrp.SystemBuffer);
			NtStatus = RtlAnsiStringToUnicodeString(&AdapterNameW, &AdapterNameA, TRUE);
			if (NtStatus != STATUS_SUCCESS)
				break;
			NdisuioBindAdapter(&Status, 0, &AdapterNameW, 0, 0);
			if (Status == NDIS_STATUS_SUCCESS)
			{
				NtStatus = ndisuioOpenDevice(
					(PUCHAR)AdapterNameW.Buffer,
					AdapterNameW.Length,
					pIrpSp->FileObject,
					&pOpenContext
					);
				if (NT_SUCCESS(NtStatus))
				{
					pIrpSp->FileObject->FsContext = pOpenContext;
					DEBUGP(DL_VERY_LOUD, ("OPEN_DEVICE: Open %p <-> FileObject %p\n",
						pOpenContext, pIrpSp->FileObject));
				}
				NDIS_STATUS_TO_NT_STATUS(Status, &NtStatus);
			}
			RtlFreeUnicodeString(&AdapterNameW);
		}
		break;
	case IOCTL_NDIS5PKT_UNBIND_ADAPTER:
		if (pOpenContext != NULL)
		{
			NdisuioUnbindAdapter(&Status, pOpenContext, 0);
			NDIS_STATUS_TO_NT_STATUS(Status, &NtStatus);
		}
		else
		{
			NtStatus = STATUS_DEVICE_NOT_CONNECTED;
		}
		break;
	case IOCTL_NDIS5PKT_OPEN_DEVICE:
		if (pOpenContext != NULL)
		{
			NUIO_STRUCT_ASSERT(pOpenContext, oc);
			DEBUGP(DL_WARN, ("IoControl: OPEN_DEVICE: FileObj %p already"
				" associated with open %p\n", pIrpSp->FileObject, pOpenContext));
			NtStatus = STATUS_DEVICE_BUSY;
			break;
		}
		else
		{
			ANSI_STRING AdapterNameA;
			NDIS_STRING AdapterNameW;
			RtlInitAnsiString(&AdapterNameA, (PCSZ)pIrp->AssociatedIrp.SystemBuffer);
			NtStatus = RtlAnsiStringToUnicodeString(&AdapterNameW, &AdapterNameA, TRUE);
			if (NtStatus == STATUS_SUCCESS)
			{
				NtStatus = ndisuioOpenDevice(
					(PUCHAR)AdapterNameW.Buffer,
					AdapterNameW.Length,
					pIrpSp->FileObject,
					&pOpenContext
					);
				RtlFreeUnicodeString(&AdapterNameW);
				if (NT_SUCCESS(NtStatus))
				{
					pIrpSp->FileObject->FsContext = pOpenContext;
					DEBUGP(DL_VERY_LOUD, ("OPEN_DEVICE: Open %p <-> FileObject %p\n",
						pOpenContext, pIrpSp->FileObject));
				}
			}
		}
		break;
	case IOCTL_NDIS5PKT_QUERY_OID_VALUE:
		if (pOpenContext != NULL)
		{
			Status = ndisuioQueryOidValue(
				pOpenContext,
				pIrp->AssociatedIrp.SystemBuffer,
				pIrpSp->Parameters.DeviceIoControl.OutputBufferLength,
				&BytesReturned
				);
			NDIS_STATUS_TO_NT_STATUS(Status, &NtStatus);
		}
		else
		{
			NtStatus = STATUS_DEVICE_NOT_CONNECTED;
		}
		break;
	case IOCTL_NDIS5PKT_SET_OID_VALUE:
		if (pOpenContext != NULL)
		{
			Status = ndisuioSetOidValue(
				pOpenContext,
				pIrp->AssociatedIrp.SystemBuffer,
				pIrpSp->Parameters.DeviceIoControl.InputBufferLength
				);
			NDIS_STATUS_TO_NT_STATUS(Status, &NtStatus);
		}
		else
		{
			NtStatus = STATUS_DEVICE_NOT_CONNECTED;
		}
		break;
	case IOCTL_NDIS5PKT_QUERY_STATISTICS:
		if (pOpenContext != NULL)
		{
			PSTATISTICS_DATA Data = pIrp->AssociatedIrp.SystemBuffer;
			ULONG Size = pIrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			if (Size >= sizeof(STATISTICS_DATA))
			{
				Data->DroppedPackets = pOpenContext->DroppedPackets;
				Data->ReceivedPackets = pOpenContext->ReceivedPackets;
				BytesReturned = sizeof(STATISTICS_DATA);
				NtStatus = STATUS_SUCCESS;
			}
			else
			{
				NtStatus = STATUS_BUFFER_TOO_SMALL;
			}
		}
		else
		{
			NtStatus = STATUS_DEVICE_NOT_CONNECTED;
		}
		break;
	default:
		NtStatus = STATUS_NOT_SUPPORTED;
		break;
    }
    if (NtStatus != STATUS_PENDING)
    {
        pIrp->IoStatus.Information = BytesReturned;
        pIrp->IoStatus.Status = NtStatus;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }
    return NtStatus;
}
NTSTATUS ndisuioOpenDevice(
    IN PUCHAR                   pDeviceName,
    IN ULONG                    DeviceNameLength,
    IN PFILE_OBJECT             pFileObject,
    OUT PNDISUIO_OPEN_CONTEXT * ppOpenContext
    )
{
    PNDISUIO_OPEN_CONTEXT   pOpenContext;
    NTSTATUS                NtStatus;
    pOpenContext = NULL;
    do
    {
        pOpenContext = ndisuioLookupDevice(pDeviceName, DeviceNameLength);
        if (pOpenContext == NULL)
        {
            DEBUGP(DL_WARN, ("OpenDevice: couldn't find device\n"));
            NtStatus = STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);
        if (!NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_OPEN_FLAGS, NUIOO_OPEN_IDLE))
        {
            NUIO_ASSERT(pOpenContext->pFileObject != NULL);
            DEBUGP(DL_WARN, ("OpenDevice: Open %p/%x already associated"
                " with another FileObject %p\n", 
                pOpenContext, pOpenContext->Flags, pOpenContext->pFileObject));
            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
            NUIO_DEREF_OPEN(pOpenContext); // ndisuioOpenDevice failure
            NtStatus = STATUS_DEVICE_BUSY;
            break;
        }
        pOpenContext->pFileObject = pFileObject;
        NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_OPEN_FLAGS, NUIOO_OPEN_ACTIVE);
        NUIO_RELEASE_LOCK(&pOpenContext->Lock);
        *ppOpenContext = pOpenContext;
        NtStatus = STATUS_SUCCESS;
    } while (FALSE);
    return (NtStatus);
}
VOID NdisuioBindAdapter(
    OUT PNDIS_STATUS                pStatus,
    IN NDIS_HANDLE                  BindContext,
    IN PNDIS_STRING                 pDeviceName,
    IN PVOID                        SystemSpecific1,
    IN PVOID                        SystemSpecific2
    )
{
    PNDISUIO_OPEN_CONTEXT           pOpenContext;
    NDIS_STATUS						Status;
    do
    {
        NUIO_ALLOC_MEM(pOpenContext, sizeof(NDISUIO_OPEN_CONTEXT));
        if (pOpenContext == NULL)
        {
            Status = NDIS_STATUS_RESOURCES;
            break;
        }
        NUIO_ZERO_MEM(pOpenContext, sizeof(NDISUIO_OPEN_CONTEXT));
        NUIO_SET_SIGNATURE(pOpenContext, oc);
        NUIO_INIT_LOCK(&pOpenContext->Lock);
        NUIO_INIT_LIST_HEAD(&pOpenContext->PendedReads);
        NUIO_INIT_LIST_HEAD(&pOpenContext->PendedWrites);
        NUIO_INIT_LIST_HEAD(&pOpenContext->RecvPktQueue);
        NUIO_INIT_EVENT(&pOpenContext->PoweredUpEvent);
        NUIO_SIGNAL_EVENT(&pOpenContext->PoweredUpEvent);
		pOpenContext->bRunningOnWin9x = FALSE;
        NUIO_REF_OPEN(pOpenContext); // Bind
        NUIO_ACQUIRE_LOCK(&Globals.GlobalLock);
        NUIO_INSERT_TAIL_LIST(&Globals.OpenList, &pOpenContext->Link);
        NUIO_RELEASE_LOCK(&Globals.GlobalLock);
        Status = ndisuioCreateBinding(
			pOpenContext,
			(PUCHAR)pDeviceName->Buffer,
			pDeviceName->Length);
    } while (FALSE);
    *pStatus = Status;
    return;
}
VOID NdisuioOpenAdapterComplete(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN NDIS_STATUS                  Status,
    IN NDIS_STATUS                  OpenErrorCode
    )
{
    PNDISUIO_OPEN_CONTEXT           pOpenContext;
    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    NUIO_STRUCT_ASSERT(pOpenContext, oc);
    pOpenContext->BindStatus = Status;
    NUIO_SIGNAL_EVENT(&pOpenContext->BindEvent);
}
VOID NdisuioUnbindAdapter(
    OUT PNDIS_STATUS                pStatus,
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN NDIS_HANDLE                  UnbindContext
    )
{
    PNDISUIO_OPEN_CONTEXT           pOpenContext;
    DEBUGP(DL_LOUD, ("Unbind Enter\n"));
    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    NUIO_STRUCT_ASSERT(pOpenContext, oc);
    NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);
	if (NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_IDLE) ||
		NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_UNBIND_FLAGS, NUIOO_UNBIND_RECEIVED))
	{
		NUIO_RELEASE_LOCK(&pOpenContext->Lock);
		*pStatus = NDIS_STATUS_FAILURE;
		return;
	}
    NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_UNBIND_FLAGS, NUIOO_UNBIND_RECEIVED);
    NUIO_SIGNAL_EVENT(&pOpenContext->PoweredUpEvent);
    NUIO_RELEASE_LOCK(&pOpenContext->Lock);
    ndisuioShutdownBinding(pOpenContext);
    *pStatus = NDIS_STATUS_SUCCESS;
	DEBUGP(DL_LOUD, ("Unbind Exit\n"));
}
VOID NdisuioCloseAdapterComplete(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN NDIS_STATUS                  Status
    )
{
    PNDISUIO_OPEN_CONTEXT           pOpenContext;
    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    NUIO_STRUCT_ASSERT(pOpenContext, oc);
    pOpenContext->BindStatus = Status;
    NUIO_SIGNAL_EVENT(&pOpenContext->BindEvent);
}  
NDIS_STATUS NdisuioPnPEventHandler(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN PNET_PNP_EVENT               pNetPnPEvent
    )
{
    PNDISUIO_OPEN_CONTEXT           pOpenContext;
    NDIS_STATUS                     Status;
    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    switch (pNetPnPEvent->NetEvent)
    {
        case NetEventSetPower:
            NUIO_STRUCT_ASSERT(pOpenContext, oc);
            pOpenContext->PowerState = *(PNET_DEVICE_POWER_STATE)pNetPnPEvent->Buffer;
            if (pOpenContext->PowerState > NdisDeviceStateD0) 
            {
                NUIO_INIT_EVENT(&pOpenContext->PoweredUpEvent);
                ndisuioWaitForPendingIO(pOpenContext, FALSE);
                ndisuioFlushReceiveQueue(pOpenContext);
                DEBUGP(DL_INFO, ("PnPEvent: Open %p, SetPower to %d\n",
                    pOpenContext, pOpenContext->PowerState));
            }
            else
            {
                DEBUGP(DL_INFO, ("PnPEvent: Open %p, SetPower ON: %d\n",
                    pOpenContext, pOpenContext->PowerState));
                NUIO_SIGNAL_EVENT(&pOpenContext->PoweredUpEvent);
            }
            Status = NDIS_STATUS_SUCCESS;
            break;
        case NetEventQueryPower:
            Status = NDIS_STATUS_SUCCESS;
            break;
        case NetEventBindsComplete:
            NUIO_SIGNAL_EVENT(&Globals.BindsComplete);
            Status = NDIS_STATUS_SUCCESS;
            break;
        case NetEventQueryRemoveDevice:
        case NetEventCancelRemoveDevice:
        case NetEventReconfigure:
        case NetEventBindList:
        case NetEventPnPCapabilities:
            Status = NDIS_STATUS_SUCCESS;
            break;
        default:
            Status = NDIS_STATUS_NOT_SUPPORTED;
            break;
    }
    DEBUGP(DL_INFO, ("PnPEvent: Open %p, Event %d, Status 0x%X\n",
            pOpenContext, pNetPnPEvent->NetEvent, Status));
    return (Status);
}
NDIS_STATUS ndisuioCreateBinding(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext,
    IN PUCHAR                       pBindingInfo,
    IN ULONG                        BindingInfoLength
    )
{
    NDIS_STATUS             Status;
    NDIS_STATUS             OpenErrorCode;
    NDIS_MEDIUM             MediumArray[1] = {NdisMedium802_3};
    UINT                    SelectedMediumIndex;
    PNDISUIO_OPEN_CONTEXT   pTmpOpenContext;
    BOOLEAN                 fDoNotDisturb = FALSE;
    BOOLEAN                 fOpenComplete = FALSE;
    ULONG                   BytesProcessed;
    ULONG                   GenericUlong = 0;
    DEBUGP(DL_LOUD, ("Bind %p/%x, device=%ws\n", pOpenContext, pOpenContext->Flags, pBindingInfo));
    Status = NDIS_STATUS_SUCCESS;
    do
    {
        pTmpOpenContext = ndisuioLookupDevice(pBindingInfo, BindingInfoLength);
        if (pTmpOpenContext != NULL)
        {
            DEBUGP(DL_WARN, ("Binding to device %ws already exists on open %p\n",
				pTmpOpenContext->DeviceName.Buffer, pTmpOpenContext));
            NUIO_DEREF_OPEN(pTmpOpenContext);  // temp ref added by Lookup
            Status = NDIS_STATUS_FAILURE;
            break;
        }
        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);
        if (!NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_IDLE) ||
            NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_UNBIND_FLAGS, NUIOO_UNBIND_RECEIVED))
        {
            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
            Status = NDIS_STATUS_NOT_ACCEPTED;
            fDoNotDisturb = TRUE;
            break;
        }
        NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_OPENING);
        NUIO_RELEASE_LOCK(&pOpenContext->Lock);
        NUIO_ALLOC_MEM(pOpenContext->DeviceName.Buffer, BindingInfoLength + sizeof(WCHAR));
        if (pOpenContext->DeviceName.Buffer == NULL)
        {
            DEBUGP(DL_WARN, ("failed to alloc device name buf (%d bytes)\n",
                BindingInfoLength + sizeof(WCHAR)));
            Status = NDIS_STATUS_RESOURCES;
            break;
        }
        NUIO_COPY_MEM(pOpenContext->DeviceName.Buffer, pBindingInfo, BindingInfoLength);
        *(PWCHAR)((PUCHAR)pOpenContext->DeviceName.Buffer + BindingInfoLength) = L'\0';
        NdisInitUnicodeString(&pOpenContext->DeviceName, pOpenContext->DeviceName.Buffer);
        NdisAllocatePacketPoolEx(
            &Status,
            &pOpenContext->SendPacketPool,
            MIN_SEND_PACKET_POOL_SIZE,
            MAX_SEND_PACKET_POOL_SIZE - MIN_SEND_PACKET_POOL_SIZE,
            sizeof(NUIO_SEND_PACKET_RSVD));
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("failed to alloc send packet pool: 0x%X\n", Status));
            break;
        }
        NdisSetPacketPoolProtocolId(pOpenContext->SendPacketPool, 0x4);
        NdisAllocatePacketPoolEx(
            &Status,
            &pOpenContext->RecvPacketPool,
            MIN_RECV_PACKET_POOL_SIZE,
            MAX_RECV_PACKET_POOL_SIZE - MIN_RECV_PACKET_POOL_SIZE,
            sizeof(NUIO_RECV_PACKET_RSVD));
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("failed to alloc recv packet pool: 0x%X\n", Status));
            break;
        }
        NdisAllocateBufferPool(
            &Status,
            &pOpenContext->RecvBufferPool,
            MAX_RECV_PACKET_POOL_SIZE);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("failed to alloc recv buffer pool: 0x%X\n", Status));
            break;
        }
        if (pOpenContext->bRunningOnWin9x)
        {
            NdisAllocateBufferPool(
                &Status,
                &pOpenContext->SendBufferPool,
                MAX_SEND_PACKET_POOL_SIZE);
            if (Status != NDIS_STATUS_SUCCESS)
            {
                DEBUGP(DL_WARN, ("failed to alloc send buffer pool: 0x%X\n", Status));
                break;
            }
        }

        NUIO_INIT_EVENT(&pOpenContext->BindEvent);
        NdisOpenAdapter(
            &Status,
            &OpenErrorCode,
            &pOpenContext->BindingHandle,
            &SelectedMediumIndex,
            &MediumArray[0],
            sizeof(MediumArray) / sizeof(NDIS_MEDIUM),
            Globals.NdisProtocolHandle,
            (NDIS_HANDLE)pOpenContext,
            &pOpenContext->DeviceName,
            0,
            NULL);
        if (Status == NDIS_STATUS_PENDING)
        {
            NUIO_WAIT_EVENT(&pOpenContext->BindEvent, 0);
            Status = pOpenContext->BindStatus;
        }
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("NdisOpenAdapter (%ws) failed: 0x%X\n",
                pOpenContext->DeviceName.Buffer, Status));
            break;
        }
        fOpenComplete = TRUE;
        NdisQueryAdapterInstanceName(&pOpenContext->DeviceDescr, pOpenContext->BindingHandle);
        Status = ndisuioDoRequest(
			pOpenContext,
			NdisRequestQueryInformation,
			OID_802_3_CURRENT_ADDRESS,
			pOpenContext->CurrentAddress,
			NUIO_MAC_ADDR_LEN,
			&BytesProcessed
			);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("query current address failed: 0x%X\n", Status));
            break;
        }
        Status = ndisuioDoRequest(
			pOpenContext,
			NdisRequestQueryInformation,
			OID_GEN_MAC_OPTIONS,
			&pOpenContext->MacOptions,
			sizeof(pOpenContext->MacOptions),
			&BytesProcessed
			);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("query MAC options failed: 0x%X\n", Status));
            break;
        }
        Status = ndisuioDoRequest(
			pOpenContext,
			NdisRequestQueryInformation,
			OID_GEN_MAXIMUM_FRAME_SIZE,
			&pOpenContext->MaxFrameSize,
			sizeof(pOpenContext->MaxFrameSize),
			&BytesProcessed
			);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("query max frame failed: 0x%X\n", Status));
            break;
        }
        Status = ndisuioDoRequest(
			pOpenContext,
			NdisRequestQueryInformation,
			OID_GEN_MEDIA_CONNECT_STATUS,
			&GenericUlong,
			sizeof(GenericUlong),
			&BytesProcessed
			);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DEBUGP(DL_WARN, ("query media connect status failed: 0x%X\n", Status));
            break;
        }
        if (GenericUlong == NdisMediaStateConnected)
        {
            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_MEDIA_FLAGS, NUIOO_MEDIA_CONNECTED);
        }
        else
        {
            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_MEDIA_FLAGS, NUIOO_MEDIA_DISCONNECTED);
        }
        pOpenContext->PowerState = NdisDeviceStateD0;
        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);
        NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_ACTIVE);
        if (NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_UNBIND_FLAGS, NUIOO_UNBIND_RECEIVED))
        {
            Status = NDIS_STATUS_FAILURE;
        }
        NUIO_RELEASE_LOCK(&pOpenContext->Lock);
    } while (FALSE);
    if ((Status != NDIS_STATUS_SUCCESS) && !fDoNotDisturb)
    {
        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);
        if (fOpenComplete)
        {
            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_ACTIVE);
        }
        else if (NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_OPENING))
        {
            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_FAILED);
        }
        NUIO_RELEASE_LOCK(&pOpenContext->Lock);
        ndisuioShutdownBinding(pOpenContext);
    }
    DEBUGP(DL_INFO, ("Bind: OpenContext %p, Status 0x%X\n", pOpenContext, Status));
    return Status;
}
VOID ndisuioShutdownBinding(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext
    )
/*++

Routine Description:

    Utility function to shut down the NDIS binding, if one exists, on
    the specified open. This is written to be called from:

        ndisuioCreateBinding - on failure
        NdisuioUnbindAdapter

    We handle the case where a binding is in the process of being set up.
    This precaution is not needed if this routine is only called from
    the context of our UnbindAdapter handler, but they are here in case
    we initiate unbinding from elsewhere (e.g. on processing a user command).

    NOTE: this blocks and finishes synchronously.

Arguments:

    pOpenContext - pointer to open context block

Return Value:

    None

--*/
{
    NDIS_STATUS             Status;
    BOOLEAN                 DoCloseBinding = FALSE;

    do
    {
        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

        if (NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_OPENING))
        {
            //
            //  We are still in the process of setting up this binding.
            //
            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
            break;
        }

        if (NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_ACTIVE))
        {
            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_CLOSING);
            DoCloseBinding = TRUE;
        }

        NUIO_RELEASE_LOCK(&pOpenContext->Lock);

        if (DoCloseBinding)
        {
            //
            //  Wait for any pending sends or requests on
            //  the binding to complete.
            //
            ndisuioWaitForPendingIO(pOpenContext, TRUE);

            //
            //  Discard any queued receives.
            //
            ndisuioFlushReceiveQueue(pOpenContext);

            //
            //  Close the binding now.
            //
            NUIO_INIT_EVENT(&pOpenContext->BindEvent);

            DEBUGP(DL_INFO, ("Closing OpenContext %p, BindingHandle %p\n",
                    pOpenContext, pOpenContext->BindingHandle));

            NdisCloseAdapter(&Status, pOpenContext->BindingHandle);

            if (Status == NDIS_STATUS_PENDING)
            {
                NUIO_WAIT_EVENT(&pOpenContext->BindEvent, 0);
                Status = pOpenContext->BindStatus;
            }

            NUIO_ASSERT(Status == NDIS_STATUS_SUCCESS);

            pOpenContext->BindingHandle = NULL;

            NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_IDLE);

            NUIO_SET_FLAGS(pOpenContext->Flags, NUIOO_UNBIND_FLAGS, 0);

            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
        }

        //
        //  Remove it from the global list.
        //
        NUIO_ACQUIRE_LOCK(&Globals.GlobalLock);

        NUIO_REMOVE_ENTRY_LIST(&pOpenContext->Link);

        NUIO_RELEASE_LOCK(&Globals.GlobalLock);

        //
        //  Free any other resources allocated for this bind.
        //
        ndisuioFreeBindResources(pOpenContext);

        NUIO_DEREF_OPEN(pOpenContext);  // Shutdown binding
    } while (FALSE);
}

VOID
ndisuioFreeBindResources(
    IN PNDISUIO_OPEN_CONTEXT       pOpenContext
    )
/*++

Routine Description:

    Free any resources set up for an NDIS binding.

Arguments:

    pOpenContext - pointer to open context block

Return Value:

    None

--*/
{
    if (pOpenContext->SendPacketPool != NULL)
    {
        NdisFreePacketPool(pOpenContext->SendPacketPool);
        pOpenContext->SendPacketPool = NULL;
    }

    if (pOpenContext->RecvPacketPool != NULL)
    {
        NdisFreePacketPool(pOpenContext->RecvPacketPool);
        pOpenContext->RecvPacketPool = NULL;
    }

    if (pOpenContext->RecvBufferPool != NULL)
    {
        NdisFreeBufferPool(pOpenContext->RecvBufferPool);
        pOpenContext->RecvBufferPool = NULL;
    }

    if (pOpenContext->SendBufferPool != NULL)
    {
        NdisFreeBufferPool(pOpenContext->SendBufferPool);
        pOpenContext->SendBufferPool = NULL;
    }

    if (pOpenContext->DeviceName.Buffer != NULL)
    {
        NUIO_FREE_MEM(pOpenContext->DeviceName.Buffer);
        pOpenContext->DeviceName.Buffer = NULL;
        pOpenContext->DeviceName.Length =
        pOpenContext->DeviceName.MaximumLength = 0;
    }

    if (pOpenContext->DeviceDescr.Buffer != NULL)
    {
        //
        // this would have been allocated by NdisQueryAdpaterInstanceName.
        //
        NdisFreeMemory(pOpenContext->DeviceDescr.Buffer, 0, 0);
        pOpenContext->DeviceDescr.Buffer = NULL;
    }
}

VOID
ndisuioWaitForPendingIO(
    IN PNDISUIO_OPEN_CONTEXT            pOpenContext,
    IN BOOLEAN                          DoCancelReads
    )
/*++

Routine Description:

    Utility function to wait for all outstanding I/O to complete
    on an open context. It is assumed that the open context
    won't go away while we are in this routine.

Arguments:

    pOpenContext - pointer to open context structure
    DoCancelReads - do we wait for pending reads to go away (and cancel them)?

Return Value:

    None

--*/
{
    ULONG           LoopCount;
	ULONG           PendingCount;
	NDIS_STATUS     Status;

#ifdef NDIS51
    //
    //  Wait for any pending sends or requests on the binding to complete.
    //
    for (LoopCount = 0; LoopCount < 60; LoopCount++)
    {
        Status = NdisQueryPendingIOCount(
                    pOpenContext->BindingHandle,
                    &PendingCount);

        if ((Status != NDIS_STATUS_SUCCESS) ||
            (PendingCount == 0))
        {
            break;
        }

        DEBUGP(DL_INFO, ("WaitForPendingIO: Open %p, %d pending I/O at NDIS\n",
                pOpenContext, PendingCount));

        NUIO_SLEEP(2);
    }

    NUIO_ASSERT(LoopCount < 60);

#endif // NDIS51

    //
    //  Make sure any threads trying to send have finished.
    //
    for (LoopCount = 0; LoopCount < 60; LoopCount++)
    {
        if (pOpenContext->PendedSendCount == 0)
        {
            break;
        }

        DEBUGP(DL_WARN, ("WaitForPendingIO: Open %p, %d pended sends\n",
                pOpenContext, pOpenContext->PendedSendCount));

        NUIO_SLEEP(1);
    }

    NUIO_ASSERT(LoopCount < 60);

    if (DoCancelReads)
    {
        //
        //  Wait for any pended reads to complete/cancel.
        //
        while (pOpenContext->PendedReadCount != 0)
        {
            DEBUGP(DL_INFO, ("WaitForPendingIO: Open %p, %d pended reads\n",
                pOpenContext, pOpenContext->PendedReadCount));

            //
            //  Cancel any pending reads.
            //
            ndisuioCancelPendingReads(pOpenContext);

            NUIO_SLEEP(1);
        }
    }
}

VOID
ndisuioDoProtocolUnload(
    VOID
    )
/*++

Routine Description:

    Utility routine to handle unload from the NDIS protocol side.

Arguments:

    None

Return Value:

    None

--*/
{
    NDIS_HANDLE     ProtocolHandle;
    NDIS_STATUS     Status;

    DEBUGP(DL_INFO, ("ProtocolUnload: ProtocolHandle=0x%X\n", Globals.NdisProtocolHandle));

    if (Globals.NdisProtocolHandle != NULL)
    {
        ProtocolHandle = Globals.NdisProtocolHandle;
        Globals.NdisProtocolHandle = NULL;

        NdisDeregisterProtocol(&Status, ProtocolHandle);
    }
}

NDIS_STATUS
ndisuioDoRequest(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext,
    IN NDIS_REQUEST_TYPE            RequestType,
    IN NDIS_OID                     Oid,
    IN PVOID                        InformationBuffer,
    IN UINT                         InformationBufferLength,
    OUT PUINT                       pBytesProcessed
    )
/*++

Routine Description:

    Utility routine that forms and sends an NDIS_REQUEST to the
    miniport, waits for it to complete, and returns status
    to the caller.

    NOTE: this assumes that the calling routine ensures validity
    of the binding handle until this returns.

Arguments:

    pOpenContext - pointer to our open context
    RequestType - NdisRequest[Set|Query]Information
    Oid - the object being set/queried
    InformationBuffer - data for the request
    InformationBufferLength - length of the above
    pBytesProcessed - place to return bytes read/written

Return Value:

    Status of the set/query request

--*/
{
    NDISUIO_REQUEST	ReqContext;
    PNDIS_REQUEST	pNdisRequest = &ReqContext.Request;
    NDIS_STATUS		Status;

    NUIO_INIT_EVENT(&ReqContext.ReqEvent);

    pNdisRequest->RequestType = RequestType;

    switch (RequestType)
    {
        case NdisRequestQueryInformation:
            pNdisRequest->DATA.QUERY_INFORMATION.Oid = Oid;
            pNdisRequest->DATA.QUERY_INFORMATION.InformationBuffer =
                                    InformationBuffer;
            pNdisRequest->DATA.QUERY_INFORMATION.InformationBufferLength =
                                    InformationBufferLength;
            break;

        case NdisRequestSetInformation:
            pNdisRequest->DATA.SET_INFORMATION.Oid = Oid;
            pNdisRequest->DATA.SET_INFORMATION.InformationBuffer =
                                    InformationBuffer;
            pNdisRequest->DATA.SET_INFORMATION.InformationBufferLength =
                                    InformationBufferLength;
            break;

        default:
            NUIO_ASSERT(FALSE);
            break;
    }

    NdisRequest(&Status, pOpenContext->BindingHandle, pNdisRequest);
    
    if (Status == NDIS_STATUS_PENDING)
    {
        NUIO_WAIT_EVENT(&ReqContext.ReqEvent, 0);
        Status = ReqContext.Status;
    }

    if (Status == NDIS_STATUS_SUCCESS)
    {
        *pBytesProcessed = (RequestType == NdisRequestQueryInformation) ?
                            pNdisRequest->DATA.QUERY_INFORMATION.BytesWritten:
                            pNdisRequest->DATA.SET_INFORMATION.BytesRead;
        //
        // The driver below should set the correct value to BytesWritten
        // or BytesRead. But now, we just truncate the value to InformationBufferLength if
        // BytesWritten or BytesRead is greater than InformationBufferLength
        //
        if (*pBytesProcessed > InformationBufferLength)
        {
            *pBytesProcessed = InformationBufferLength;
        }
    }

    return Status;
}

VOID
NdisuioRequestComplete(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN PNDIS_REQUEST                pNdisRequest,
    IN NDIS_STATUS                  Status
    )
/*++

Routine Description:

    NDIS entry point indicating completion of a pended NDIS_REQUEST.

Arguments:

    ProtocolBindingContext - pointer to open context
    pNdisRequest - pointer to NDIS request
    Status - status of reset completion

Return Value:

    None

--*/
{
    PNDISUIO_OPEN_CONTEXT       pOpenContext;
    PNDISUIO_REQUEST            pReqContext;

    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    NUIO_STRUCT_ASSERT(pOpenContext, oc);

    //
    //  Get at the request context.
    //
    pReqContext = CONTAINING_RECORD(pNdisRequest, NDISUIO_REQUEST, Request);

    //
    //  Save away the completion status.
    //
    pReqContext->Status = Status;

    //
    //  Wake up the thread blocked for this request to complete.
    //
    NUIO_SIGNAL_EVENT(&pReqContext->ReqEvent);
}

NDIS_STATUS
ndisuioValidateOpenAndDoRequest(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext,
    IN NDIS_REQUEST_TYPE            RequestType,
    IN NDIS_OID                     Oid,
    IN PVOID                        InformationBuffer,
    IN UINT                         InformationBufferLength,
    OUT PUINT                       pBytesProcessed,
    IN BOOLEAN                      bWaitForPowerOn
    )
/*++

Routine Description:

    Utility routine to prevalidate and reference an open context
    before calling ndisuioDoRequest. This routine makes sure
    we have a valid binding.

Arguments:

    pOpenContext - pointer to our open context
    RequestType - NdisRequest[Set|Query]Information
    Oid - the object being set/queried
    InformationBuffer - data for the request
    InformationBufferLength - length of the above
    pBytesProcessed - place to return bytes read/written
    bWaitForPowerOn - Wait for the device to be powered on if it isn't already.

Return Value:

    Status of the set/query request

--*/
{
    NDIS_STATUS             Status;

    do
    {
        if (pOpenContext == NULL)
        {
            DEBUGP(DL_WARN, ("ValidateOpenAndDoRequest: request on unassociated file object!\n"));
            Status = NDIS_STATUS_INVALID_DATA;
            break;
        }
               
        NUIO_STRUCT_ASSERT(pOpenContext, oc);

        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

        //
        //  Proceed only if we have a binding.
        //
        if (!NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_ACTIVE))
        {
            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
            Status = NDIS_STATUS_INVALID_DATA;
            break;
        }

        NUIO_ASSERT(pOpenContext->BindingHandle != NULL);

        //
        //  Make sure that the binding does not go away until we
        //  are finished with the request.
        //
        NdisInterlockedIncrement(&pOpenContext->PendedSendCount);

        NUIO_RELEASE_LOCK(&pOpenContext->Lock);

        if (bWaitForPowerOn)
        {
            //
            //  Wait for the device below to be powered up.
            //  We don't wait indefinitely here - this is to avoid
            //  a PROCESS_HAS_LOCKED_PAGES bugcheck that could happen
            //  if the calling process terminates, and this IRP doesn't
            //  complete within a reasonable time. An alternative would
            //  be to explicitly handle cancellation of this IRP.
            //
            NUIO_WAIT_EVENT(&pOpenContext->PoweredUpEvent, 4500);
        }

        Status = ndisuioDoRequest(
			pOpenContext,
			RequestType,
			Oid,
			InformationBuffer,
			InformationBufferLength,
			pBytesProcessed);
        
        //
        //  Let go of the binding.
        //
        NdisInterlockedDecrement(&pOpenContext->PendedSendCount);
    } while (FALSE);

    DEBUGP(DL_LOUD, ("ValidateOpenAndDoReq: Open %p/%x, OID %x, Status %x\n",
                pOpenContext, pOpenContext->Flags, Oid, Status));

    return (Status);
}

VOID
NdisuioResetComplete(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN NDIS_STATUS                  Status
    )
/*++

Routine Description:

    NDIS entry point indicating that a protocol initiated reset
    has completed. Since we never call NdisReset(), this should
    never be called.

Arguments:

    ProtocolBindingContext - pointer to open context
    Status - status of reset completion

Return Value:

    None

--*/
{
    NUIO_ASSERT(FALSE);
}

VOID
NdisuioStatus(
    IN NDIS_HANDLE                  ProtocolBindingContext,
    IN NDIS_STATUS                  GeneralStatus,
    IN PVOID                        StatusBuffer,
    IN UINT                         StatusBufferSize
    )
/*++

Routine Description:

    Protocol entry point called by NDIS to indicate a change
    in status at the miniport.

    We make note of reset and media connect status indications.

Arguments:

    ProtocolBindingContext - pointer to open context
    GeneralStatus - status code
    StatusBuffer - status-specific additional information
    StatusBufferSize - size of the above

Return Value:

    None

--*/
{
    PNDISUIO_OPEN_CONTEXT       pOpenContext;

    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    NUIO_STRUCT_ASSERT(pOpenContext, oc);

    DEBUGP(DL_INFO, ("Status: Open %p, Status %x\n", pOpenContext, GeneralStatus));

    NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

    do
    {
        if (pOpenContext->PowerState != NdisDeviceStateD0)
        {
            //
            //  Ignore status indications if the device is in
            //  a low power state.
            //
            DEBUGP(DL_INFO, ("Status: Open %p in power state %d,"
                " Status %x ignored\n", pOpenContext,
                pOpenContext->PowerState, GeneralStatus));
            break;
        }

        switch (GeneralStatus)
        {
            case NDIS_STATUS_RESET_START:
    
                NUIO_ASSERT(!NUIO_TEST_FLAGS(pOpenContext->Flags,
                                             NUIOO_RESET_FLAGS,
                                             NUIOO_RESET_IN_PROGRESS));

                NUIO_SET_FLAGS(pOpenContext->Flags,
                               NUIOO_RESET_FLAGS,
                               NUIOO_RESET_IN_PROGRESS);

                break;

            case NDIS_STATUS_RESET_END:

                NUIO_ASSERT(NUIO_TEST_FLAGS(pOpenContext->Flags,
                                            NUIOO_RESET_FLAGS,
                                            NUIOO_RESET_IN_PROGRESS));
   
                NUIO_SET_FLAGS(pOpenContext->Flags,
                               NUIOO_RESET_FLAGS,
                               NUIOO_NOT_RESETTING);

                break;

            case NDIS_STATUS_MEDIA_CONNECT:

                NUIO_SET_FLAGS(pOpenContext->Flags,
                               NUIOO_MEDIA_FLAGS,
                               NUIOO_MEDIA_CONNECTED);

                break;

            case NDIS_STATUS_MEDIA_DISCONNECT:

                NUIO_SET_FLAGS(pOpenContext->Flags,
                               NUIOO_MEDIA_FLAGS,
                               NUIOO_MEDIA_DISCONNECTED);

                break;

            default:
                break;
        }
    } while (FALSE);
       
    NUIO_RELEASE_LOCK(&pOpenContext->Lock);
}

VOID
NdisuioStatusComplete(
    IN NDIS_HANDLE                  ProtocolBindingContext
    )
/*++

Routine Description:

    Protocol entry point called by NDIS. We ignore this.

Arguments:

    ProtocolBindingContext - pointer to open context

Return Value:

    None

--*/
{
    PNDISUIO_OPEN_CONTEXT       pOpenContext;

    pOpenContext = (PNDISUIO_OPEN_CONTEXT)ProtocolBindingContext;
    NUIO_STRUCT_ASSERT(pOpenContext, oc);

    return;
}

PNDISUIO_OPEN_CONTEXT
ndisuioLookupDevice(
    IN PUCHAR                       pBindingInfo,
    IN ULONG                        BindingInfoLength
    )
/*++

Routine Description:

    Search our global list for an open context structure that
    has a binding to the specified device, and return a pointer
    to it.

    NOTE: we reference the open that we return.

Arguments:

    pBindingInfo - pointer to unicode device name string
    BindingInfoLength - length in bytes of the above.

Return Value:

    Pointer to the matching open context if found, else NULL

--*/
{
    PNDISUIO_OPEN_CONTEXT       pOpenContext;
    PLIST_ENTRY                 pEnt;

    pOpenContext = NULL;

    NUIO_ACQUIRE_LOCK(&Globals.GlobalLock);

    for (pEnt = Globals.OpenList.Flink; pEnt != &Globals.OpenList; pEnt = pEnt->Flink)
    {
        pOpenContext = CONTAINING_RECORD(pEnt, NDISUIO_OPEN_CONTEXT, Link);
        NUIO_STRUCT_ASSERT(pOpenContext, oc);

        //
        //  Check if this has the name we are looking for.
        //
        if ((pOpenContext->DeviceName.Length == BindingInfoLength) &&
            NUIO_MEM_CMP(pOpenContext->DeviceName.Buffer, pBindingInfo, BindingInfoLength))
        {
            NUIO_REF_OPEN(pOpenContext);   // ref added by LookupDevice
            break;
        }

        pOpenContext = NULL;
    }

    NUIO_RELEASE_LOCK(&Globals.GlobalLock);

    return (pOpenContext);
}

NDIS_STATUS
ndisuioQueryOidValue(
    IN  PNDISUIO_OPEN_CONTEXT       pOpenContext,
    OUT PVOID                       pDataBuffer,
    IN  ULONG                       BufferLength,
    OUT PULONG                      pBytesWritten
    )
/*++

Routine Description:

    Query an arbitrary OID value from the miniport.

Arguments:

    pOpenContext - pointer to open context representing our binding to the miniport
    pDataBuffer - place to store the returned value
    BufferLength - length of the above
    pBytesWritten - place to return length returned

Return Value:

    NDIS_STATUS_SUCCESS if we successfully queried the OID.
    NDIS_STATUS_XXX error code otherwise.

--*/
{
    NDIS_STATUS             Status;
    PPACKET_OID_DATA		pQuery;
    NDIS_OID                Oid;

    Oid = 0;

    do
    {
        if (BufferLength < sizeof(PACKET_OID_DATA))
        {
            Status = NDIS_STATUS_BUFFER_TOO_SHORT;
            break;
        }

        pQuery = (PPACKET_OID_DATA)pDataBuffer;
        Oid = pQuery->Oid;

        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

        if (!NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_ACTIVE))
        {
            DEBUGP(DL_WARN, ("QueryOid: Open %p/%x is in invalid state\n",
                    pOpenContext, pOpenContext->Flags));

            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
            Status = NDIS_STATUS_FAILURE;
            break;
        }

        //
        //  Make sure the binding doesn't go away.
        //
        NdisInterlockedIncrement(&pOpenContext->PendedSendCount);

        NUIO_RELEASE_LOCK(&pOpenContext->Lock);

        Status = ndisuioDoRequest(
                    pOpenContext,
                    NdisRequestQueryInformation,
                    Oid,
                    pQuery->Data,
                    BufferLength - FIELD_OFFSET(PACKET_OID_DATA, Data),
                    pBytesWritten);

        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

        NdisInterlockedDecrement(&pOpenContext->PendedSendCount);

        NUIO_RELEASE_LOCK(&pOpenContext->Lock);

        if (Status == NDIS_STATUS_SUCCESS)
        {
            *pBytesWritten += FIELD_OFFSET(PACKET_OID_DATA, Data);
        }

    } while (FALSE);

    DEBUGP(DL_LOUD, ("QueryOid: Open %p/%x, OID %x, Status %x\n",
                pOpenContext, pOpenContext->Flags, Oid, Status));

    return (Status);
}

NDIS_STATUS
ndisuioSetOidValue(
    IN  PNDISUIO_OPEN_CONTEXT       pOpenContext,
    OUT PVOID                       pDataBuffer,
    IN  ULONG                       BufferLength
    )
/*++

Routine Description:

    Set an arbitrary OID value to the miniport.

Arguments:

    pOpenContext - pointer to open context representing our binding to the miniport
    pDataBuffer - buffer that contains the value to be set
    BufferLength - length of the above

Return Value:

    NDIS_STATUS_SUCCESS if we successfully set the OID
    NDIS_STATUS_XXX error code otherwise.

--*/
{
    NDIS_STATUS             Status;
    PPACKET_OID_DATA        pSet;
    NDIS_OID                Oid;
    ULONG                   BytesWritten;

    Oid = 0;

    do
    {
        if (BufferLength < sizeof(PACKET_OID_DATA))
        {
            Status = NDIS_STATUS_BUFFER_TOO_SHORT;
            break;
        }

        pSet = (PPACKET_OID_DATA)pDataBuffer;
        Oid = pSet->Oid;

        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

        if (!NUIO_TEST_FLAGS(pOpenContext->Flags, NUIOO_BIND_FLAGS, NUIOO_BIND_ACTIVE))
        {
            DEBUGP(DL_WARN,
                ("SetOid: Open %p/%x is in invalid state\n",
                    pOpenContext, pOpenContext->Flags));

            NUIO_RELEASE_LOCK(&pOpenContext->Lock);
            Status = NDIS_STATUS_FAILURE;
            break;
        }

        //
        //  Make sure the binding doesn't go away.
        //
        NdisInterlockedIncrement(&pOpenContext->PendedSendCount);

        NUIO_RELEASE_LOCK(&pOpenContext->Lock);

        Status = ndisuioDoRequest(
                    pOpenContext,
                    NdisRequestSetInformation,
                    Oid,
                    pSet->Data,
                    BufferLength - FIELD_OFFSET(PACKET_OID_DATA, Data),
                    &BytesWritten);

        NUIO_ACQUIRE_LOCK(&pOpenContext->Lock);

        NdisInterlockedDecrement(&pOpenContext->PendedSendCount);

        NUIO_RELEASE_LOCK(&pOpenContext->Lock);
    } while (FALSE);

    DEBUGP(DL_LOUD, ("SetOid: Open %p/%x, OID %x, Status %x\n",
                pOpenContext, pOpenContext->Flags, Oid, Status));

    return (Status);
}

VOID
ndisuioRefOpen(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext
    )
/*++

Routine Description:

    Reference the given open context.

    NOTE: Can be called with or without holding the opencontext lock.

Arguments:

    pOpenContext - pointer to open context

Return Value:

    None

--*/
{
    NdisInterlockedIncrement(&pOpenContext->RefCount);
}


VOID
ndisuioDerefOpen(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext
    )
/*++

Routine Description:

    Dereference the given open context. If the ref count goes to zero,
    free it.

    NOTE: called without holding the opencontext lock

Arguments:

    pOpenContext - pointer to open context

Return Value:

    None

--*/
{
    if (NdisInterlockedDecrement(&pOpenContext->RefCount) == 0)
    {
        DEBUGP(DL_INFO, ("DerefOpen: Open %p, Flags %x, ref count is zero!\n",
            pOpenContext, pOpenContext->Flags));
        
        NUIO_ASSERT(pOpenContext->BindingHandle == NULL);
        NUIO_ASSERT(pOpenContext->RefCount == 0);
        NUIO_ASSERT(pOpenContext->pFileObject == NULL);

        pOpenContext->oc_sig++;

        //
        //  Free it.
        //
        NUIO_FREE_MEM(pOpenContext);
    }
}

#if DBG

VOID
ndisuioDbgRefOpen(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext,
    IN LPCSTR						pFilename,
    IN ULONG                        LineNumber
    )
{
    DEBUGP(DL_VERY_LOUD, ("  RefOpen: Open %p, old ref %d, File %s, line %d\n",
            pOpenContext,
            pOpenContext->RefCount,
			pFilename,
            LineNumber));

    ndisuioRefOpen(pOpenContext);
}

VOID
ndisuioDbgDerefOpen(
    IN PNDISUIO_OPEN_CONTEXT        pOpenContext,
    IN LPCSTR						pFilename,
    IN ULONG                        LineNumber
    )
{
    DEBUGP(DL_VERY_LOUD, ("DerefOpen: Open %p, old ref %d, File %s, line %d\n",
            pOpenContext,
            pOpenContext->RefCount,
			pFilename,
            LineNumber));

    ndisuioDerefOpen(pOpenContext);
}

INT             	ndisuioDebugLevel = DL_LOUD;

NDIS_SPIN_LOCK		ndisuioDbgLogLock;

PNUIOD_ALLOCATION	ndisuiodMemoryHead = (PNUIOD_ALLOCATION)NULL;
PNUIOD_ALLOCATION	ndisuiodMemoryTail = (PNUIOD_ALLOCATION)NULL;
ULONG				ndisuiodAllocCount = 0;	// how many allocated so far (unfreed)

NDIS_SPIN_LOCK		ndisuiodMemoryLock;
BOOLEAN				ndisuiodInitDone = FALSE;

PVOID
ndisuioAuditAllocMem(
	PVOID	pPointer,
	ULONG	Size,
	LPCSTR	pFilename,
	ULONG	LineNumber
	)
{
	PVOID				pBuffer;
	PNUIOD_ALLOCATION	pAllocInfo;

	if (!ndisuiodInitDone)
	{
		NdisAllocateSpinLock(&(ndisuiodMemoryLock));
		ndisuiodInitDone = TRUE;
	}

	NdisAllocateMemoryWithTag(&pAllocInfo, Size + sizeof(NUIOD_ALLOCATION), 'P5DN');

	if (pAllocInfo == NULL)
	{
		DEBUGP(DL_VERY_LOUD, ("ndisuioAuditAllocMem: file %s, line %d, Size %d failed!\n",
				pFilename, LineNumber, Size));
		pBuffer = NULL;
	}
	else
	{
		pBuffer = &pAllocInfo->UserData;
		NUIO_SET_MEM(pBuffer, 0xCC, Size);

		pAllocInfo->Signature = NUIOD_MEMORY_SIGNATURE;
		pAllocInfo->FileName = pFilename;
		pAllocInfo->LineNumber = LineNumber;
		pAllocInfo->Size = Size;
		pAllocInfo->Location = pPointer;
		pAllocInfo->Next = NULL;

		NdisAcquireSpinLock(&ndisuiodMemoryLock);

		pAllocInfo->Prev = ndisuiodMemoryTail;
		if (ndisuiodMemoryTail == NULL)
		{
			// empty list
			ndisuiodMemoryHead = ndisuiodMemoryTail = pAllocInfo;
		}
		else
		{
			ndisuiodMemoryTail->Next = pAllocInfo;
		}
		ndisuiodMemoryTail = pAllocInfo;
		
		ndisuiodAllocCount++;
		NdisReleaseSpinLock(&ndisuiodMemoryLock);
	}

	DEBUGP(DL_VERY_LOUD+100, ("AllocMem: file %s, line %d, %d bytes, [0x%x] = 0x%x\n",
	 	pFilename, LineNumber, Size, pPointer, pBuffer));

	return pBuffer;
}

VOID
ndisuioAuditFreeMem(
	PVOID	Pointer
	)
{
	PNUIOD_ALLOCATION	pAllocInfo;

	NdisAcquireSpinLock(&ndisuiodMemoryLock);

	pAllocInfo = CONTAINING_RECORD(Pointer, NUIOD_ALLOCATION, UserData);

	if (pAllocInfo->Signature != NUIOD_MEMORY_SIGNATURE)
	{
		DEBUGP(DL_ERROR, ("FreeMem: unknown buffer 0x%x!\n", Pointer));
		NdisReleaseSpinLock(&(ndisuiodMemoryLock));
#if DBG
		DbgBreakPoint();
#endif
		return;
	}

	pAllocInfo->Signature = 'DEAD';
	if (pAllocInfo->Prev != NULL)
	{
		pAllocInfo->Prev->Next = pAllocInfo->Next;
	}
	else
	{
		ndisuiodMemoryHead = pAllocInfo->Next;
	}
	if (pAllocInfo->Next != NULL)
	{
		pAllocInfo->Next->Prev = pAllocInfo->Prev;
	}
	else
	{
		ndisuiodMemoryTail = pAllocInfo->Prev;
	}
	ndisuiodAllocCount--;
	NdisReleaseSpinLock(&ndisuiodMemoryLock);

	NdisFreeMemory(pAllocInfo, 0, 0);
}

VOID
ndisuioAuditShutdown(
	VOID
	)
{
	if (ndisuiodInitDone)
	{
		if (ndisuiodAllocCount != 0)
		{
			DEBUGP(DL_ERROR, ("AuditShutdown: unfreed memory, %d blocks!\n",
					ndisuiodAllocCount));
			DEBUGP(DL_ERROR, ("MemoryHead: 0x%x, MemoryTail: 0x%x\n",
					ndisuiodMemoryHead, ndisuiodMemoryTail));
			DbgBreakPoint();
			{
				PNUIOD_ALLOCATION		pAllocInfo;

				while (ndisuiodMemoryHead != (PNUIOD_ALLOCATION)NULL)
				{
					pAllocInfo = ndisuiodMemoryHead;
					DEBUGP(DL_INFO, ("AuditShutdown: will free 0x%x\n", pAllocInfo));
					ndisuioAuditFreeMem(&pAllocInfo->UserData);
				}
			}
		}
		ndisuiodInitDone = FALSE;
	}
}

#define MAX_HD_LENGTH		128

VOID
DbgPrintHexDump(
	IN	PUCHAR			pBuffer,
	IN	ULONG			Length
	)
/*++

Routine Description:

	Print a hex dump of the given contiguous buffer. If the length
	is too long, we truncate it.

Arguments:
	pBuffer			- Points to start of data to be dumped
	Length			- Length of above.

Return Value:
	None

--*/
{
	ULONG		i;
	if (Length > MAX_HD_LENGTH)
	{
		Length = MAX_HD_LENGTH;
	}
	for (i = 0; i < Length; i++)
	{
		//
		//  Check if we are at the end of a line
		//
		if ((i > 0) && ((i & 0xf) == 0))
		{
			DbgPrint("\n");
		}

		//
		//  Print addr if we are at start of a new line
		//
		if ((i & 0xf) == 0)
		{
			DbgPrint("%08x ", pBuffer);
		}

		DbgPrint(" %02x", *pBuffer++);
	}
	//
	//  Terminate the last line.
	//
	if (Length > 0)
	{
		DbgPrint("\n");
	}
}
#endif // DBG
#if DBG_SPIN_LOCK
ULONG	ndisuiodSpinLockInitDone = 0;
NDIS_SPIN_LOCK	ndisuiodLockLock;
VOID
ndisuioAllocateSpinLock(
	IN	PNUIO_LOCK		pLock,
	IN	ULONG				FileNumber,
	IN	ULONG				LineNumber
	)
{
	if (ndisuiodSpinLockInitDone == 0)
	{
		ndisuiodSpinLockInitDone = 1;
		NdisAllocateSpinLock(&(ndisuiodLockLock));
	}

	NdisAcquireSpinLock(&(ndisuiodLockLock));
	pLock->Signature = NUIOL_SIG;
	pLock->TouchedByFileNumber = FileNumber;
	pLock->TouchedInLineNumber = LineNumber;
	pLock->IsAcquired = 0;
	pLock->OwnerThread = 0;
	NdisAllocateSpinLock(&(pLock->NdisLock));
	NdisReleaseSpinLock(&(ndisuiodLockLock));
}
VOID
ndisuioAcquireSpinLock(
	IN	PNUIO_LOCK		pLock,
	IN	ULONG				FileNumber,
	IN	ULONG				LineNumber
	)
{
	PKTHREAD		pThread;

	pThread = KeGetCurrentThread();
	NdisAcquireSpinLock(&(ndisuiodLockLock));
	if (pLock->Signature != NUIOL_SIG)
	{
		DbgPrint("Trying to acquire uninited lock 0x%x, File %c%c%c%c, Line %d\n",
				pLock,
				(CHAR)(FileNumber & 0xff),
				(CHAR)((FileNumber >> 8) & 0xff),
				(CHAR)((FileNumber >> 16) & 0xff),
				(CHAR)((FileNumber >> 24) & 0xff),
				LineNumber);
		DbgBreakPoint();
	}

	if (pLock->IsAcquired != 0)
	{
		if (pLock->OwnerThread == pThread)
		{
			DbgPrint("Detected multiple locking!: pLock 0x%x, File %c%c%c%c, Line %d\n",
				pLock,
				(CHAR)(FileNumber & 0xff),
				(CHAR)((FileNumber >> 8) & 0xff),
				(CHAR)((FileNumber >> 16) & 0xff),
				(CHAR)((FileNumber >> 24) & 0xff),
				LineNumber);
			DbgPrint("pLock 0x%x already acquired in File %c%c%c%c, Line %d\n",
				pLock,
				(CHAR)(pLock->TouchedByFileNumber & 0xff),
				(CHAR)((pLock->TouchedByFileNumber >> 8) & 0xff),
				(CHAR)((pLock->TouchedByFileNumber >> 16) & 0xff),
				(CHAR)((pLock->TouchedByFileNumber >> 24) & 0xff),
				pLock->TouchedInLineNumber);
			DbgBreakPoint();
		}
	}

	pLock->IsAcquired++;

	NdisReleaseSpinLock(&(ndisuiodLockLock));
	NdisAcquireSpinLock(&(pLock->NdisLock));

	//
	//  Mark this lock.
	//
	pLock->OwnerThread = pThread;
	pLock->TouchedByFileNumber = FileNumber;
	pLock->TouchedInLineNumber = LineNumber;
}

VOID
ndisuioReleaseSpinLock(
	IN	PNUIO_LOCK		pLock,
	IN	ULONG				FileNumber,
	IN	ULONG				LineNumber
	)
{
	NdisDprAcquireSpinLock(&(ndisuiodLockLock));
	if (pLock->Signature != NUIOL_SIG)
	{
		DbgPrint("Trying to release uninited lock 0x%x, File %c%c%c%c, Line %d\n",
				pLock,
				(CHAR)(FileNumber & 0xff),
				(CHAR)((FileNumber >> 8) & 0xff),
				(CHAR)((FileNumber >> 16) & 0xff),
				(CHAR)((FileNumber >> 24) & 0xff),
				LineNumber);
		DbgBreakPoint();
	}

	if (pLock->IsAcquired == 0)
	{
		DbgPrint("Detected release of unacquired lock 0x%x, File %c%c%c%c, Line %d\n",
				pLock,
				(CHAR)(FileNumber & 0xff),
				(CHAR)((FileNumber >> 8) & 0xff),
				(CHAR)((FileNumber >> 16) & 0xff),
				(CHAR)((FileNumber >> 24) & 0xff),
				LineNumber);
		DbgBreakPoint();
	}
	pLock->TouchedByFileNumber = FileNumber;
	pLock->TouchedInLineNumber = LineNumber;
	pLock->IsAcquired--;
	pLock->OwnerThread = 0;
	NdisDprReleaseSpinLock(&(ndisuiodLockLock));

	NdisReleaseSpinLock(&(pLock->NdisLock));
}
#endif // DBG_SPIN_LOCK


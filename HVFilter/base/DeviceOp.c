//#include <ntddk.h>
#include <ntifs.h>
//#include <wdm.h>

#include "DeviceOp.h"

#include "DeviceInfo.h"
#include "../HVService/HVService/Stuff.h"
#include "../HVService/HVService/HVioctl.h"
#include "../samples/passthrough/SendPacketsInfo.h"

extern PDEVICE_OBJECT OsrDataDeviceObject;
extern PDEVICE_OBJECT OsrCommDeviceObject;
extern LONG OsrRequestID;



//
// OsrCommCreate
//
//  This is the create entry point
//
// Inputs:
//  DeviceObject - this is the device object on which we are operating
//  Irp - this is the create IRP
//
// Outputs:
//  None.
//
// Returns:
//  SUCCESS - the operation was successful.
//
// Notes:
//  None.
//
NTSTATUS OsrCommCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
  POSR_COMM_DATA_DEVICE_EXTENSION dataExt;

  //
  // Tell the caller "yes"
  //
  Irp->IoStatus.Status = STATUS_SUCCESS;

  Irp->IoStatus.Information = FILE_OPENED;

  IoCompleteRequest(Irp, IO_NO_INCREMENT);

  //
  // If this was the control device, note that we now have
  // activated the data device.
  //
  if (OSR_COMM_CONTROL_TYPE == DeviceObject->DeviceType) {

    dataExt = (POSR_COMM_DATA_DEVICE_EXTENSION) OsrDataDeviceObject->DeviceExtension;

    dataExt->DeviceState = OSR_COMM_DATA_DEVICE_ACTIVE;

  }

  //
  // Done.
  //
  return STATUS_SUCCESS;

}

typedef struct _OSR_COMM_DATA_REQUEST {
  //
  // This is used to thread the requests onto a data request queue
  //
  LIST_ENTRY ListEntry;

  //
  // This is used to thread the requests onto a service request queue
  //
  LIST_ENTRY ServiceListEntry;

  //
  // The request ID is used to match up the response.
  //
  ULONG RequestID;

  //
  // The IRP is the one associated with this particular operation.
  //
  PIRP Irp;

} OSR_COMM_DATA_REQUEST, *POSR_COMM_DATA_REQUEST;

//
// CancelPendingRequestList
//
//   This routine will walk through a list of data requests
//   looking for requests that need to be cancelled.
//
// Inputs:
//   List - this is the list to traverse
//   ListLock - this is the fast mutex protecting the list
//   FileObject - this is the file object to match against the requests being cancelled.  If it is
//                zero, it indicates that all entries on the queue should be cancelled.
//
// Outputs:
//   None.
//
// Returns:
//   VOID function
//
// Notes:
//   This is a "helper" function for cleanup, not a general-purpose cancellation mechanism.
//
static VOID CancelPendingRequestList(PLIST_ENTRY List, PFAST_MUTEX ListLock, PFILE_OBJECT FileObject)
{
  PLIST_ENTRY listEntry;
  PLIST_ENTRY nextListEntry;
  POSR_COMM_DATA_REQUEST dataRequest;

  //
  // Lock the list
  //
  ExAcquireFastMutex(ListLock);

  //
  // Walk the list
  //
  for (listEntry = List->Flink;
       listEntry != List;
       listEntry = nextListEntry) {
    //
    // Set up the next list entry first.  Thus, even if we delete the
    // list entry from the queue, we can skip to the next entry
    //
    nextListEntry = listEntry->Flink;

    //
    // Extract the data request from the list entry
    //
    dataRequest = CONTAINING_RECORD(listEntry, OSR_COMM_DATA_REQUEST, ListEntry);

    //
    // If there is an IRP associated with this request and the file object in the IRP
    // stack location matches the file object passed to this call, then we need to
    // cancel this request.
    //
    // Note that if the file object is NULL we cancel ALL I/O operations on the queue.
    //
    if (dataRequest->Irp &&
        ((NULL == FileObject) ||
         (IoGetCurrentIrpStackLocation(dataRequest->Irp)->FileObject == FileObject))) {
      
      //
      // We need to remove this entry from the queue
      //
      RemoveEntryList(listEntry);

      //
      // Cancel this IRP
      //
      dataRequest->Irp->IoStatus.Status = STATUS_CANCELLED;

      dataRequest->Irp->IoStatus.Information = 0;

      IoCompleteRequest(dataRequest->Irp, IO_NO_INCREMENT);

      //
      // Free this data request
      //
      ExFreePool(dataRequest);

    }

  }

  //
  // Unlock the list
  //
  ExReleaseFastMutex(ListLock);

  //
  // Done!
  //
  return;
}

//
// 
//OsrCommCleanup
//  This is the cleanup entry point
//
// Inputs:
//  DeviceObject - this is the device object on which we are operating
//  Irp - this is the cleanup IRP
//
// Outputs:
//  None.
//
// Returns:
//  SUCCESS - the operation was successful.
//
// Notes:
//  None.
//
NTSTATUS OsrCommCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
//	PLIST_ENTRY listEntry;
//  PLIST_ENTRY nextListEntry;
  POSR_COMM_DATA_DEVICE_EXTENSION dataExt;
  POSR_COMM_CONTROL_DEVICE_EXTENSION controlExt;

  DbgPrint("OsrCommCleanup: entered. \n");

  //
  // If this is the control object, we need to deal with it closing,
  // in case there are any existing threads waiting for answers from
  // the control object.
  //
  if (OSR_COMM_CONTROL_TYPE == DeviceObject->DeviceType) {

    //
    // Need the data device
    //
    dataExt = (POSR_COMM_DATA_DEVICE_EXTENSION) OsrDataDeviceObject->DeviceExtension;
    controlExt = (POSR_COMM_CONTROL_DEVICE_EXTENSION) DeviceObject->DeviceExtension;

    //
    // Set the device state
    //
    dataExt->DeviceState = OSR_COMM_DATA_DEVICE_INACTIVE;


    //
    // In this case, we must cancel all pending requests on the queue
    //

    CancelPendingRequestList(&controlExt->ServiceQueue,
                             &controlExt->ServiceQueueLock,
                             NULL);

    CancelPendingRequestList(&controlExt->RequestQueue,
                             &controlExt->RequestQueueLock,
                             NULL);


    //
    // In this case, we must cancel all pending requests on the queue
    //
    CancelPendingRequestList(&dataExt->ReadRequestQueue,
                             &dataExt->ReadRequestQueueLock,
                             NULL);

    CancelPendingRequestList(&dataExt->WriteRequestQueue,
                             &dataExt->WriteRequestQueueLock,
                             NULL);

  }

  //
  // If this is the data object, we need to find any pending requests
  // and remove them from the queue
  //
  if (OSR_COMM_DATA_TYPE == DeviceObject->DeviceType) {

    //
    // We need to walk the list of pending requests and
    // remove any matching the file object in the cleanup request.
    //
    dataExt = (POSR_COMM_DATA_DEVICE_EXTENSION) DeviceObject->DeviceExtension;
    controlExt = (POSR_COMM_CONTROL_DEVICE_EXTENSION) OsrCommDeviceObject->DeviceExtension;

    //
    // In this case, we must cancel all pending requests on the queue
    //

    CancelPendingRequestList(&controlExt->ServiceQueue,
                             &controlExt->ServiceQueueLock,
                             IoGetCurrentIrpStackLocation(Irp)->FileObject);

    CancelPendingRequestList(&controlExt->RequestQueue,
                             &controlExt->RequestQueueLock,
                             IoGetCurrentIrpStackLocation(Irp)->FileObject);


    //
    // Clean up the pending read queue
    //
    CancelPendingRequestList(&dataExt->ReadRequestQueue,
                             &dataExt->ReadRequestQueueLock,
                             IoGetCurrentIrpStackLocation(Irp)->FileObject);

    //
    // Clean up the pending write queue
    //
    CancelPendingRequestList(&dataExt->WriteRequestQueue,
                             &dataExt->WriteRequestQueueLock,
                             IoGetCurrentIrpStackLocation(Irp)->FileObject);

  }

  //
  // Tell the caller "yes"
  //
  Irp->IoStatus.Status = STATUS_SUCCESS;

  Irp->IoStatus.Information = 0;

  IoCompleteRequest(Irp, IO_NO_INCREMENT);

  //
  // Done.
  //
  return STATUS_SUCCESS;

}

//
// Close
//
//  This is the close entry point
//
// Inputs:
//  DeviceObject - this is the device object on which we are operating
//  Irp - this is the close IRP
//
// Outputs:
//  None.
//
// Returns:
//  SUCCESS - the operation was successful.
//
// Notes:
//  None.
//
NTSTATUS OsrCommClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
  //
  // Tell the caller "yes"
  //
  Irp->IoStatus.Status = STATUS_SUCCESS;

  Irp->IoStatus.Information = 0;

  IoCompleteRequest(Irp, IO_NO_INCREMENT);

  //
  // Done.
  //
  return STATUS_SUCCESS;

}



//
// ProcessResponse
//
//  This routine is used to process a response
//
// Inputs:
//  Irp - this is the IRP containing a (validated) response
//
// Outputs:
//  None.
//
// Returns:
//  STATUS_SUCCESS - the operation completed successfully
//
// Notes:
//  This is a helper function for the device control logic.  It does NOT
//  complete the control request - that is the job of the caller!  It DOES
//  complete the data request (if it finds a matching entry).
//

NTSTATUS ProcessResponse(PIRP Irp)

{
  POSR_COMM_CONTROL_RESPONSE response;
  PLIST_ENTRY queue;
  PFAST_MUTEX queueLock;
  PLIST_ENTRY listEntry;
//  PLIST_ENTRY nextEntry;
  POSR_COMM_DATA_REQUEST dataRequest = NULL;
  NTSTATUS status = STATUS_SUCCESS;
  PVOID requestBuffer;
  PIO_STACK_LOCATION irpSp;
  ULONG bytesToCopy;
  POSR_COMM_DATA_DEVICE_EXTENSION dataExt =
    (POSR_COMM_DATA_DEVICE_EXTENSION) OsrDataDeviceObject->DeviceExtension;
//  POSR_COMM_CONTROL_DEVICE_EXTENSION controlExt = (POSR_COMM_CONTROL_DEVICE_EXTENSION) OsrCommDeviceObject->DeviceExtension;
  //UCHAR* next_addr = NULL;

  DbgPrint("ProcessResponse: Entered. \n");

  //
  // Get the response packet
  //
  response = (POSR_COMM_CONTROL_RESPONSE) Irp->AssociatedIrp.SystemBuffer;

  //
  // Let's pick the right queue to process
  //
  if (OSR_COMM_READ_RESPONSE == response->ResponseType) {

    queue = &dataExt->ReadRequestQueue;
    queueLock = &dataExt->ReadRequestQueueLock;
    
  } else if (OSR_COMM_WRITE_RESPONSE == response->ResponseType) {

      queue = &dataExt->WriteRequestQueue;
      queueLock = &dataExt->WriteRequestQueueLock;

  } else {

    //
    // Invalid response
    //
    Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;

    Irp->IoStatus.Information = 0;

    //
    // Caller will handle completing the request
    //
    return STATUS_INVALID_PARAMETER;

  }
        
  ExAcquireFastMutex(queueLock);

  for (listEntry = queue->Flink;
       listEntry != queue;
       listEntry = listEntry->Flink) {

    //
    // Check to see if this response matches up
    //
    dataRequest = CONTAINING_RECORD(listEntry, OSR_COMM_DATA_REQUEST, ListEntry);

    if (dataRequest->RequestID == response->RequestID) {

      //
      // This is our request, process it:
      //  - Remove it from the list
      //  - Transfer the data
      //  - Indicate the results
      //  - Complete the IRP
      //
      RemoveEntryList(listEntry);

      requestBuffer = MmGetSystemAddressForMdlSafe(dataRequest->Irp->MdlAddress, NormalPagePriority);

      if (NULL == requestBuffer) {
        //
        // We were unable to obtain the system PTEs
        //
        dataRequest->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

        dataRequest->Irp->IoStatus.Information = 0;

        IoCompleteRequest(dataRequest->Irp, IO_NO_INCREMENT);

        status = STATUS_INSUFFICIENT_RESOURCES;

        ExFreePool(dataRequest);

        break; // from loop

      }

      //
      // Figure out how much data we are going to actually copy here.
      //
      irpSp = IoGetCurrentIrpStackLocation(dataRequest->Irp);
      
      if (response->ResponseBufferLength < irpSp->Parameters.Read.Length ||
		  response->ResponseBufferLength < 2000 * 2) {
        
		  ExReleaseFastMutex(queueLock);

		  //TODO: does it leak if I return here?
		  return STATUS_INVALID_PARAMETER;

        //bytesToCopy = response->ResponseBufferLength;
        
      } else {
        
        bytesToCopy = irpSp->Parameters.Read.Length;
        
      }

	  
	  ExAcquireFastMutex(&g_inbound_mutex);
	  ExAcquireFastMutex(&g_outbound_mutex);
	  //next_addr = ((unsigned char*)requestBuffer) + sizeof(ulCount);
      //
      // We run this in a try/except to protect against bogus pointers, the usual
      //
      __try { 
		  RtlCopyMemory(requestBuffer, g_pInboundData, 2000);
		  RtlCopyMemory((BYTE*)requestBuffer + 2000, g_pOutboundData, 2000);

      } __except (EXCEPTION_EXECUTE_HANDLER) {

        status = GetExceptionCode();

      }

	  ExReleaseFastMutex(&g_inbound_mutex);
	  ExReleaseFastMutex(&g_outbound_mutex);

      dataRequest->Irp->IoStatus.Status = status;

      dataRequest->Irp->IoStatus.Information = NT_SUCCESS(status) ? bytesToCopy : 0;

      IoCompleteRequest(dataRequest->Irp, IO_NO_INCREMENT);

      ExFreePool(dataRequest);

      //
      // The control operation was successful in any case
      //
      status = STATUS_SUCCESS;

      //
      // And break from the loop, no sense looking any farther
      //
      break;

    }
    
  }

  ExReleaseFastMutex(queueLock);

  //
  // Return the results of the operation.
  //
  return status;
  
}

//
// ProcessControlRequest
//
//  This routine is used to either satisfy the control request or enqueue it
//
// Inputs:
//  Irp - this is the IRP that we are processing
//  ControlRequest - this is the control request (from the IRP, actually)
//
// Outputs:
//  None.
//
// Returns:
//  SUCCESS - there's data going back up to the application
//  PENDING - the IRP will block and wait 'til it is time...
//
//
NTSTATUS ProcessControlRequest(PIRP Irp)
{
  POSR_COMM_CONTROL_DEVICE_EXTENSION controlExt =
    (POSR_COMM_CONTROL_DEVICE_EXTENSION) OsrCommDeviceObject->DeviceExtension;
  PLIST_ENTRY listEntry = NULL;
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  POSR_COMM_CONTROL_REQUEST controlRequest;
//  PIRP dataIrp;
  POSR_COMM_DATA_REQUEST dataRequest;
  PIO_STACK_LOCATION irpSp;
  PVOID dataBuffer;
  ULONG bytesToCopy;

  DbgPrint("ProcessControlRequest: Entered. \n");

  //
  // First, we need to lock the control queue before we do anything else
  //
  ExAcquireFastMutex(&controlExt->ServiceQueueLock);

  ExAcquireFastMutex(&controlExt->RequestQueueLock);

  //
  // Check request queue
  //
  if (!IsListEmpty(&controlExt->RequestQueue)) {

    listEntry = RemoveHeadList(&controlExt->RequestQueue);

    status = STATUS_SUCCESS;

  } else {

    POSR_COMM_CONTROL_REQUEST controlRequest = (POSR_COMM_CONTROL_REQUEST) Irp->AssociatedIrp.SystemBuffer;
    irpSp = IoGetCurrentIrpStackLocation(Irp);

    if(!controlRequest || irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(OSR_COMM_CONTROL_REQUEST)) {

        status = STATUS_INVALID_PARAMETER;

    } else {

        //
        // We have to insert the control IRP into the queue
        //
        IoMarkIrpPending(Irp);

        DbgPrint("ProcessControlRequest:  Irp %x, RequestBuffer %x RequestBufferLength %x\n.",
                 Irp,
                 controlRequest->RequestBuffer,
                 controlRequest->RequestBufferLength);

        InsertTailList(&controlExt->ServiceQueue, &Irp->Tail.Overlay.ListEntry);

        status = STATUS_PENDING;

    }

  }

  //
  // OK.  At this point we can drop both locks
  //
  ExReleaseFastMutex(&controlExt->RequestQueueLock);

  ExReleaseFastMutex(&controlExt->ServiceQueueLock);

  //
  // If we found an entry to process, we need to return the information to
  // the caller here.
  //
  if (listEntry) {

    //
    // This is the request we removed from the queue.
    //
    controlRequest = (POSR_COMM_CONTROL_REQUEST) Irp->AssociatedIrp.SystemBuffer;

    dataRequest = CONTAINING_RECORD(listEntry, OSR_COMM_DATA_REQUEST, ServiceListEntry);

    irpSp = IoGetCurrentIrpStackLocation(dataRequest->Irp);

    //
    // We are going to use the request ID to match the response
    //
    controlRequest->RequestID = dataRequest->RequestID;

    //
    // Is this a read or write?
    //
    if (IRP_MJ_WRITE == irpSp->MajorFunction) {

      controlRequest->RequestType = OSR_COMM_WRITE_REQUEST; // : OSR_COMM_READ_REQUEST;

      //
      // We must copy the data from the user's address space to the control application's
      // address space.
      //
      dataBuffer = MmGetSystemAddressForMdlSafe(dataRequest->Irp->MdlAddress, NormalPagePriority);

      if (NULL == dataBuffer) {
        //
        // It failed.
        //
        dataRequest->Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

        dataRequest->Irp->IoStatus.Information = 0;

        IoCompleteRequest(dataRequest->Irp, IO_NO_INCREMENT);

        ExFreePool(dataRequest);

        return status;

      }

      //
      // Figure out how much data we are going to move.  Allow control app to set its
      // own MAX size here...
      //
      if (irpSp->Parameters.Write.Length < controlRequest->RequestBufferLength) {
        
        bytesToCopy = irpSp->Parameters.Write.Length;

      } else {

        bytesToCopy = controlRequest->RequestBufferLength;

      }

      //
      // Since the control application's address space is "naked" here we must protect our
      // data copy.
      //
      __try {

        RtlCopyMemory(controlRequest->RequestBuffer, dataBuffer, bytesToCopy);

        controlRequest->RequestBufferLength = bytesToCopy;

        status = STATUS_SUCCESS;

      } __except(EXCEPTION_EXECUTE_HANDLER) {

        status = GetExceptionCode();

      }

      //
      // We return the final results below...
      //

    } else {

      //
      // This is a read operation
      //

      controlRequest->RequestType = OSR_COMM_READ_REQUEST;

      //
      // For a READ operation, we must lob data into the user's address space
      //
      controlRequest->RequestBuffer = NULL;

      controlRequest->RequestBufferLength = IoGetCurrentIrpStackLocation(dataRequest->Irp)->Parameters.Read.Length;


      //
      // We've finished processing the request to this point.  Dispatch to the control application
      // for further processing.
      //

      status = STATUS_SUCCESS;
    }

  }

  if(status == STATUS_SUCCESS) {

      Irp->IoStatus.Information = sizeof(OSR_COMM_CONTROL_REQUEST);

  }

  return status;
}

//
// OsrCommDeviceControl
//
//  This is the device control entry point
//
// Inputs:
//  DeviceObject - this is the device object on which we are operating
//  Irp - this is the device control IRP
//
// Outputs:
//  None.
//
// Returns:
//  SUCCESS - the operation was successful.
//
// Notes:
//  None.
//
NTSTATUS OsrCommDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
//  POSR_COMM_CONTROL_RESPONSE controlResponse;
//  POSR_COMM_CONTROL_REQUEST  controlRequest;
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  BOOLEAN sendResponse;
  BOOLEAN getRequest;
  NTSTATUS status = STATUS_SUCCESS;

  DbgPrint("OsrCommDeviceControl: Entered. \n");

  //
  // This is only supported for the control device
  //
  if (OSR_COMM_CONTROL_TYPE != DeviceObject->DeviceType) {

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;

    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    //
    // Note that we don't do this...
    //
    return STATUS_INVALID_DEVICE_REQUEST;

  }

  //
  // Case on the control code
  //
  switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
    
    case OSR_COMM_CONTROL_GET_AND_SEND:
    DbgPrint("OsrCommDeviceControl: GET_AND_SEND received.\n");
    sendResponse = TRUE;
    getRequest = TRUE;
    break;

    case OSR_COMM_CONTROL_GET_REQUEST:
    DbgPrint("OsrCommDeviceControl: GET received.\n");
    sendResponse = FALSE;
    getRequest = TRUE;
    break;

    case OSR_COMM_CONTROL_SEND_RESPONSE:
    DbgPrint("OsrCommDeviceControl: SEND received.\n");
    sendResponse = TRUE;
    getRequest = FALSE;
    break;

    default:
    //
    // What IS this thing?
    //
    sendResponse = FALSE;
    getRequest = FALSE;
    break;
  }

  if (!sendResponse && !getRequest) {
    //
    // Invalid request operation
    //
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;

    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_INVALID_DEVICE_REQUEST;

  }

  //
  // Parameter validation...
  //

  if (sendResponse) {
    //
    // Validate response parameters
    //
    if (irpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(OSR_COMM_CONTROL_RESPONSE)) {

      Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;

      Irp->IoStatus.Information = sizeof(OSR_COMM_CONTROL_RESPONSE);

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return STATUS_BUFFER_TOO_SMALL;

    }

  }

  if (getRequest) {

    if (irpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(OSR_COMM_CONTROL_REQUEST)) {
      
      Irp->IoStatus.Status = STATUS_BUFFER_OVERFLOW;
      
      Irp->IoStatus.Information = sizeof(OSR_COMM_CONTROL_REQUEST);
      
      IoCompleteRequest(Irp, IO_NO_INCREMENT);
      
      return STATUS_BUFFER_OVERFLOW;
      
    }

  }

  //
  // Parameters are OK at this point.  Let's process the request/response stuff
  //
  if (sendResponse) {

    //
    // Have to handle response first.
    //
    status = ProcessResponse(Irp);

    //
    // If there was an error, the error is reported back
    // immediately, even if this was also a get request call
    //
    if (!NT_SUCCESS(status)) {

      return status;

    }

  }

  if (getRequest) {

    //
    // Now process the request.  This IRP may be queued
    // as part of the processing here.
    //
    status = ProcessControlRequest(Irp);

  }

  //
  // If the request was not pending, we complete it here.  Note that
  // we assume the subroutines have set up the IRP for completion.  We're
  // just the only point where we can safely complete it (convergence for
  // both branches above.)
  //
  if (STATUS_PENDING != status) {

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

  }

  //
  // Regardless, at this point we return the status back to the caller.
  //
  return status;

}

//
// OsrCommReadWrite
//
//  This is the read/write entry point
//
// Inputs:
//  DeviceObject - this is the device object on which we are operating
//  Irp - this is the read IRP
//
// Outputs:
//  None.
//
// Returns:
//  SUCCESS - the operation was successful.
//
// Notes:
//  The operation is common for both read/write, except that it uses the "correct" queue
//  for each operation.
//
NTSTATUS OsrCommReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
  NTSTATUS status = STATUS_SUCCESS;
//  ULONG information = 0;
  POSR_COMM_DATA_REQUEST dataRequest;
  POSR_COMM_DATA_DEVICE_EXTENSION dataExt =
    (POSR_COMM_DATA_DEVICE_EXTENSION) DeviceObject->DeviceExtension;
  POSR_COMM_CONTROL_DEVICE_EXTENSION controlExt =
    (POSR_COMM_CONTROL_DEVICE_EXTENSION) OsrCommDeviceObject->DeviceExtension;
  PLIST_ENTRY listEntry;
  PIRP controlIrp;
  POSR_COMM_CONTROL_REQUEST controlRequest;
  BOOLEAN writeOp = IRP_MJ_WRITE == IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
  PFAST_MUTEX queueLock;
  PLIST_ENTRY queue;
  PMDL mdl;
  PVOID dataBuffer, controlBuffer;
  ULONG bytesToCopy;

  if (OSR_COMM_CONTROL_TYPE == DeviceObject->DeviceType) {

    //
    // Control device does not support read operations
    //
    status = STATUS_INVALID_DEVICE_REQUEST;

  }

  if (OSR_COMM_DATA_TYPE == DeviceObject->DeviceType) {

    //
    // Set the queue to use appropriately
    //
    if (writeOp) {

        DbgPrint("OsrCommReadWrite: Write Request Received.\n");

        queue = &dataExt->WriteRequestQueue;

        queueLock = &dataExt->WriteRequestQueueLock;

    } else {

        DbgPrint("OsrCommReadWrite: Read Request Received.\n");

        queue = &dataExt->ReadRequestQueue;

        queueLock = &dataExt->ReadRequestQueueLock;

    }

    //
    // If the device is enabled, enqueue the request
    //
    switch (dataExt->DeviceState) {

        case OSR_COMM_DATA_DEVICE_ACTIVE:
          //
          // Data device read must be satisfied by queuing request
          // off to the service.
          //
          dataRequest = (POSR_COMM_DATA_REQUEST) ExAllocatePoolWithTag(PagedPool, sizeof(OSR_COMM_DATA_REQUEST), 'rdCO');

          if (!dataRequest) {

            //
            // Complete the request, indicating that the operation failed
            //
            status = STATUS_INSUFFICIENT_RESOURCES;
        
            break;
          }
      
          RtlZeroMemory(dataRequest, sizeof(OSR_COMM_DATA_REQUEST));
      
          dataRequest->RequestID = (ULONG) InterlockedIncrement(&OsrRequestID);
      
          dataRequest->Irp = Irp;
      
          //
          // Since we are enqueuing the IRP, mark it pending
          //
          IoMarkIrpPending(Irp);
      
          status = STATUS_PENDING;

          //
          // Insert the request into the appropriate queue here
          //
          ExAcquireFastMutex(queueLock);
        
          InsertTailList(queue, &dataRequest->ListEntry);
        
          ExReleaseFastMutex(queueLock);
        
          //
          // Now, let's try to dispatch this to a service thread (really an IRP)
          // and if we cannot do so, we need to enqueue it for later processing
          // when a thread becomes available.
          //
          ExAcquireFastMutex(&controlExt->ServiceQueueLock);
      
          if (IsListEmpty(&controlExt->ServiceQueue)) {

            //
            // No waiting threads.  We need to insert this into the service request queue
            //
            ExAcquireFastMutex(&controlExt->RequestQueueLock);

            InsertTailList(&controlExt->RequestQueue, &dataRequest->ServiceListEntry);

            ExReleaseFastMutex(&controlExt->RequestQueueLock);
        
            //
            // Release the service queue lock
            //
            ExReleaseFastMutex(&controlExt->ServiceQueueLock);

          } else {

            //
            // A service thread is available right now.  Remove the service thread
            // from the queue.
            //
            listEntry = RemoveHeadList(&controlExt->ServiceQueue);
        
            controlIrp = CONTAINING_RECORD(listEntry, IRP, Tail.Overlay.ListEntry);

            //
            // Now build the request packet here.
            //
            controlRequest = (POSR_COMM_CONTROL_REQUEST) controlIrp->AssociatedIrp.SystemBuffer;

            controlRequest->RequestID = dataRequest->RequestID;

            DbgPrint("OsrCommReadWrite:  Irp %x, RequestBuffer %x RequestBufferLength %x\n.",
                     controlIrp,
                     controlRequest->RequestBuffer,
                     controlRequest->RequestBufferLength);


            if (writeOp) {

              controlRequest->RequestType = OSR_COMM_WRITE_REQUEST;

              //
              // Our problem here is that the control buffer is in a different
              // address space.  So, we need to reach over into that address space and
              // grab it.
              //
              mdl = IoAllocateMdl(controlRequest->RequestBuffer,
                                  controlRequest->RequestBufferLength,
                                  FALSE, // should not be any other MDLs associated with control IRP
                                  FALSE, // no quota charged
                                  controlIrp); // track the MDL in the control IRP...

              if (NULL == mdl) {
                //
                // We failed to get an MDL.  What a pain.
                //
                InsertTailList(&controlExt->ServiceQueue, listEntry);

                //
                // Complete the data request - this falls through and completes below.
                //
                status = STATUS_INSUFFICIENT_RESOURCES;

                //
                // Release the service queue lock
                //
                ExReleaseFastMutex(&controlExt->ServiceQueueLock);

                Irp->IoStatus.Status = status;
    
                Irp->IoStatus.Information = 0;
    
                IoCompleteRequest(Irp, IO_NO_INCREMENT);

                status = STATUS_PENDING;

                break;
            
              }

              __try {
                //
                // Probe and lock the pages
                //
                MmProbeAndLockProcessPages(mdl,
                                           IoGetRequestorProcess(controlIrp),
                                           UserMode,
                                           IoWriteAccess);

              } __except(EXCEPTION_EXECUTE_HANDLER) {

                //
                // Access probe failed
                //
                status = GetExceptionCode();

                //
                // Cleanup what we were doing....
                //

                IoFreeMdl(mdl);

                InsertTailList(&controlExt->ServiceQueue, listEntry);

                //
                // Release the service queue lock
                //
                ExReleaseFastMutex(&controlExt->ServiceQueueLock);

                Irp->IoStatus.Status = status;
    
                Irp->IoStatus.Information = 0;
    
                IoCompleteRequest(Irp, IO_NO_INCREMENT);

                status = STATUS_PENDING;

                break;

              }

              //
              // We now have an MDL we can use
              //
              dataBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);

              controlBuffer = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);

              if ((NULL == dataBuffer) || (NULL == controlBuffer)) {

                //
                // Not enough PTEs, obviously. Since we've modified the control IRP we need
                // to complete it here so we don't leave junk lying around.
                //
                controlIrp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

                controlIrp->IoStatus.Information = 0;

                IoCompleteRequest(controlIrp, IO_NO_INCREMENT);

                status = STATUS_INSUFFICIENT_RESOURCES;

                //
                // Release the service queue lock
                //
                ExReleaseFastMutex(&controlExt->ServiceQueueLock);

                //
                // Handle data irp failure below
                //
                break;

              }

              bytesToCopy = IoGetCurrentIrpStackLocation(Irp)->Parameters.Write.Length;

              //
              // Cannot copy more data than there is room to copy it...
              //
              if (controlRequest->RequestBufferLength < bytesToCopy) {
            
                bytesToCopy = controlRequest->RequestBufferLength;

              }

              //
              // OK.  We can copy data!  Yeah!
              //
              RtlCopyMemory(controlBuffer, dataBuffer, bytesToCopy);

              //
              // Complete the control IRP
              //
              controlRequest->RequestBufferLength = bytesToCopy;

              controlIrp->IoStatus.Status = STATUS_SUCCESS;

              controlIrp->IoStatus.Information = sizeof(OSR_COMM_CONTROL_REQUEST);

              IoCompleteRequest(controlIrp, IO_NO_INCREMENT);

              status = STATUS_PENDING;

			} else {
				//IS READ OPERATION

              controlRequest->RequestType = OSR_COMM_READ_REQUEST;

              controlRequest->RequestBuffer = Irp->AssociatedIrp.SystemBuffer;
          
              //
              // Note that length is in the same location for both read and write
              //
              controlRequest->RequestBufferLength = IoGetCurrentIrpStackLocation(Irp)->Parameters.Read.Length;
          
              //
              // And complete the control request
              //
              controlIrp->IoStatus.Status = STATUS_SUCCESS;
          
              controlIrp->IoStatus.Information = sizeof(OSR_COMM_CONTROL_REQUEST);
          
              IoCompleteRequest(controlIrp, IO_NO_INCREMENT);
          
              status = STATUS_PENDING;

            }
        
            //
            // Release the service queue lock
            //
            ExReleaseFastMutex(&controlExt->ServiceQueueLock);
        
          }
          break;
        
        default:
          status = STATUS_INVALID_DEVICE_REQUEST;
      
    }

  }

  //
  // If the status is not STATUS_PENDING, complete the request
  //
  if (STATUS_PENDING != status) {
    
    //
    // Set status
    //
    Irp->IoStatus.Status = status;
    
    Irp->IoStatus.Information = 0;
    
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
  }

  //
  // Done.
  //
  return status;

}
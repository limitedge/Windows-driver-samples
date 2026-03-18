/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

Module Name:

    usermode_accessors_sample.c

Abstract:

    KMDF sample driver demonstrating safe kernel-to-user and user-to-kernel
    memory access using the usermode_accessors.h API family.

    All usermode_accessors functions raise SEH exceptions on invalid access
    rather than returning error codes. Callers must wrap them in __try/__except
    and use UmaExceptionFilter() as the exception filter.

    API patterns demonstrated:
      - ReadXxxFromUser   : returns value directly, 1 param (source pointer)
      - WriteXxxToUser    : void return, 2 params (dest pointer, value)
      - CopyFromUser/To   : void return, 3 params (dest, src, length)
      - FillUserMemory    : void return, 3 params (dest, length, fill)
      - StringLengthFromUser / WideStringLengthFromUser : returns SIZE_T, 1 param
      - InterlockedXxxToUser : returns previous value (LONG/LONG64)
      - ReadStructFromUser / WriteStructToUser : macros (statement-only)
      - ReadULongFromMode / WriteULongToMode / CopyFromMode : mode-aware variants
      - UmaExceptionFilter : takes only KPROCESSOR_MODE

Environment:

    Kernel mode

--*/

#include "usermode_accessors_sample.h"

//
// Forward declarations for IOCTL handlers
//
static NTSTATUS HandleReadValues(_In_ WDFREQUEST Request);
static NTSTATUS HandleWriteValues(_In_ WDFREQUEST Request);
static NTSTATUS HandleCopyBuffer(_In_ WDFREQUEST Request);
static NTSTATUS HandleFillBuffer(_In_ WDFREQUEST Request);
static NTSTATUS HandleInterlockedOps(_In_ WDFREQUEST Request);
static NTSTATUS HandleStringLength(_In_ WDFREQUEST Request);
static NTSTATUS HandleStructAccess(_In_ WDFREQUEST Request);
static NTSTATUS HandleModeOperations(_In_ WDFREQUEST Request);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, EvtDeviceAdd)
#pragma alloc_text(PAGE, EvtIoDeviceControl)
#endif

// -----------------------------------------------------------------------
// DriverEntry
// -----------------------------------------------------------------------
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
        );

    return status;
}

// -----------------------------------------------------------------------
// EvtDeviceAdd
// -----------------------------------------------------------------------
NTSTATUS
EvtDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFQUEUE queue;
    WDF_IO_QUEUE_CONFIG queueConfig;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Create a default parallel queue for IOCTLs.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Create a device interface so user-mode apps can open the device.
    //
    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_UMA_SAMPLE,
        NULL
        );

    return status;
}

// -----------------------------------------------------------------------
// Helper: extract METHOD_NEITHER buffer pointers from IRP
// -----------------------------------------------------------------------
static
VOID
GetNeitherBuffers(
    _In_ WDFREQUEST Request,
    _Out_ PVOID *InputBuffer,
    _Out_ ULONG *InputLength,
    _Out_ PVOID *OutputBuffer,
    _Out_ ULONG *OutputLength
    )
{
    PIRP irp = WdfRequestWdmGetIrp(Request);
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

    *InputBuffer  = irpSp->Parameters.DeviceIoControl.Type3InputBuffer;
    *InputLength  = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    *OutputBuffer = irp->UserBuffer;
    *OutputLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
}

// -----------------------------------------------------------------------
// EvtIoDeviceControl - dispatch IOCTLs to handlers
// -----------------------------------------------------------------------
VOID
EvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    NTSTATUS status;
    ULONG_PTR bytesReturned = 0;
    PDEVICE_CONTEXT devCtx;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    devCtx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    devCtx->OperationCount++;

    switch (IoControlCode) {

    case IOCTL_UMA_READ_VALUES:
        status = HandleReadValues(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(UMA_READ_VALUES_OUTPUT);
        }
        break;

    case IOCTL_UMA_WRITE_VALUES:
        status = HandleWriteValues(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(UMA_WRITE_VALUES_INPUT);
        }
        break;

    case IOCTL_UMA_COPY_BUFFER:
        status = HandleCopyBuffer(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = (ULONG_PTR)OutputBufferLength;
        }
        break;

    case IOCTL_UMA_FILL_BUFFER:
        status = HandleFillBuffer(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = (ULONG_PTR)OutputBufferLength;
        }
        break;

    case IOCTL_UMA_INTERLOCKED_OPS:
        status = HandleInterlockedOps(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(UMA_INTERLOCKED_OUTPUT);
        }
        break;

    case IOCTL_UMA_STRING_LENGTH:
        status = HandleStringLength(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(UMA_STRING_LENGTH_OUTPUT);
        }
        break;

    case IOCTL_UMA_STRUCT_ACCESS:
        status = HandleStructAccess(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(UMA_SAMPLE_STRUCT);
        }
        break;

    case IOCTL_UMA_MODE_OPERATIONS:
        status = HandleModeOperations(Request);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(ULONG);
        }
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

// -----------------------------------------------------------------------
// IOCTL_UMA_READ_VALUES
//
// Demonstrates: ReadUCharFromUser, ReadUShortFromUser, ReadULongFromUser,
//   ReadULong64FromUser, ReadBooleanFromUser
//   WriteUCharToUser, WriteUShortToUser, WriteULongToUser,
//   WriteULong64ToUser, WriteBooleanToUserRelease
//
// ReadXxxFromUser(ptr) -> returns value directly. Single parameter.
// WriteXxxToUser(ptr, value) -> void. Two parameters.
// -----------------------------------------------------------------------
static
NTSTATUS
HandleReadValues(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    PUMA_READ_VALUES_INPUT userInput;
    PUMA_READ_VALUES_OUTPUT userOutput;
    UCHAR ucharVal;
    USHORT ushortVal;
    ULONG ulongVal;
    ULONG64 ulong64Val;
    BOOLEAN boolVal;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_READ_VALUES_INPUT) ||
        outputLength < sizeof(UMA_READ_VALUES_OUTPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    userInput = (PUMA_READ_VALUES_INPUT)inputBuffer;
    userOutput = (PUMA_READ_VALUES_OUTPUT)outputBuffer;

    __try {
        //
        // ReadXxxFromUser: takes a single pointer to user-mode memory,
        // returns the value at that location. Raises SEH on invalid access.
        //
        ucharVal   = ReadUCharFromUser(&userInput->UCharValue);
        ushortVal  = ReadUShortFromUser(&userInput->UShortValue);
        ulongVal   = ReadULongFromUser(&userInput->ULongValue);
        ulong64Val = ReadULong64FromUser(&userInput->ULong64Value);
        boolVal    = ReadBooleanFromUser(&userInput->BoolValue);

        //
        // WriteXxxToUser: takes a destination pointer and a value,
        // writes the value to user-mode memory. Void return.
        //
        WriteUCharToUser(&userOutput->UCharValue, ucharVal);
        WriteUShortToUser(&userOutput->UShortValue, ushortVal);
        WriteULongToUser(&userOutput->ULongValue, ulongVal);
        WriteULong64ToUser(&userOutput->ULong64Value, ulong64Val);
        WriteBooleanToUserRelease(&userOutput->BoolValue, boolVal);
        WriteULongToUser((volatile ULONG *)&userOutput->StatusResult, (ULONG)STATUS_SUCCESS);

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_WRITE_VALUES
//
// Demonstrates: WriteUCharToUser, WriteUShortToUser, WriteULongToUser,
//   WriteULong64ToUser, WriteBooleanToUserRelease
//
// WriteXxxToUser(destPtr, value) -> void. Raises SEH on failure.
// -----------------------------------------------------------------------
static
NTSTATUS
HandleWriteValues(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    PUMA_WRITE_VALUES_INPUT userInput;
    PUMA_WRITE_VALUES_INPUT userOutput;
    UCHAR ucharVal;
    USHORT ushortVal;
    ULONG ulongVal;
    ULONG64 ulong64Val;
    BOOLEAN boolVal;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_WRITE_VALUES_INPUT) ||
        outputLength < sizeof(UMA_WRITE_VALUES_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    userInput = (PUMA_WRITE_VALUES_INPUT)inputBuffer;
    userOutput = (PUMA_WRITE_VALUES_INPUT)outputBuffer;

    __try {
        //
        // Read values from user input buffer.
        //
        ucharVal   = ReadUCharFromUser(&userInput->UCharValue);
        ushortVal  = ReadUShortFromUser(&userInput->UShortValue);
        ulongVal   = ReadULongFromUser(&userInput->ULongValue);
        ulong64Val = ReadULong64FromUser(&userInput->ULong64Value);
        boolVal    = ReadBooleanFromUser(&userInput->BoolValue);

        //
        // Write each value to the output buffer using WriteXxxToUser.
        //
        WriteUCharToUser(&userOutput->UCharValue, ucharVal);
        WriteUShortToUser(&userOutput->UShortValue, ushortVal);
        WriteULongToUser(&userOutput->ULongValue, ulongVal);
        WriteULong64ToUser(&userOutput->ULong64Value, ulong64Val);
        WriteBooleanToUserRelease(&userOutput->BoolValue, boolVal);

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_COPY_BUFFER
//
// Demonstrates: CopyFromUser, CopyToUser
//
// CopyFromUser(kernelDest, userSrc, length) -> void
// CopyToUser(userDest, kernelSrc, length)   -> void
// -----------------------------------------------------------------------
static
NTSTATUS
HandleCopyBuffer(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    PVOID kernelBuffer = NULL;
    SIZE_T copyLength;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    copyLength = min(inputLength, outputLength);
    if (copyLength == 0) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // Allocate a kernel-mode intermediate buffer to demonstrate
    // the copy-from-user then copy-to-user pattern.
    //
    kernelBuffer = ExAllocatePool2(POOL_FLAG_PAGED, copyLength, UMA_POOL_TAG);
    if (kernelBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {
        //
        // CopyFromUser: copy from user-mode source into kernel buffer.
        // Signature: VOID CopyFromUser(volatile VOID* Dest, volatile const VOID* Src, SIZE_T Len)
        //
        CopyFromUser(kernelBuffer, inputBuffer, copyLength);

        //
        // CopyToUser: copy from kernel buffer to user-mode destination.
        // Signature: VOID CopyToUser(volatile VOID* Dest, const VOID* Src, SIZE_T Len)
        //
        CopyToUser(outputBuffer, kernelBuffer, copyLength);

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    ExFreePoolWithTag(kernelBuffer, UMA_POOL_TAG);
    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_FILL_BUFFER
//
// Demonstrates: FillUserMemory
//
// FillUserMemory(dest, length, fillByte) -> void
// -----------------------------------------------------------------------
static
NTSTATUS
HandleFillBuffer(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    UMA_FILL_INPUT localFillInput;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_FILL_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // First, safely read the fill parameters from user-mode input.
    //
    __try {
        CopyFromUser(&localFillInput, inputBuffer, sizeof(UMA_FILL_INPUT));
    } __except (UmaExceptionFilter(UserMode)) {
        return GetExceptionCode();
    }

    if (localFillInput.Length == 0 || localFillInput.Length > outputLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // Fill the user-mode output buffer with the specified byte value.
    // Signature: VOID FillUserMemory(volatile VOID* Dest, SIZE_T Length, UCHAR Fill)
    //
    __try {
        FillUserMemory(outputBuffer, localFillInput.Length, localFillInput.FillValue);
    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_INTERLOCKED_OPS
//
// Demonstrates: InterlockedAndToUser, InterlockedOrToUser,
//   InterlockedCompareExchangeToUser, InterlockedAnd64ToUser,
//   InterlockedOr64ToUser, InterlockedCompareExchange64ToUser
//
// These return the original (previous) value as LONG or LONG64.
// -----------------------------------------------------------------------
static
NTSTATUS
HandleInterlockedOps(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    UMA_INTERLOCKED_INPUT localInput;
    UMA_INTERLOCKED_OUTPUT localOutput;
    PUMA_INTERLOCKED_INPUT userInput;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_INTERLOCKED_INPUT) ||
        outputLength < sizeof(UMA_INTERLOCKED_OUTPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    userInput = (PUMA_INTERLOCKED_INPUT)inputBuffer;

    //
    // Copy input to local kernel memory so we can safely read the operands.
    //
    __try {
        CopyFromUser(&localInput, inputBuffer, sizeof(UMA_INTERLOCKED_INPUT));
    } __except (UmaExceptionFilter(UserMode)) {
        return GetExceptionCode();
    }

    __try {
        //
        // 32-bit interlocked operations on user-mode memory.
        // Each returns the previous value before the operation.
        //
        localOutput.AndResult32 = InterlockedAndToUser(
            &userInput->Value32, localInput.Operand32);

        localOutput.OrResult32 = InterlockedOrToUser(
            &userInput->Value32, localInput.Operand32);

        localOutput.CmpXchgResult32 = InterlockedCompareExchangeToUser(
            &userInput->Value32, localInput.Operand32, localInput.Operand32);

        //
        // 64-bit interlocked operations on user-mode memory.
        //
        localOutput.AndResult64 = InterlockedAnd64ToUser(
            &userInput->Value64, localInput.Operand64);

        localOutput.OrResult64 = InterlockedOr64ToUser(
            &userInput->Value64, localInput.Operand64);

        localOutput.CmpXchgResult64 = InterlockedCompareExchange64ToUser(
            &userInput->Value64, localInput.Operand64, localInput.Operand64);

        //
        // Write results to user-mode output buffer.
        //
        CopyToUser(outputBuffer, &localOutput, sizeof(UMA_INTERLOCKED_OUTPUT));

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_STRING_LENGTH
//
// Demonstrates: StringLengthFromUser, WideStringLengthFromUser
//
// StringLengthFromUser(stringPtr)      -> returns SIZE_T (1 param)
// WideStringLengthFromUser(stringPtr)  -> returns SIZE_T (1 param)
// -----------------------------------------------------------------------
static
NTSTATUS
HandleStringLength(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    PUMA_STRING_INPUT userInput;
    UMA_STRING_LENGTH_OUTPUT localOutput;
    SIZE_T ansiLen;
    SIZE_T wideLen;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_STRING_INPUT) ||
        outputLength < sizeof(UMA_STRING_LENGTH_OUTPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    userInput = (PUMA_STRING_INPUT)inputBuffer;

    __try {
        //
        // StringLengthFromUser: takes a single pointer to a user-mode
        // null-terminated ANSI string, returns the length in characters.
        //
        ansiLen = StringLengthFromUser(userInput->AnsiString);

        //
        // WideStringLengthFromUser: same for wide (WCHAR) strings.
        //
        wideLen = WideStringLengthFromUser(userInput->WideString);

        localOutput.AnsiLength = ansiLen;
        localOutput.WideLength = wideLen;

        //
        // Write the result to user-mode output.
        //
        CopyToUser(outputBuffer, &localOutput, sizeof(UMA_STRING_LENGTH_OUTPUT));

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_STRUCT_ACCESS
//
// Demonstrates: ReadStructFromUser, WriteStructToUser (macros)
//
// These are macros that expand to do { ... } while(0) statements.
// They CANNOT be used as expressions. Use as standalone statements only.
//
// ReadStructFromUser(kernelDest, userSrc)   - copies user struct to kernel
// WriteStructToUser(userDest, kernelSrc)    - copies kernel struct to user
// -----------------------------------------------------------------------
static
NTSTATUS
HandleStructAccess(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    UMA_SAMPLE_STRUCT localStruct;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_SAMPLE_STRUCT) ||
        outputLength < sizeof(UMA_SAMPLE_STRUCT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    __try {
        //
        // ReadStructFromUser: macro that copies a user-mode struct to a local copy.
        // Expands to do { CopyFromUser(dst, src, sizeof(*dst)); } while(0)
        //
        ReadStructFromUser(&localStruct, (PUMA_SAMPLE_STRUCT)inputBuffer);

        //
        // Modify the struct in kernel mode to prove we read and can write back.
        //
        localStruct.Id += 1;
        localStruct.Timestamp += 100;

        //
        // WriteStructToUser: macro that copies a kernel struct to user-mode memory.
        // Expands to do { CopyToUser(dst, src, sizeof(*dst)); } while(0)
        //
        WriteStructToUser((PUMA_SAMPLE_STRUCT)outputBuffer, &localStruct);

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

// -----------------------------------------------------------------------
// IOCTL_UMA_MODE_OPERATIONS
//
// Demonstrates: ReadULongFromMode, WriteULongToMode, CopyFromMode
//
// ReadULongFromMode(srcPtr, Mode)             -> returns ULONG
// WriteULongToMode(destPtr, value, Mode)      -> void (value before Mode)
// CopyFromMode(dest, src, length, Mode)       -> void
// -----------------------------------------------------------------------
static
NTSTATUS
HandleModeOperations(
    _In_ WDFREQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID inputBuffer;
    ULONG inputLength;
    PVOID outputBuffer;
    ULONG outputLength;
    UMA_MODE_INPUT localModeInput;
    ULONG readValue;

    GetNeitherBuffers(Request, &inputBuffer, &inputLength, &outputBuffer, &outputLength);

    if (inputLength < sizeof(UMA_MODE_INPUT) ||
        outputLength < sizeof(ULONG)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //
    // Read input using CopyFromMode to demonstrate the mode-aware copy.
    // CopyFromMode(dest, src, length, Mode) -> void
    //
    __try {
        CopyFromMode(
            &localModeInput,
            inputBuffer,
            sizeof(UMA_MODE_INPUT),
            UserMode
            );
    } __except (UmaExceptionFilter(UserMode)) {
        return GetExceptionCode();
    }

    __try {
        //
        // ReadULongFromMode: read a ULONG from user-mode memory with explicit mode.
        // Signature: ULONG ReadULongFromMode(const volatile ULONG* Source, KPROCESSOR_MODE Mode)
        //
        readValue = ReadULongFromMode(
            (const volatile ULONG *)&((PUMA_MODE_INPUT)inputBuffer)->Value,
            UserMode
            );

        //
        // WriteULongToMode: write a ULONG to user-mode memory with explicit mode.
        // Signature: VOID WriteULongToMode(volatile ULONG* Dest, ULONG Value, KPROCESSOR_MODE Mode)
        // Note: value parameter comes before mode parameter.
        //
        WriteULongToMode(
            (volatile ULONG *)outputBuffer,
            readValue,
            UserMode
            );

    } __except (UmaExceptionFilter(UserMode)) {
        status = GetExceptionCode();
    }

    return status;
}

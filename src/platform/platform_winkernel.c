/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC Platform Abstraction Layer.

Environment:

    Windows Kernel Mode

--*/

#include "platform_internal.h"

#ifdef QUIC_LOGS_WPP
#include "platform_winkernel.tmh"
#pragma warning(push) // Don't care about OACR warnings in publics
#pragma warning(disable:28170)
#include <fastwppimpl.h>
#pragma warning(pop)
#endif

/*
    This multiline comment forces WPP to generate the definitions for the
    initialization and cleanup code, which happens only if there is direct
    textual reference to the "WPP_INIT_TRACING();" macro.
*/

typedef enum _SYSTEM_INFORMATION_CLASS {
    SystemBasicInformation                          = 0
} SYSTEM_INFORMATION_CLASS;

NTSYSAPI // Copied from zwapi.h.
NTSTATUS
NTAPI
ZwQuerySystemInformation (
    __in SYSTEM_INFORMATION_CLASS SystemInformationClass,
    __out_bcount_part_opt(SystemInformationLength, *ReturnLength) PVOID SystemInformation,
    __in ULONG SystemInformationLength,
    __out_opt PULONG ReturnLength
    );

typedef struct _SYSTEM_BASIC_INFORMATION {
    ULONG Reserved;
    ULONG TimerResolution;
    ULONG PageSize;

    //
    // WARNING: The following fields are 32-bit and may get
    // capped to MAXULONG on systems with a lot of RAM!
    //
    // Use SYSTEM_PHYSICAL_MEMORY_INFORMATION instead.
    //

    ULONG NumberOfPhysicalPages;      // Deprecated, do not use.
    ULONG LowestPhysicalPageNumber;   // Deprecated, do not use.
    ULONG HighestPhysicalPageNumber;  // Deprecated, do not use.

    ULONG AllocationGranularity;
    ULONG_PTR MinimumUserModeAddress;
    ULONG_PTR MaximumUserModeAddress;
    ULONG_PTR ActiveProcessorsAffinityMask;
    CCHAR NumberOfProcessors;
} SYSTEM_BASIC_INFORMATION, *PSYSTEM_BASIC_INFORMATION;


uint64_t QuicPlatformPerfFreq;
uint64_t QuicTotalMemory;
QUIC_PLATFORM QuicPlatform = { NULL, NULL };

INITCODE
_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicPlatformSystemLoad(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
#ifdef QUIC_LOGS_WPP
    FAST_WPP_INIT_TRACING(DriverObject, RegistryPath);
#endif

    EventRegisterMicrosoft_Quic();

    InitializeTelemetryAssertsKM(RegistryPath);

    QuicPlatform.DriverObject = DriverObject;
    (VOID)KeQueryPerformanceCounter((LARGE_INTEGER*)&QuicPlatformPerfFreq);
    QuicPlatform.RngAlgorithm = NULL;

    QuicTraceLogInfo("[ sys] Loaded");
}

PAGEDX
_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicPlatformSystemUnload(
    void
    )
{
    PAGED_CODE();
    QuicTraceLogInfo("[ sys] Unloaded");
    UninitializeTelemetryAssertsKM();
    EventUnregisterMicrosoft_Quic();

#ifdef QUIC_LOGS_WPP
    FAST_WPP_CLEANUP(QuicPlatform.DriverObject);
#endif
}

PAGEDX
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicPlatformInitialize(
    void
    )
{
    SYSTEM_BASIC_INFORMATION Sbi;

    PAGED_CODE();

    QUIC_STATUS Status =
        BCryptOpenAlgorithmProvider(
            &QuicPlatform.RngAlgorithm,
            BCRYPT_RNG_ALGORITHM,
            NULL,
            BCRYPT_PROV_DISPATCH);
    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(LibraryErrorStatus, Status, "BCryptOpenAlgorithmProvider (RNG)");
        goto Error;
    }
    QUIC_DBG_ASSERT(QuicPlatform.RngAlgorithm != NULL);

    Status =
        ZwQuerySystemInformation(
            SystemBasicInformation, &Sbi, sizeof(Sbi), NULL);
    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(LibraryErrorStatus, Status, "ZwQuerySystemInformation(SystemBasicInformation)");
        goto Error;
    }

    Status = QuicTlsLibraryInitialize();
    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(LibraryErrorStatus, Status, "QuicTlsLibraryInitialize");
        goto Error;
    }

    //
    // TODO - Apparently this can be increased via hot memory add. Figure out
    // how to know when to update this value.
    //
    QuicTotalMemory = (uint64_t)Sbi.NumberOfPhysicalPages * (uint64_t)Sbi.PageSize;

    QuicTraceLogInfo("[ sys] Initialized (PageSize = %u bytes; AvailMem = %llu bytes)",
        Sbi.PageSize, QuicTotalMemory);

Error:

    if (QUIC_FAILED(Status)) {
        if (QuicPlatform.RngAlgorithm != NULL) {
            BCryptCloseAlgorithmProvider(QuicPlatform.RngAlgorithm, 0);
            QuicPlatform.RngAlgorithm = NULL;
        }
    }

    return Status;
}

PAGEDX
_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicPlatformUninitialize(
    void
    )
{
    PAGED_CODE();
    QuicTlsLibraryUninitialize();
    BCryptCloseAlgorithmProvider(QuicPlatform.RngAlgorithm, 0);
    QuicPlatform.RngAlgorithm = NULL;
    QuicTraceLogInfo("[ sys] Uninitialized");
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicPlatformLogAssert(
    _In_z_ const char* File,
    _In_ int Line,
    _In_z_ const char* Expr
    )
{
    QuicTraceEvent(LibraryAssert, (uint32_t)Line, File, Expr);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicRandom(
    _In_ uint32_t BufferLen,
    _Out_writes_bytes_(BufferLen) void* Buffer
    )
{
    //
    // Use the algorithm we initialized for DISPATCH_LEVEL usage.
    //
    QUIC_DBG_ASSERT(QuicPlatform.RngAlgorithm != NULL);
    return (QUIC_STATUS)
        BCryptGenRandom(
            QuicPlatform.RngAlgorithm,
            (uint8_t*)Buffer,
            BufferLen,
            0);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_IRQL_requires_same_
void
NTAPI
QuicEtwCallback(
    _In_ LPCGUID SourceId,
    _In_ ULONG ControlCode,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAnyKeyword,
    _In_ ULONGLONG MatchAllKeyword,
    _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData,
    _Inout_opt_ PVOID CallbackContext
    )
{
    UNREFERENCED_PARAMETER(SourceId);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(MatchAnyKeyword);
    UNREFERENCED_PARAMETER(MatchAllKeyword);
    UNREFERENCED_PARAMETER(FilterData);

    switch(ControlCode) {
    case EVENT_CONTROL_CODE_ENABLE_PROVIDER:
    case EVENT_CONTROL_CODE_CAPTURE_STATE:
        if (CallbackContext == &MICROSOFT_MSQUIC_PROVIDER_Context) {
            QuicTraceRundown();
        }
        break;
    case EVENT_CONTROL_CODE_DISABLE_PROVIDER:
    default:
        break;
    }
}
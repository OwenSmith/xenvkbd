/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documetation and/or other
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include <debug_interface.h>
#include <store_interface.h>
#include <cache_interface.h>
#include <gnttab_interface.h>
#include <range_set_interface.h>
#include <evtchn_interface.h>

#include "pdo.h"
#include "frontend.h"
#include "ring.h"
#include "hid.h"
#include "thread.h"
#include "registry.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define MAXNAMELEN  128

struct _XENVKBD_RING {
    PXENVKBD_FRONTEND       Frontend;
    PXENVKBD_HID_CONTEXT    Hid;

    XENBUS_DEBUG_INTERFACE  DebugInterface;
    XENBUS_STORE_INTERFACE  StoreInterface;
    XENBUS_GNTTAB_INTERFACE GnttabInterface;
    XENBUS_EVTCHN_INTERFACE EvtchnInterface;

    PXENBUS_DEBUG_CALLBACK  DebugCallback;

    PXENBUS_GNTTAB_CACHE    GnttabCache;
    PMDL                    Mdl;
    struct xenkbd_page      *Shared;
    PXENBUS_GNTTAB_ENTRY    Entry;
    PXENBUS_EVTCHN_CHANNEL  Channel;
    KSPIN_LOCK              Lock;
    KDPC                    Dpc;
    ULONG                   Dpcs;
    ULONG                   Events;
    BOOLEAN                 Connected;
    BOOLEAN                 Enabled;
    BOOLEAN                 AbsPointer;
};

#define XENVKBD_RING_TAG    'gniR'

static FORCEINLINE PVOID
__RingAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENVKBD_RING_TAG);
}

static FORCEINLINE VOID
__RingFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, XENVKBD_RING_TAG);
}

__drv_functionClass(KDEFERRED_ROUTINE)
__drv_maxIRQL(DISPATCH_LEVEL)
__drv_minIRQL(DISPATCH_LEVEL)
__drv_requiresIRQL(DISPATCH_LEVEL)
__drv_sameIRQL
static VOID
RingDpc(
    IN  PKDPC       Dpc,
    IN  PVOID       Context,
    IN  PVOID       Argument1,
    IN  PVOID       Argument2
    )
{
    PXENVKBD_RING   Ring = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT(Ring != NULL);

    for (;;) {
        ULONG   in_cons;
        ULONG   in_prod;

        KeMemoryBarrier();

        in_cons = Ring->Shared->in_cons;
        in_prod = Ring->Shared->in_prod;

        KeMemoryBarrier();

        if (in_cons == in_prod)
            break;

        while (in_cons != in_prod) {
            union xenkbd_in_event *in_evt;

            in_evt = &XENKBD_IN_RING_REF(Ring->Shared, in_cons);
            ++in_cons;

            switch (in_evt->type) {
            case XENKBD_TYPE_MOTION:
                HidEventMotion(Ring->Hid,
                               in_evt->motion.rel_x,
                               in_evt->motion.rel_y,
                               in_evt->motion.rel_z);
                break;
            case XENKBD_TYPE_KEY:
                HidEventKeypress(Ring->Hid,
                                 in_evt->key.keycode,
                                 in_evt->key.pressed);                        
                break;
            case XENKBD_TYPE_POS:
                HidEventPosition(Ring->Hid,
                                 in_evt->pos.abs_x,
                                 in_evt->pos.abs_y,
                                 in_evt->pos.rel_z);
                break;
            case XENKBD_TYPE_MTOUCH:
                Trace("MTOUCH: %u %u %u %u\n",
                     in_evt->mtouch.event_type,
                     in_evt->mtouch.contact_id,
                     in_evt->mtouch.u.pos.abs_x,
                     in_evt->mtouch.u.pos.abs_y);
                // call Frontend
                break;
            default:
                Trace("UNKNOWN: %u\n",
                      in_evt->type);
                break;
            }
        }

        KeMemoryBarrier();

        Ring->Shared->in_cons = in_cons;
    }

    XENBUS_EVTCHN(Unmask,
                  &Ring->EvtchnInterface,
                  Ring->Channel,
                  FALSE);
}

static VOID
RingAcquireLock(
    IN  PVOID       Context
    )
{
    PXENVKBD_RING   Ring = Context;
    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
}

static VOID
RingReleaseLock(
    IN  PVOID       Context
    )
{
    PXENVKBD_RING   Ring = Context;
#pragma warning(suppress:26110)
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);
}

KSERVICE_ROUTINE    RingEvtchnCallback;

BOOLEAN
RingEvtchnCallback(
    IN  PKINTERRUPT     InterruptObject,
    IN  PVOID           Argument
    )
{
    PXENVKBD_RING       Ring = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Ring != NULL);
    Ring->Events++;

    if (KeInsertQueueDpc(&Ring->Dpc, NULL, NULL))
        Ring->Dpcs++;

    return TRUE;
}

static VOID
RingDebugCallback(
    IN  PVOID           Argument,
    IN  BOOLEAN         Crashing
    )
{
    PXENVKBD_RING       Ring = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "0x%p [%s]\n",
                 Ring,
                 (Ring->Enabled) ? "ENABLED" : "DISABLED");
}

NTSTATUS
RingInitialize(
    IN  PXENVKBD_FRONTEND   Frontend,
    OUT PXENVKBD_RING       *Ring
    )
{
    NTSTATUS                status;

    Trace("=====>\n");
    status = STATUS_NO_MEMORY;
    *Ring = __RingAllocate(sizeof(XENVKBD_RING));
    if (*Ring == NULL)
        goto fail1;

    (*Ring)->Frontend = Frontend;
    (*Ring)->Hid = PdoGetHidContext(FrontendGetPdo(Frontend));
    KeInitializeDpc(&(*Ring)->Dpc, RingDpc, *Ring);
    KeInitializeSpinLock(&(*Ring)->Lock);

    FdoGetDebugInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->DebugInterface);

    FdoGetStoreInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->StoreInterface);

    FdoGetGnttabInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->GnttabInterface);

    FdoGetEvtchnInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->EvtchnInterface);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static FORCEINLINE VOID
RingReadFeatures(
    IN  PXENVKBD_RING   Ring
    )
{
    PCHAR               Buffer;
    NTSTATUS            status;

    status = XENBUS_STORE(Read,
                          &Ring->StoreInterface,
                          NULL,
                          FrontendGetBackendPath(Ring->Frontend),
                          "feature-abs-pointer",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Ring->AbsPointer = (BOOLEAN)strtoul(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Ring->StoreInterface,
                     Buffer);
    } else {
        Ring->AbsPointer = FALSE;
    }
}

NTSTATUS
RingConnect(
    IN  PXENVKBD_RING   Ring
    )
{
    PFN_NUMBER          Pfn;
    PXENVKBD_FRONTEND   Frontend;
    NTSTATUS            status;

    Trace("=====>\n");
    Frontend = Ring->Frontend;

    status = XENBUS_DEBUG(Acquire, &Ring->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_STORE(Acquire, &Ring->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_EVTCHN(Acquire, &Ring->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_GNTTAB(Acquire, &Ring->GnttabInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_GNTTAB(CreateCache,
                           &Ring->GnttabInterface,
                           "VKBD_Ring_Gnttab",
                           0,
                           RingAcquireLock,
                           RingReleaseLock,
                           Ring,
                           &Ring->GnttabCache);
    if (!NT_SUCCESS(status))
        goto fail5;

    RingReadFeatures(Ring);

    Ring->Mdl = __AllocatePage();
    
    status = STATUS_NO_MEMORY;
    if (Ring->Mdl == NULL)
        goto fail6;

    ASSERT(Ring->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    Ring->Shared = Ring->Mdl->MappedSystemVa;
    ASSERT(Ring->Shared != NULL);

    ASSERT(Ring->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    RtlZeroMemory(Ring->Mdl->MappedSystemVa, PAGE_SIZE);

    Pfn = MmGetMdlPfnArray(Ring->Mdl)[0];

    status = XENBUS_GNTTAB(PermitForeignAccess,
                           &Ring->GnttabInterface,
                           Ring->GnttabCache,
                           TRUE,
                           FrontendGetBackendDomain(Frontend),
                           Pfn,
                           FALSE,
                           &Ring->Entry);
    if (!NT_SUCCESS(status))
        goto fail7;

    Ring->Channel = XENBUS_EVTCHN(Open,
                                  &Ring->EvtchnInterface,
                                  XENBUS_EVTCHN_TYPE_UNBOUND,
                                  RingEvtchnCallback,
                                  Ring,
                                  FrontendGetBackendDomain(Frontend),
                                  TRUE);

    status = STATUS_UNSUCCESSFUL;
    if (Ring->Channel == NULL)
        goto fail8;

    XENBUS_EVTCHN(Unmask,
                  &Ring->EvtchnInterface,
                  Ring->Channel,
                  FALSE);

    status = XENBUS_DEBUG(Register,
                          &Ring->DebugInterface,
                          __MODULE__ "|RING",
                          RingDebugCallback,
                          Ring,
                          &Ring->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail9;

    Ring->Connected = TRUE;
    return STATUS_SUCCESS;

fail9:
    Error("fail9\n");

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

    Ring->Events = 0;

fail8:
    Error("fail8\n");

    (VOID) XENBUS_GNTTAB(RevokeForeignAccess,
                         &Ring->GnttabInterface,
                         Ring->GnttabCache,
                         TRUE,
                         Ring->Entry);
    Ring->Entry = NULL;

fail7:
    Error("fail7\n");

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

fail6:
    Error("fail6\n");

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

fail5:
    Error("fail5\n");

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);

fail4:
    Error("fail4\n");

    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);

fail3:
    Error("fail3\n");

    XENBUS_STORE(Release, &Ring->StoreInterface);

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Ring->DebugInterface);

fail1:
    Error("fail1 %08x\n", status);

    return status;
}

NTSTATUS
RingStoreWrite(
    IN  PXENVKBD_RING               Ring,
    IN  PXENBUS_STORE_TRANSACTION   Transaction
    )
{
    ULONG                           Port;
    NTSTATUS                        status;

    Trace("=====>\n");
    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "page-gref",
                          "%u",
                          XENBUS_GNTTAB(GetReference,
                                        &Ring->GnttabInterface,
                                        Ring->Entry));
    if (!NT_SUCCESS(status))
        goto fail1;

    // this should not be required - QEMU should use grant references
    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "page-ref",
                          "%llu",
                          (ULONG64)MmGetMdlPfnArray(Ring->Mdl)[0]);
    if (!NT_SUCCESS(status))
        goto fail2;

    Port = XENBUS_EVTCHN(GetPort,
                         &Ring->EvtchnInterface,
                         Ring->Channel);

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "event-channel",
                          "%u",
                          Port);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "request-abs-pointer",
                          "%u",
                          Ring->AbsPointer);
    if (!NT_SUCCESS(status))
        goto fail4;

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");
fail3:
    Error("fail3\n");
fail2:
    Error("fail2\n");
fail1:
    Error("fail1 %08x\n", status);
    return status;
}

NTSTATUS
RingEnable(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");

    ASSERT(!Ring->Enabled);
    Ring->Enabled = TRUE;

    KeInsertQueueDpc(&Ring->Dpc, NULL, NULL);

    Trace("<=====\n");
    return STATUS_SUCCESS;
}

VOID
RingDisable(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");

    ASSERT(Ring->Enabled);
    Ring->Enabled = FALSE;

    Trace("<=====\n");
}

VOID
RingDisconnect(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");

    XENBUS_DEBUG(Deregister,
                 &Ring->DebugInterface,
                 Ring->DebugCallback);
    Ring->DebugCallback = NULL;

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

    Ring->Events = 0;

    (VOID) XENBUS_GNTTAB(RevokeForeignAccess,
                         &Ring->GnttabInterface,
                         Ring->GnttabCache,
                         TRUE,
                         Ring->Entry);
    Ring->Entry = NULL;

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);
    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);
    XENBUS_STORE(Release, &Ring->StoreInterface);
    XENBUS_DEBUG(Release, &Ring->DebugInterface);

    Trace("<=====\n");
}

VOID
RingTeardown(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");
    Ring->Dpcs = 0;

    Ring->AbsPointer = FALSE;

    RtlZeroMemory(&Ring->Dpc, sizeof (KDPC));

    RtlZeroMemory(&Ring->Lock,
                  sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Ring->GnttabInterface,
                  sizeof (XENBUS_GNTTAB_INTERFACE));

    RtlZeroMemory(&Ring->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Ring->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Ring->EvtchnInterface,
                  sizeof (XENBUS_EVTCHN_INTERFACE));

    Ring->Frontend = NULL;
    Ring->Hid = NULL;

    ASSERT(IsZeroMemory(Ring, sizeof (XENVKBD_RING)));
    __RingFree(Ring);
    Trace("<=====\n");
}

VOID
RingNotify(
    IN  PXENVKBD_RING   Ring
    )
{
    if (KeInsertQueueDpc(&Ring->Dpc, NULL, NULL))
        Ring->Dpcs++;
}
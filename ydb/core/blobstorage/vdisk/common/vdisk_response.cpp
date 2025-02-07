#include "vdisk_response.h"
#include "vdisk_events.h"
#include <ydb/core/base/interconnect_channels.h>
#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_util_space_color.h>
#include <util/system/datetime.h>

namespace NKikimr {

void LogOOSStatus(ui32 flags, const TLogoBlobID& blobId, const TString& vDiskLogPrefix);
void UpdateMonOOSStatus(ui32 flags, const std::shared_ptr<NMonGroup::TOutOfSpaceGroup>& monGroup);

void SendVDiskResponse(const TActorContext &ctx, const TActorId &recipient, IEventBase *ev, ui64 cookie, const TString& vDiskLogPrefix, const std::shared_ptr<NMonGroup::TOutOfSpaceGroup>& monGroup) {
    ui32 channel = TInterconnectChannels::IC_BLOBSTORAGE;
    if (TEvVResultBase *base = dynamic_cast<TEvVResultBase *>(ev)) {
        channel = base->GetChannelToSend();
    }
    SendVDiskResponse(ctx, recipient, ev, cookie, channel, vDiskLogPrefix, monGroup);
}

void SendVDiskResponse(const TActorContext &ctx, const TActorId &recipient, IEventBase *ev, ui64 cookie, ui32 channel, const TString& vDiskLogPrefix, const std::shared_ptr<NMonGroup::TOutOfSpaceGroup>& monGroup) {
    switch(ev->Type()) {
        case TEvBlobStorage::TEvVPutResult::EventType: {
            TEvBlobStorage::TEvVPutResult* event = static_cast<TEvBlobStorage::TEvVPutResult *>(ev);
            LogOOSStatus(event->Record.GetStatusFlags(), LogoBlobIDFromLogoBlobID(event->Record.GetBlobID()), vDiskLogPrefix);
            UpdateMonOOSStatus(event->Record.GetStatusFlags(), monGroup);
            break;
        }
        case TEvBlobStorage::TEvVMultiPutResult::EventType: {
            TEvBlobStorage::TEvVMultiPutResult *event = static_cast<TEvBlobStorage::TEvVMultiPutResult *>(ev);
            for (ui64 i = 0; i < event->Record.ItemsSize(); ++i) {
                const auto& item = event->Record.GetItems(i);
                LogOOSStatus(item.GetStatusFlags(), LogoBlobIDFromLogoBlobID(item.GetBlobID()), vDiskLogPrefix);
                UpdateMonOOSStatus(item.GetStatusFlags(), monGroup);
            }
        }
    }

    switch (const ui32 type = ev->Type()) {
#define HANDLE_EVENT(T) \
        case TEvBlobStorage::T::EventType: { \
            TEvBlobStorage::T *event = static_cast<TEvBlobStorage::T *>(ev); \
            const double usPerCycle = 1000000.0 / NHPTimer::GetCyclesPerSecond(); \
            event->Record.MutableTimestamps()->SetSentByVDiskUs(GetCycleCountFast() * usPerCycle); \
            break; \
        }

        HANDLE_EVENT(TEvVPutResult)
        HANDLE_EVENT(TEvVMultiPutResult)
        HANDLE_EVENT(TEvVGetResult)
        HANDLE_EVENT(TEvVPatchFoundParts)
        HANDLE_EVENT(TEvVPatchXorDiffResult)
        HANDLE_EVENT(TEvVPatchResult)

#undef HANDLE_EVENT

        case TEvBlobStorage::EvVAssimilateResult: // override channel for assimilation result
            channel = TInterconnectChannels::IC_BLOBSTORAGE_ASYNC_DATA;
            break;
    }

    auto event = std::make_unique<IEventHandle>(recipient, ctx.SelfID, ev, IEventHandle::MakeFlags(channel, 0), cookie);
    if (TEvVResultBase *base = dynamic_cast<TEvVResultBase *>(ev)) {
        base->FinalizeAndSend(ctx, std::move(event));
    } else {
        TActivationContext::Send(event.release());
    }
}

void LogOOSStatus(ui32 flags, const TLogoBlobID& blobId, const TString& vDiskLogPrefix) {
    if (!TlsActivationContext) {
        return;
    }

    LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::BS_VDISK_CHUNKS,
        vDiskLogPrefix << "Disk space status changed to " <<
        TPDiskSpaceColor_Name(StatusFlagToSpaceColor(flags)) << " on blob " << blobId.ToString());
}

void UpdateMonOOSStatus(ui32 flags, const std::shared_ptr<NMonGroup::TOutOfSpaceGroup>& monGroup) {
    if (!monGroup) {
        return;
    }

    if (flags & NKikimrBlobStorage::StatusDiskSpaceRed) {
        monGroup->ResponsesWithDiskSpaceRed().Inc();
    } else if (flags & NKikimrBlobStorage::StatusDiskSpaceOrange) {
        monGroup->ResponsesWithDiskSpaceOrange().Inc();
    } else if (flags & NKikimrBlobStorage::StatusDiskSpaceLightOrange) {
        monGroup->ResponsesWithDiskSpaceLightOrange().Inc();
    } else if (flags & NKikimrBlobStorage::StatusDiskSpacePreOrange) {
        monGroup->ResponsesWithDiskSpacePreOrange().Inc();
    } else if (flags & NKikimrBlobStorage::StatusDiskSpaceYellowStop) {
        monGroup->ResponsesWithDiskSpaceYellowStop().Inc();
    } else if (flags & NKikimrBlobStorage::StatusDiskSpaceLightYellowMove) {
        monGroup->ResponsesWithDiskSpaceLightYellowMove().Inc();
    }
}

}//NKikimr

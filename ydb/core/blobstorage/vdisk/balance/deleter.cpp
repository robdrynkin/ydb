#include "defs.h"
#include "utils.h"

#include <ydb/core/blobstorage/base/vdisk_sync_common.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_queues.h>
#include <ydb/core/blobstorage/vdisk/hullop/blobstorage_hulllog.h>
#include <ydb/core/blobstorage/vdisk/synclog/blobstorage_synclog_public_events.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_private_events.h>

#include <ydb/core/util/stlog.h>

namespace NKikimr {
namespace NBalancing {

namespace {
    struct TPartOnMain {
        TLogoBlobID Key;
        bool HasOnMain;
    };

    class TPartsRequester {
    private:
        const TActorId NotifyId;
        const size_t BatchSize;
        TQueue<TLogoBlobID> Parts;
        TReplQuoter::TPtr Quoter;
        TIntrusivePtr<TBlobStorageGroupInfo> GInfo;
        TQueueActorMapPtr QueueActorMapPtr;
        TDuration BatchTimeout;
        TInstant CurBatchStart;
        ui32 BatchId = 0;

        TVector<TPartOnMain> Result;
        ui32 Responses = 0;
        ui32 ExpectedResponses = 0;
    public:

        TPartsRequester(
            TActorId notifyId,
            size_t batchSize,
            TQueue<TLogoBlobID> parts,
            TReplQuoter::TPtr quoter,
            TIntrusivePtr<TBlobStorageGroupInfo> gInfo,
            TQueueActorMapPtr queueActorMapPtr,
            TDuration batchTimeout
        )
            : NotifyId(notifyId)
            , BatchSize(batchSize)
            , Parts(std::move(parts))
            , Quoter(quoter)
            , GInfo(gInfo)
            , QueueActorMapPtr(queueActorMapPtr)
            , BatchTimeout(batchTimeout)
            , Result(Reserve(BatchSize))
        {}

        ui64 MakeCookie(ui32 itemIndex) {
            return BatchId * BatchSize + itemIndex;
        }

        std::pair<ui32, ui32> ParseCookie(ui64 cookie) {
            return {cookie / BatchSize, cookie % BatchSize};
        }

        void ScheduleJobQuant(const TActorId& selfId) {
            if (ExpectedResponses != 0) {
                return;
            }

            CurBatchStart = TlsActivationContext->Now();
            Result.resize(Min(Parts.size(), BatchSize));
            for (ui64 i = 0; i < BatchSize && !Parts.empty(); ++i) {
                auto key = Parts.front();
                Parts.pop();
                Result[i] = TPartOnMain{
                    .Key=key,
                    .HasOnMain=false
                };

                auto vDiskId = GetMainReplicaVDiskId(*GInfo, key);

                // query which would tell us which parts are realy on main (not by ingress)
                auto ev = TEvBlobStorage::TEvVGet::CreateExtremeIndexQuery(
                    vDiskId, TInstant::Max(), NKikimrBlobStorage::EGetHandleClass::AsyncRead,
                    TEvBlobStorage::TEvVGet::EFlags::None, MakeCookie(i),
                    {{key.FullID(), 0, 0}}
                );
                ui32 msgSize = ev->CalculateSerializedSize();
                TReplQuoter::QuoteMessage(
                    Quoter,
                    std::make_unique<IEventHandle>(QueueActorMapPtr->at(TVDiskIdShort(vDiskId)), selfId, ev.release()),
                    msgSize
                );
                ++ExpectedResponses;
            }
        }

        std::pair<std::optional<TVector<TPartOnMain>>, ui32> TryGetResults() {
            if (ExpectedResponses == Responses || CurBatchStart + BatchTimeout <= TlsActivationContext->Now()) {
                if (ExpectedResponses != 0) {
                    ++BatchId;
                }
                ExpectedResponses = 0;
                Responses = 0;
                return {std::move(Result), Parts.size()};
            }
            return {std::nullopt, Parts.size()};
        }

        void Handle(TEvBlobStorage::TEvVGetResult::TPtr ev, const TString& vDiskLogPrefix) {
            ++Responses;
            auto msg = ev->Get()->Record;
            if (msg.GetStatus() != NKikimrProto::EReplyStatus::OK) {
                STLOG(PRI_ERROR, BS_VDISK_BALANCING, BSVB19, VDISKP(vDiskLogPrefix, "Failed to get parts from main"), (Status, msg.GetStatus()));
                return;
            }
            const auto& [batchId, i] = ParseCookie(msg.GetCookie());
            if (batchId != BatchId) {
                STLOG(PRI_ERROR, BS_VDISK_BALANCING, BSVB20, VDISKP(vDiskLogPrefix, "Got response for old batch"), (BatchId, BatchId), (Cookie, msg.GetCookie()));
                return;
            }
            auto res = msg.GetResult().at(0);
            for (ui32 partId: res.GetParts()) {
                if (partId == Result[i].Key.PartId()) {
                    Result[i].HasOnMain = true;
                }
            }
        }
    };


    class TDeleter : public TActorBootstrapped<TDeleter> {
        TActorId NotifyId;
        std::shared_ptr<TBalancingCtx> Ctx;
        TIntrusivePtr<TBlobStorageGroupInfo> GInfo;
        TPartsRequester PartsRequester;
        ui32 OrderId = 0;

        struct TStats {
            ui32 PartsRequested = 0;
            ui32 PartsDecidedToDelete = 0;
            ui32 PartsMarkedDeleted = 0;
        };
        TStats Stats;

        void ScheduleJobQuant() {
            PartsRequester.ScheduleJobQuant(SelfId());
            TryProcessResults();
            Schedule(BATCH_TIMEOUT, new NActors::TEvents::TEvWakeup());
        }

        void Handle(TEvBlobStorage::TEvVGetResult::TPtr ev) {
            PartsRequester.Handle(ev, Ctx->VCtx->VDiskLogPrefix);
            TryProcessResults();
        }

        void TryProcessResults() {
            if (auto [batch, partsLeft] = PartsRequester.TryGetResults(); batch.has_value()) {
                Stats.PartsRequested += batch->size();
                for (auto& part: *batch) {
                    if (part.HasOnMain) {
                        ++Stats.PartsDecidedToDelete;
                        DeleteLocal(part.Key);
                    }
                }
                Send(NotifyId, new NActors::TEvents::TEvCompleted(DELETER_ID, partsLeft));
                if (partsLeft == 0) {
                    PassAway();
                }
            }
        }

        void DeleteLocal(const TLogoBlobID& key) {
            TLogoBlobID keyWithoutPartId(key, 0);

            TIngress ingress;
            ingress.DeleteHandoff(&GInfo->GetTopology(), Ctx->VCtx->ShortSelfVDisk, key);

            STLOG(PRI_DEBUG, BS_VDISK_BALANCING, BSVB10, VDISKP(Ctx->VCtx, "Deleting local"), (LogoBlobID, key.ToString()),
                (Ingress, ingress.ToString(&GInfo->GetTopology(), Ctx->VCtx->ShortSelfVDisk, keyWithoutPartId)));

            Send(Ctx->SkeletonId, new TEvDelLogoBlobDataSyncLog(keyWithoutPartId, ingress, OrderId++));
        }

        void Handle(TEvDelLogoBlobDataSyncLogResult::TPtr ev) {
            Y_VERIFY(ev->Get()->OrderId == Stats.PartsMarkedDeleted++);
            ++Ctx->MonGroup.MarkedReadyToDelete();
        }

        void PassAway() override {
            Send(NotifyId, new NActors::TEvents::TEvCompleted(DELETER_ID));
            TActorBootstrapped::PassAway();
        }

        void Handle(TEvVGenerationChange::TPtr ev) {
            GInfo = ev->Get()->NewInfo;
        }

        STRICT_STFUNC(StateFunc,
            cFunc(NActors::TEvents::TEvWakeup::EventType, ScheduleJobQuant)
            hFunc(TEvBlobStorage::TEvVGetResult, Handle)
            hFunc(TEvDelLogoBlobDataSyncLogResult, Handle)
            cFunc(NActors::TEvents::TEvPoison::EventType, PassAway)

            hFunc(TEvVGenerationChange, Handle)
        );

        constexpr static ui32 BATCH_SIZE = 32;
        constexpr static TDuration BATCH_TIMEOUT = TDuration::Seconds(30);

    public:
        TDeleter() = default;
        TDeleter(
            TActorId notifyId,
            TQueue<TLogoBlobID> parts,
            TQueueActorMapPtr queueActorMapPtr,
            std::shared_ptr<TBalancingCtx> ctx
        )
            : NotifyId(notifyId)
            , Ctx(ctx)
            , GInfo(ctx->GInfo)
            , PartsRequester(SelfId(), BATCH_SIZE, std::move(parts), Ctx->VCtx->ReplNodeRequestQuoter, GInfo, queueActorMapPtr, BATCH_TIMEOUT)
        {
        }

        void Bootstrap() {
            Become(&TThis::StateFunc);
        }
    };
}

IActor* CreateDeleterActor(
    TActorId notifyId,
    TQueue<TLogoBlobID> parts,
    TQueueActorMapPtr queueActorMapPtr,
    std::shared_ptr<TBalancingCtx> ctx
) {
    return new TDeleter(notifyId, std::move(parts), queueActorMapPtr, ctx);
}

} // NBalancing
} // NKikimr

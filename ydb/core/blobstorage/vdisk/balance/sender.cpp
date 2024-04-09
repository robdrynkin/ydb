#include "defs.h"
#include "utils.h"

#include <ydb/core/blobstorage/vdisk/common/vdisk_queues.h>
#include <ydb/core/blobstorage/base/vdisk_sync_common.h>

#include <ydb/core/util/stlog.h>

#include <random>


namespace NKikimr {
namespace NBalancing {
namespace {
    struct TPart {
        TLogoBlobID Key;
        NMatrix::TVectorType PartsMask;
        TVector<TRope> PartsData;
        TPartInfo OrigPart;
    };

    class TReader {
    private:
        const size_t BatchSize;
        TPDiskCtxPtr PDiskCtx;
        TQueue<TPartInfo> Parts;
        TReplQuoter::TPtr Quoter;
        const TBlobStorageGroupType GType;

        // TVector<TPart> Result;
        ui32 BatchId = 0;
        ui32 Responses = 0;
        ui32 ExpectedResponses = 0;

        THashMap<ui64, TPart> ResultMap;
        std::mt19937_64 Gen64;

        TVector<std::tuple<ui64, TLogoBlobID, TDiskPart>> ExpectedParts;
        TVector<std::tuple<ui64, TLogoBlobID, TDiskPart, NKikimrProto::EReplyStatus>> ReceivedParts;

    public:

        TReader(size_t batchSize, TPDiskCtxPtr pDiskCtx, TQueue<TPartInfo> parts, TReplQuoter::TPtr replPDiskReadQuoter, TBlobStorageGroupType gType)
            : BatchSize(batchSize)
            , PDiskCtx(pDiskCtx)
            , Parts(std::move(parts))
            , Quoter(replPDiskReadQuoter)
            , GType(gType)
            // , Result(Reserve(BatchSize))
            , Gen64(TlsActivationContext->Now().MilliSeconds())
        {}

        ui64 MakeCookie(ui32 itemIndex) {
            return BatchId * BatchSize + itemIndex;
        }

        std::pair<ui32, ui32> ParseCookie(ui64 cookie) {
            return {cookie / BatchSize, cookie % BatchSize};
        }

        void ScheduleJobQuant(const TActorId& selfId) {
            // Result.resize(Min(Parts.size(), BatchSize));
            ResultMap.clear();
            ExpectedResponses = 0;
            for (ui64 i = 0; i < BatchSize && !Parts.empty(); ++i) {
                auto item = Parts.front();
                Parts.pop();
                // Result[i] = TPart{
                //     .Key=item.Key,
                //     .PartsMask=item.PartsMask,
                //     .OrigPart=item
                // };

                ui64 key = Gen64();
                ResultMap[key] = TPart{
                    .Key=item.Key,
                    .PartsMask=item.PartsMask,
                    .OrigPart=item
                };

                std::visit(TOverloaded{
                    [&](const TRope& data) {
                        // part is already in memory, no need to read it from disk
                        Y_DEBUG_ABORT_UNLESS(item.PartsMask.CountBits() == 1);
                        ResultMap[key].PartsData = {data};
                        // Result[i].PartsData = {data};
                        ++Responses;
                    },
                    [&](const TDiskPart& diskPart) {
                        auto ev = std::make_unique<NPDisk::TEvChunkRead>(
                            PDiskCtx->Dsk->Owner,
                            PDiskCtx->Dsk->OwnerRound,
                            diskPart.ChunkIdx,
                            diskPart.Offset,
                            diskPart.Size,
                            NPriRead::HullLow,
                            // reinterpret_cast<void*>(MakeCookie(i))
                            reinterpret_cast<void*>(key)
                        );

                        TReplQuoter::QuoteMessage(
                            Quoter,
                            std::make_unique<IEventHandle>(PDiskCtx->PDiskId, selfId, ev.release()),
                            diskPart.Size
                        );

                        ExpectedParts.emplace_back(key, item.Key, diskPart);
                    }
                }, item.PartData);
                ++ExpectedResponses;
            }
        }

        std::pair<std::optional<TVector<TPart>>, ui32> TryGetResults() {
            if (ExpectedResponses == Responses) {
                if (Responses != 0) {
                    ++BatchId;
                }
                ExpectedResponses = 0;
                Responses = 0;

                TVector<TPart> result;
                result.reserve(ResultMap.size());
                for (auto& [_, part]: ResultMap) {
                    result.push_back(std::move(part));
                }
                return {std::move(result), Parts.size()};
            }
            return {std::nullopt, Parts.size()};
        }

        void Handle(NPDisk::TEvChunkReadResult::TPtr ev, const TString& vDiskLogPrefix) {
            ++Responses;
            auto *msg = ev->Get();
            ui64 cookie = reinterpret_cast<ui64>(msg->Cookie);
            Y_DEBUG_ABORT_UNLESS(ResultMap.contains(cookie));
            auto& res = ResultMap[cookie];
            ReceivedParts.emplace_back(cookie, res.Key, TDiskPart{msg->ChunkIdx, msg->Offset, static_cast<ui32>(msg->Data.ToString().GetSize())}, msg->Status);

            if (msg->Status != NKikimrProto::EReplyStatus::OK) {
                STLOG(PRI_WARN, BS_VDISK_BALANCING, BSVB15, VDISKP(vDiskLogPrefix, "TEvChunkRead failed"), (Status, msg->Status));
                return;
            }
            // const auto& [batchId, i] = ParseCookie(reinterpret_cast<ui64>(msg->Cookie));
            // if (batchId != BatchId) {
            //     STLOG(PRI_WARN, BS_VDISK_BALANCING, BSVB16, VDISKP(vDiskLogPrefix, "Got response for old batch"), (BatchId, BatchId), (i, i));
            //     return;
            // }
            if (!msg->Data.IsReadable(0, std::get<TDiskPart>(res.OrigPart.PartData).Size)) {
                STLOG(PRI_WARN, BS_VDISK_BALANCING, BSVB16, VDISKP(vDiskLogPrefix, "Got response with wrong size"), (ExpectedSize, std::get<TDiskPart>(res.OrigPart.PartData).Size), (Key, res.Key.ToString()));
                return;
            }
            const auto& key = res.Key;
            auto data = TRope(msg->Data.ToString());
            auto localParts = res.PartsMask;

            {
                auto part = res.OrigPart.PartData;

                if (std::holds_alternative<TRope>(part)) {
                    Y_DEBUG_ABORT_UNLESS(data.size() == std::get<TRope>(part).GetSize());
                } else {
                    const auto& diskPart = std::get<TDiskPart>(part);
                    Y_DEBUG_ABORT_UNLESS(data.size() == diskPart.Size);
                }
            }

            auto diskBlob = TDiskBlob(&data, localParts, GType, key);

            for (ui8 partIdx = localParts.FirstPosition(); partIdx < localParts.GetSize(); partIdx = localParts.NextPosition(partIdx)) {
                TRope result;
                result = diskBlob.GetPart(partIdx, &result);
                res.PartsData.emplace_back(std::move(result));
            }
        }
    };


    class TSender : public TActorBootstrapped<TSender> {
        TActorId NotifyId;
        TQueueActorMapPtr QueueActorMapPtr;
        std::shared_ptr<TBalancingCtx> Ctx;
        TIntrusivePtr<TBlobStorageGroupInfo> GInfo;
        TReader Reader;

        struct TStats {
            ui32 PartsRead = 0;
            ui32 PartsSent = 0;
            ui32 PartsSentUnsuccsesfull = 0;
        };
        TStats Stats;

        void ScheduleJobQuant() {
            Reader.ScheduleJobQuant(SelfId());
            // if all parts are already in memory, we could process results right away
            TryProcessResults();
        }

        void TryProcessResults() {
            if (auto [batch, partsLeft] = Reader.TryGetResults(); batch.has_value()) {
                Stats.PartsRead += batch->size();
                SendParts(*batch);

                // notify about job quant completion
                Send(NotifyId, new NActors::TEvents::TEvCompleted(SENDER_ID, partsLeft));

                if (partsLeft == 0) {
                    // no more parts to send
                    PassAway();
                }
            }
        }

        void Handle(NPDisk::TEvChunkReadResult::TPtr ev) {
            Reader.Handle(ev, Ctx->VCtx->VDiskLogPrefix);
            TryProcessResults();
        }

        void SendParts(const TVector<TPart>& batch) {
            STLOG(PRI_DEBUG, BS_VDISK_BALANCING, BSVB11, VDISKP(Ctx->VCtx, "Sending parts"), (BatchSize, batch.size()));

            for (const auto& part: batch) {
                auto localParts = part.PartsMask;
                if (part.PartsData.empty()) {
                    STLOG(PRI_WARN, BS_VDISK_BALANCING, BSVB12, VDISKP(Ctx->VCtx, "No parts data, probably TEvChunkRead failed"), (Key, part.Key.ToString()));
                    continue;
                }
                Y_DEBUG_ABORT_UNLESS(localParts.CountBits() == part.PartsData.size());
                for (ui8 partIdx = localParts.FirstPosition(), i = 0; partIdx < localParts.GetSize(); partIdx = localParts.NextPosition(partIdx), ++i) {
                    auto key = TLogoBlobID(part.Key, partIdx + 1);
                    const auto& data = part.PartsData[i];
                    auto vDiskId = GetMainReplicaVDiskId(*GInfo, key);
                    STLOG(PRI_DEBUG, BS_VDISK_BALANCING, BSVB17, VDISKP(Ctx->VCtx, "Sending"), (LogoBlobId, key.ToString()),
                        (To, GInfo->GetTopology().GetOrderNumber(TVDiskIdShort(vDiskId))), (DataSize, data.size()));

                    auto& queue = (*QueueActorMapPtr)[TVDiskIdShort(vDiskId)];
                    auto ev = std::make_unique<TEvBlobStorage::TEvVPut>(
                        key, data, vDiskId,
                        true, nullptr,
                        TInstant::Max(), NKikimrBlobStorage::EPutHandleClass::AsyncBlob
                    );
                    TReplQuoter::QuoteMessage(
                        Ctx->VCtx->ReplNodeRequestQuoter,
                        std::make_unique<IEventHandle>(queue, SelfId(), ev.release()),
                        data.size()
                    );
                }
            }
        }

        void Handle(TEvBlobStorage::TEvVPutResult::TPtr ev) {
            ++Stats.PartsSent;
            if (ev->Get()->Record.GetStatus() != NKikimrProto::OK) {
                ++Stats.PartsSentUnsuccsesfull;
                STLOG(PRI_WARN, BS_VDISK_BALANCING, BSVB18, VDISKP(Ctx->VCtx, "Put failed"), (Msg, ev->Get()->ToString()));
                return;
            }
            ++Ctx->MonGroup.SentOnMain();
            STLOG(PRI_DEBUG, BS_VDISK_BALANCING, BSVB14, VDISKP(Ctx->VCtx, "Put done"), (Msg, ev->Get()->ToString()));
        }

        void PassAway() override {
            Send(NotifyId, new NActors::TEvents::TEvCompleted(SENDER_ID));
            TActorBootstrapped::PassAway();
        }

        void Handle(TEvVGenerationChange::TPtr ev) {
            GInfo = ev->Get()->NewInfo;
        }

        STRICT_STFUNC(StateFunc,
            cFunc(NActors::TEvents::TEvWakeup::EventType, ScheduleJobQuant)
            hFunc(NPDisk::TEvChunkReadResult, Handle)
            hFunc(TEvBlobStorage::TEvVPutResult, Handle)
            cFunc(NActors::TEvents::TEvPoison::EventType, PassAway)

            hFunc(TEvVGenerationChange, Handle)
        );

    public:
        TSender(
            TActorId notifyId,
            TQueue<TPartInfo> parts,
            TQueueActorMapPtr queueActorMapPtr,
            std::shared_ptr<TBalancingCtx> ctx
        )
            : NotifyId(notifyId)
            , QueueActorMapPtr(queueActorMapPtr)
            , Ctx(ctx)
            , GInfo(ctx->GInfo)
            , Reader(32, Ctx->PDiskCtx, std::move(parts), ctx->VCtx->ReplPDiskReadQuoter, GInfo->GetTopology().GType)
        {}

        void Bootstrap() {
            Become(&TThis::StateFunc);
        }
    };
}

IActor* CreateSenderActor(
    TActorId notifyId,
    TQueue<TPartInfo> parts,
    TQueueActorMapPtr queueActorMapPtr,
    std::shared_ptr<TBalancingCtx> ctx
) {
    return new TSender(notifyId, parts, queueActorMapPtr, ctx);
}

} // NBalancing
} // NKikimr

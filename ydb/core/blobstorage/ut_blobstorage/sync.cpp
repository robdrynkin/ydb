#include <ydb/core/blobstorage/ut_blobstorage/lib/ut_helpers.h>
#include <ydb/core/blobstorage/ut_blobstorage/lib/env.h>
#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_private_events.h>
#include <ydb/core/blobstorage/vdisk/synclog/blobstorage_synclog_private_events.h>
#include <util/random/random.h>

Y_UNIT_TEST_SUITE(BlobStorageSync) {

    void TestCutting(TBlobStorageGroupType groupType) {
        const ui32 groupSize = groupType.BlobSubgroupSize();

        // for (ui32 mask = 0; mask < (1 << groupSize); ++mask) {  // TIMEOUT
        {
            ui32 mask = RandomNumber(1ull << groupSize);
            for (bool compressChunks : { true, false }) {
                TEnvironmentSetup env{{
                    .NodeCount = groupSize,
                    .Erasure = groupType,
                }};

                env.CreateBoxAndPool(1, 1);
                std::vector<ui32> groups = env.GetGroups();
                UNIT_ASSERT_VALUES_EQUAL(groups.size(), 1);
                ui32 groupId = groups[0];

                const ui64 tabletId = 5000;
                const ui32 channel = 10;
                ui32 gen = 1;
                ui32 step = 1;
                ui64 cookie = 1;

                ui64 totalSize = 0;

                std::vector<TControlWrapper> cutLocalSyncLogControls;
                std::vector<TControlWrapper> compressChunksControls;
                std::vector<TActorId> edges;

                for (ui32 nodeId = 1; nodeId <= groupSize; ++nodeId) {
                    cutLocalSyncLogControls.emplace_back(0, 0, 1);
                    compressChunksControls.emplace_back(1, 0, 1);
                    TAppData* appData = env.Runtime->GetNode(nodeId)->AppData.get();
                    appData->Icb->RegisterSharedControl(cutLocalSyncLogControls.back(), "VDiskControls.EnableLocalSyncLogDataCutting");
                    appData->Icb->RegisterSharedControl(compressChunksControls.back(), "VDiskControls.EnableSyncLogChunkCompressionHDD");
                    edges.push_back(env.Runtime->AllocateEdgeActor(nodeId));
                }

                for (ui32 i = 0; i < groupSize; ++i) {
                    env.Runtime->WrapInActorContext(edges[i], [&] {
                        SendToBSProxy(edges[i], groupId, new TEvBlobStorage::TEvStatus(TInstant::Max()));
                    });
                    auto res = env.WaitForEdgeActorEvent<TEvBlobStorage::TEvStatusResult>(edges[i], false);
                    UNIT_ASSERT_VALUES_EQUAL(res->Get()->Status, NKikimrProto::OK);
                }

                auto writeBlob = [&](ui32 nodeId, ui32 blobSize) {
                    TLogoBlobID blobId(tabletId, gen, step, channel, blobSize, ++cookie);
                    totalSize += blobSize;
                    TString data = MakeData(blobSize);
                    
                    const TActorId& sender = edges[nodeId - 1];
                    env.Runtime->WrapInActorContext(sender, [&] () {
                        SendToBSProxy(sender, groupId, new TEvBlobStorage::TEvPut(blobId, std::move(data), TInstant::Max()));
                    });
                };

                env.Runtime->FilterFunction = [&](ui32/* nodeId*/, std::unique_ptr<IEventHandle>& ev) {
                    switch(ev->Type) {
                        case TEvBlobStorage::TEvPutResult::EventType:
                            UNIT_ASSERT_VALUES_EQUAL(ev->Get<TEvBlobStorage::TEvPutResult>()->Status, NKikimrProto::OK);
                            return false;
                        default:
                            return true;
                    }
                };
                
                while (totalSize < 16_MB) {
                    writeBlob(GenerateRandom(1, groupSize + 1), GenerateRandom(1, 1_MB));
                }
                env.Sim(TDuration::Minutes(5));

                for (ui32 i = 0; i < groupSize; ++i) {
                    cutLocalSyncLogControls[i] = !!(mask & (1 << i));
                    compressChunksControls[i] = compressChunks;
                }

                while (totalSize < 32_MB) {
                    writeBlob(GenerateRandom(1, groupSize + 1), GenerateRandom(1, 1_MB));
                }

                env.Sim(TDuration::Minutes(5));
            }
        }
    }

    Y_UNIT_TEST(TestSyncLogCuttingMirror3dc) {
        TestCutting(TBlobStorageGroupType::ErasureMirror3dc);
    }

    Y_UNIT_TEST(TestSyncLogCuttingMirror3of4) {
        TestCutting(TBlobStorageGroupType::ErasureMirror3of4);
    }

    Y_UNIT_TEST(TestSyncLogCuttingBlock4Plus2) {
        TestCutting(TBlobStorageGroupType::Erasure4Plus2Block);
    }

    Y_UNIT_TEST(SyncLogRetriesDeleteCommitWithoutRejectedChunk) {
        TEnvironmentSetup env{{
            .NodeCount = 1,
            .Erasure = TBlobStorageGroupType::ErasureNone,
        }};
        auto& runtime = env.Runtime;

        env.CreateBoxAndPool(1, 1);
        const auto groups = env.GetGroups();
        UNIT_ASSERT_VALUES_EQUAL(groups.size(), 1);
        const TIntrusivePtr<TBlobStorageGroupInfo> info = env.GetGroupInfo(groups.front());
        const TVDiskID vdiskId = info->GetVDiskId(0);
        const TActorId vdiskActorId = info->GetActorId(0);
        const TActorId edge = runtime->AllocateEdgeActor(vdiskActorId.NodeId(), __FILE__, __LINE__);

        TActorId syncLogId;
        TActorId syncLogKeeperId;
        bool gotOwner = false;
        NPDisk::TOwner owner = 0;
        NPDisk::TOwnerRound ownerRound = 0;
        bool observedSyncLogDataCommit = false;
        bool injectedDeleteError = false;
        bool observedRetriedDeleteCommit = false;
        ui32 rejectedChunkIdx = 0;
        TVector<ui32> firstDeleteChunks;
        TVector<ui32> retriedDeleteChunks;

        auto isSyncLogEntryPointCommit = [](const NPDisk::TEvLog& msg) {
            return msg.Signature.GetUnmasked() == TLogSignature::SignatureSyncLogIdx &&
                msg.CommitRecord.IsStartingPoint;
        };

        runtime->FilterFunction = [&](ui32 nodeId, std::unique_ptr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvBlobStorage::EvSyncLogPut:
                case TEvBlobStorage::EvSyncLogPutSst:
                    if (!syncLogId) {
                        syncLogId = ev->Recipient;
                    } else if (!syncLogKeeperId && ev->Recipient != syncLogId) {
                        syncLogKeeperId = ev->Recipient;
                    }
                    break;

                case TEvBlobStorage::EvLog: {
                    const auto *msg = ev->Get<NPDisk::TEvLog>();
                    owner = msg->Owner;
                    ownerRound = msg->OwnerRound;
                    gotOwner = true;

                    if (!isSyncLogEntryPointCommit(*msg)) {
                        break;
                    }

                    if (msg->CommitRecord.CommitChunks) {
                        observedSyncLogDataCommit = true;
                    }

                    if (!injectedDeleteError && msg->CommitRecord.DeleteToDecommitted &&
                            !msg->CommitRecord.DeleteChunks.empty()) {
                        injectedDeleteError = true;
                        firstDeleteChunks = msg->CommitRecord.DeleteChunks;
                        rejectedChunkIdx = firstDeleteChunks.front();

                        TString errorReason = TStringBuilder()
                            << "PDiskId# 1002 Can't delete chunkIdx# " << rejectedChunkIdx
                            << " ownerId# " << owner << " != trueOwnerId# 1! Marker# BPD77";
                        auto result = std::make_unique<NPDisk::TEvLogResult>(
                            NKikimrProto::ERROR,
                            NPDisk::TStatusFlags(NKikimrBlobStorage::StatusIsValid),
                            std::move(errorReason),
                            0);
                        runtime->Send(
                            new IEventHandle(ev->Sender, ev->Recipient, result.release(), 0, ev->Cookie),
                            ev->Sender.NodeId());
                        return false;
                    }

                    if (injectedDeleteError && !observedRetriedDeleteCommit) {
                        observedRetriedDeleteCommit = true;
                        retriedDeleteChunks = msg->CommitRecord.DeleteChunks;
                    }
                    break;
                }
            }

            Y_UNUSED(nodeId);
            return true;
        };

        auto simUntil = [&](auto predicate, TDuration timeout) {
            const TInstant deadline = runtime->GetClock() + timeout;
            runtime->Sim([&] { return runtime->GetClock() <= deadline && !predicate(); });
            return predicate();
        };

        env.WithQueueId(vdiskId, NKikimrBlobStorage::EVDiskQueueId::PutTabletLog, [&](const TActorId& queueId) {
            auto multiPut = std::make_unique<TEvBlobStorage::TEvVMultiPut>(vdiskId, TInstant::Max(),
                NKikimrBlobStorage::EPutHandleClass::TabletLog, false);
            const TString data = MakeData(1);
            const ui32 records = 4'096;

            for (ui32 i = 0; i < records; ++i) {
                const TLogoBlobID blobId(TLogoBlobID(42, 1, i + 1, 0, data.size(), i + 1), 1);
                multiPut->AddVPut(blobId, TRcBuf(data), nullptr, nullptr, {});
            }

            runtime->Send(new IEventHandle(queueId, edge, multiPut.release()), edge.NodeId());
            auto res = env.WaitForEdgeActorEvent<TEvBlobStorage::TEvVMultiPutResult>(edge, false);
            UNIT_ASSERT_VALUES_EQUAL(res->Get()->Record.GetStatus(), NKikimrProto::OK);
            UNIT_ASSERT_VALUES_EQUAL(res->Get()->Record.ItemsSize(), records);
        });

        UNIT_ASSERT_C(simUntil([&] {
            return syncLogId && syncLogKeeperId && gotOwner;
        }, TDuration::Seconds(30)), "failed to discover SyncLog actors or PDisk owner");

        runtime->Send(new IEventHandle(syncLogId, edge,
            new NPDisk::TEvCutLog(owner, ownerRound, Max<ui64>(), 0, 0, 0, 0)), syncLogId.NodeId());

        UNIT_ASSERT_C(simUntil([&] {
            return observedSyncLogDataCommit;
        }, TDuration::Seconds(30)), "failed to move sync log records to PDisk chunks");

        runtime->Send(new IEventHandle(syncLogKeeperId, edge,
            new NSyncLog::TEvSyncLogTrim(Max<ui64>())), syncLogKeeperId.NodeId());

        UNIT_ASSERT_C(simUntil([&] {
            return observedRetriedDeleteCommit;
        }, TDuration::Seconds(30)), "failed to observe retried sync log delete commit");

        UNIT_ASSERT_C(injectedDeleteError, "test did not inject delete error");
        UNIT_ASSERT_C(!firstDeleteChunks.empty(), "first delete commit must contain chunks");
        UNIT_ASSERT_EQUAL(std::count(firstDeleteChunks.begin(), firstDeleteChunks.end(), rejectedChunkIdx), 1);
        UNIT_ASSERT(!Find(retriedDeleteChunks, rejectedChunkIdx));
        UNIT_ASSERT_VALUES_EQUAL(retriedDeleteChunks.size() + 1, firstDeleteChunks.size());
    }

    Y_UNIT_TEST(SyncWhenDiskGetsDown) {
        return; // re-enable when protocol issue is resolved

        TEnvironmentSetup env{{
            .NodeCount = 8,
            .Erasure = TBlobStorageGroupType::Erasure4Plus2Block,
        }};
        auto& runtime = env.Runtime;

        env.CreateBoxAndPool(1, 1);
        auto groups = env.GetGroups();
        UNIT_ASSERT_VALUES_EQUAL(groups.size(), 1);
        const TIntrusivePtr<TBlobStorageGroupInfo> info = env.GetGroupInfo(groups.front());

        const TActorId edge = runtime->AllocateEdgeActor(1, __FILE__, __LINE__);
        const TString buffer = "hello, world!";
        TLogoBlobID id(1, 1, 1, 0, buffer.size(), 0);
        runtime->WrapInActorContext(edge, [&] {
            SendToBSProxy(edge, info->GroupID, new TEvBlobStorage::TEvPut(id, buffer, TInstant::Max()));
        });
        auto res = env.WaitForEdgeActorEvent<TEvBlobStorage::TEvPutResult>(edge, false);
        UNIT_ASSERT_VALUES_EQUAL(res->Get()->Status, NKikimrProto::OK);

        std::unordered_map<TVDiskID, TActorId, THash<TVDiskID>> queues;
        for (ui32 i = 0; i < info->GetTotalVDisksNum(); ++i) {
            const TVDiskID vdiskId = info->GetVDiskId(i);
            queues[vdiskId] = env.CreateQueueActor(vdiskId, NKikimrBlobStorage::EVDiskQueueId::GetFastRead, 1000);
        }

        struct TBlobInfo {
            TVDiskID VDiskId;
            TLogoBlobID BlobId;
            NKikimrProto::EReplyStatus Status;
            std::optional<TIngress> Ingress;
        };
        auto collectBlobInfo = [&] {
            std::vector<TBlobInfo> blobs;
            for (ui32 i = 0; i < info->GetTotalVDisksNum(); ++i) {
                const TVDiskID vdiskId = info->GetVDiskId(i);
                const TActorId queueId = queues.at(vdiskId);
                const TActorId edge = runtime->AllocateEdgeActor(queueId.NodeId(), __FILE__, __LINE__);
                runtime->Send(new IEventHandle(queueId, edge, TEvBlobStorage::TEvVGet::CreateExtremeDataQuery(
                    vdiskId, TInstant::Max(), NKikimrBlobStorage::EGetHandleClass::FastRead,
                    TEvBlobStorage::TEvVGet::EFlags::ShowInternals, {}, {{id}}).release()), queueId.NodeId());
                auto res = env.WaitForEdgeActorEvent<TEvBlobStorage::TEvVGetResult>(edge);
                auto& record = res->Get()->Record;
                UNIT_ASSERT(record.GetStatus() == NKikimrProto::OK || record.GetStatus() == NKikimrProto::NOTREADY);
                for (auto& result : record.GetResult()) {
                    blobs.push_back({.VDiskId = vdiskId, .BlobId = LogoBlobIDFromLogoBlobID(result.GetBlobID()),
                        .Status = result.GetStatus(), .Ingress = result.HasIngress() ? std::make_optional(
                        TIngress(result.GetIngress())) : std::nullopt});
                }
            }
            return blobs;
        };
        auto dumpBlobs = [&](const char *name, const std::vector<TBlobInfo>& blobs) {
            Cerr << "Blobs(" << name <<"):" << Endl;
            for (const auto& item : blobs) {
                Cerr << item.VDiskId << " " << item.BlobId << " " << NKikimrProto::EReplyStatus_Name(item.Status)
                    << " " << (item.Ingress ? item.Ingress->ToString(&info->GetTopology(), item.VDiskId, item.BlobId) :
                    "none") << Endl;
            }
        };

        env.Sim(TDuration::Seconds(10)); // wait for blob to get synced across the group

        auto blobsInitial = collectBlobInfo();

        runtime->WrapInActorContext(edge, [&] {
            SendToBSProxy(edge, info->GroupID, new TEvBlobStorage::TEvCollectGarbage(id.TabletID(), id.Generation(),
                0, id.Channel(), true, id.Generation(), id.Step(), new TVector<TLogoBlobID>(1, id), nullptr, TInstant::Max(),
                false));
        });
        auto res1 = env.WaitForEdgeActorEvent<TEvBlobStorage::TEvCollectGarbageResult>(edge, false);
        UNIT_ASSERT_VALUES_EQUAL(res->Get()->Status, NKikimrProto::OK);

        const ui32 suspendedNodeId = 2;
        env.StopNode(suspendedNodeId);

        runtime->WrapInActorContext(edge, [&] {
            // send the sole do not keep flag
            SendToBSProxy(edge, info->GroupID, new TEvBlobStorage::TEvCollectGarbage(id.TabletID(), 0, 0, 0, false, 0,
                0, nullptr, new TVector<TLogoBlobID>(1, id), TInstant::Max(), false));
        });
        res1 = env.WaitForEdgeActorEvent<TEvBlobStorage::TEvCollectGarbageResult>(edge, false);
        UNIT_ASSERT_VALUES_EQUAL(res->Get()->Status, NKikimrProto::OK);

        // sync barriers and then compact them all
        env.Sim(TDuration::Seconds(10));
        for (ui32 i = 0; i < info->GetTotalVDisksNum(); ++i) {
            const TActorId actorId = info->GetActorId(i);
            if (actorId.NodeId() != suspendedNodeId) {
                const auto& sender = env.Runtime->AllocateEdgeActor(actorId.NodeId());
                auto ev = std::make_unique<IEventHandle>(actorId, sender, TEvCompactVDisk::Create(EHullDbType::LogoBlobs));
                ev->Rewrite(TEvBlobStorage::EvForwardToSkeleton, actorId);
                runtime->Send(ev.release(), sender.NodeId());
                auto res = env.WaitForEdgeActorEvent<TEvCompactVDiskResult>(sender);
            }
        }
        env.Sim(TDuration::Minutes(1));
        auto blobsIntermediate = collectBlobInfo();

        env.StartNode(suspendedNodeId);
        env.Sim(TDuration::Minutes(1));

        auto blobsFinal = collectBlobInfo();

        dumpBlobs("initial", blobsInitial);
        dumpBlobs("intermediate", blobsIntermediate);
        dumpBlobs("final", blobsFinal);

        for (auto& item : blobsIntermediate) {
            UNIT_ASSERT(!item.Ingress || item.Ingress->Raw() == 0);
        }

        for (auto& item : blobsFinal) {
            UNIT_ASSERT(!item.Ingress || item.Ingress->Raw() == 0);
        }
    }
}

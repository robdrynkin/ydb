#include <ydb/core/blobstorage/ut_blobstorage/lib/env.h>


struct TTetsEnv {
    TTetsEnv()
    : Env({
        .NodeCount = 8,
        .VDiskReplPausedAtStart = false,
        .Erasure = TBlobStorageGroupType::Erasure4Plus2Block,
    })
    {
        Env.CreateBoxAndPool(1, 1);
        Env.Sim(TDuration::Minutes(1));

        auto groups = Env.GetGroups();
        UNIT_ASSERT_VALUES_EQUAL(groups.size(), 1);
        GroupInfo = Env.GetGroupInfo(groups.front());

        VDiskActorId = GroupInfo->GetActorId(0);
    }

    static TString PrepareData(const ui32 dataLen) {
        TString data(Reserve(dataLen));
        for (ui32 i = 0; i < dataLen; ++i) {
            data.push_back('a' + i % 26);
        }
        return data;
    }

    void SendPut(const TLogoBlobID& id, NKikimrProto::EReplyStatus expectedStatus) {
        const TString data = PrepareData(id.BlobSize());
        Cerr << "SEND TEvPut with key " << id.ToString() << Endl;
        const TActorId sender = Env.Runtime->AllocateEdgeActor(VDiskActorId.NodeId(), __FILE__, __LINE__);
        auto ev = std::make_unique<TEvBlobStorage::TEvPut>(id, data, TInstant::Max());
        Env.Runtime->WrapInActorContext(sender, [&] {
            SendToBSProxy(sender, GroupInfo->GroupID, ev.release());
        });
        auto res = Env.WaitForEdgeActorEvent<TEvBlobStorage::TEvPutResult>(sender, false);
        UNIT_ASSERT_VALUES_EQUAL(res->Get()->Status, expectedStatus);
        Cerr << "TEvPutResult: " << res->Get()->ToString() << Endl;
    }

    auto SendGet(const TLogoBlobID& id, bool mustRestoreFirst=false) {
        const TActorId sender = Env.Runtime->AllocateEdgeActor(GroupInfo->GetActorId(0).NodeId(), __FILE__, __LINE__);
        auto ev = std::make_unique<TEvBlobStorage::TEvGet>(
            id,
            /* shift */ 0,
            /* size */ id.BlobSize(),
            TInstant::Max(),
            NKikimrBlobStorage::EGetHandleClass::FastRead,
            mustRestoreFirst
        );
        Env.Runtime->WrapInActorContext(sender, [&] () {
            SendToBSProxy(sender, GroupInfo->GroupID, ev.release());
        });
        TInstant getDeadline = Env.Now() + TDuration::Seconds(30);
        auto res = Env.WaitForEdgeActorEvent<TEvBlobStorage::TEvGetResult>(sender, /* termOnCapture */ false, getDeadline);
        return res;
    };

    TEnvironmentSetup Env;
    TIntrusivePtr<TBlobStorageGroupInfo> GroupInfo;
    TActorId VDiskActorId;
};


Y_UNIT_TEST_SUITE(Write2SameBlob) {

    Y_UNIT_TEST(TestWrite2SameBlob) {
        TTetsEnv env;

        env.SendPut(TLogoBlobID(1, 1, 1, 0, 3894, 0), NKikimrProto::OK);
        env.SendPut(TLogoBlobID(1, 1, 1, 0, 6542, 0), NKikimrProto::OK);

        for (ui32 pos = 0; pos < env.Env.Settings.NodeCount; ++pos) {
            env.Env.CompactVDisk(env.GroupInfo->GetActorId(pos)); // VERIFY here
        }
    }

    Y_UNIT_TEST(TestWriteIgnoredBlob) {
        TTetsEnv env;

        auto goodId = TLogoBlobID(72057594037936128ULL, 4, 3, 1, 6542, 24576);
        auto badId = TLogoBlobID(72057594037936128ULL, 4, 3, 1, 3894, 24576);

        env.SendPut(goodId, NKikimrProto::OK);
        env.SendPut(badId, NKikimrProto::OK);

        for (ui32 pos = 0; pos < env.Env.Settings.NodeCount; ++pos) {
            env.Env.CompactVDisk(env.GroupInfo->GetActorId(pos));
        }

        auto res = env.SendGet(goodId);
        UNIT_ASSERT_VALUES_EQUAL(res->Get()->Responses[0].Buffer.ConvertToString().size(), goodId.BlobSize());
    }

    Y_UNIT_TEST(TestWriteIgnoredBlobReverse) {
        TTetsEnv env;

        auto goodId = TLogoBlobID(72057594037936128ULL, 4, 3, 1, 6542, 24576);
        auto badId = TLogoBlobID(72057594037936128ULL, 4, 3, 1, 3894, 24576);

        env.SendPut(badId, NKikimrProto::OK);
        env.SendPut(goodId, NKikimrProto::OK);

        for (ui32 pos = 0; pos < env.Env.Settings.NodeCount; ++pos) {
            env.Env.CompactVDisk(env.GroupInfo->GetActorId(pos));
        }

        auto res = env.SendGet(goodId);
        UNIT_ASSERT_VALUES_EQUAL(res->Get()->Responses[0].Buffer.ConvertToString().size(), goodId.BlobSize());
    }
}

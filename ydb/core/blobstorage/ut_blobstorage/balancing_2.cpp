#include <ydb/core/blobstorage/ut_blobstorage/lib/env.h>

#include <library/cpp/iterator/enumerate.h>

#include <util/random/entropy.h>



struct TTestEnv {
    TTestEnv(ui32 nodeCount, TBlobStorageGroupType erasure)
    : Env({
        .NodeCount = nodeCount,
        .VDiskReplPausedAtStart = false,
        .Erasure = erasure,
    })
    {
        Env.CreateBoxAndPool(1, 1);
        Env.Sim(TDuration::Minutes(1));

        auto groups = Env.GetGroups();
        UNIT_ASSERT_VALUES_EQUAL(groups.size(), 1);
        GroupInfo = Env.GetGroupInfo(groups.front());

        for (ui32 i = 0; i < Env.Settings.NodeCount; ++i) {
            RunningNodes.insert(i);
        }
    }

    void SendPut(ui32 step, const TString& data) {
        const TLogoBlobID id(1, 1, step, 0, data.size(), 0);
        Cerr << "SEND TEvPut with key " << id.ToString() << Endl;
        const TActorId sender = Env.Runtime->AllocateEdgeActor(GroupInfo->GetActorId(*RunningNodes.begin()).NodeId(), __FILE__, __LINE__);
        auto ev = std::make_unique<TEvBlobStorage::TEvPut>(id, data, TInstant::Max());
        Env.Runtime->WrapInActorContext(sender, [&] {
            SendToBSProxy(sender, GroupInfo->GroupID, ev.release());
        });
        auto res = Env.WaitForEdgeActorEvent<TEvBlobStorage::TEvPutResult>(sender, false);
        Cerr << "TEvPutResult: " << res->Get()->ToString() << Endl;
    };

    void StopNode(ui32 position) {
        if (!RunningNodes.contains(position)) {
            return;
        }
        Env.StopNode(GroupInfo->GetActorId(position).NodeId());
        RunningNodes.erase(position);
    }

    void StartNode(ui32 position) {
        if (RunningNodes.contains(position)) {
            return;
        }
        Env.StartNode(GroupInfo->GetActorId(position).NodeId());
        RunningNodes.insert(position);
    }

    TEnvironmentSetup* operator->() {
        return &Env;
    }

    TEnvironmentSetup Env;
    TIntrusivePtr<TBlobStorageGroupInfo> GroupInfo;
    THashSet<ui32> RunningNodes;
};


TString GenData(ui32 len) {
    TString res = TString::Uninitialized(len);
    EntropyPool().Read(res.Detach(), res.size());
    return res;
}


struct TRandomTest {
    TTestEnv Env;
    ui32 NumIters;

    void RunTest() {
        TVector<TString> data(Reserve(NumIters));

        for (ui32 step = 0; step < NumIters; ++step) {
            Cerr << "step = " << step << Endl;
            data.push_back(GenData(16 + random() % 4096));

            if (random() % 10 == 1 && Env.RunningNodes.size() + 2 > Env->Settings.NodeCount) {
                ui32 pos = random() % Env->Settings.NodeCount;
                Cerr << "Stop node " << pos << Endl;
                Env.StopNode(pos);
                Env->Sim(TDuration::Seconds(10));
            }

            Env.SendPut(step, data.back());

            if (random() % 10 == 1) {
                for (ui32 pos = 0; pos < Env->Settings.NodeCount; ++pos) {
                    if (!Env.RunningNodes.contains(pos)) {
                        Cerr << "Start node " << pos << Endl;
                        Env.StartNode(pos);
                        Env->Sim(TDuration::Seconds(10));
                        break;
                    }
                }
            }

            if (random() % 50 == 1) {
                ui32 pos = random() % Env->Settings.NodeCount;
                if (Env.RunningNodes.contains(pos)) {
                    Cerr << "Compact node " << pos << Endl;
                    Env->CompactVDisk(Env.GroupInfo->GetActorId(pos));
                    Env->Sim(TDuration::Seconds(10));
                }
            }

            // Wipe random node
            if (random() % 100 == 1) {
                ui32 pos = random() % Env->Settings.NodeCount;
                if (Env.RunningNodes.contains(pos)) {
                    Cerr << "Wipe node " << pos << Endl;
                    auto baseConfig = Env->FetchBaseConfig();
                    const auto& somePDisk = baseConfig.GetPDisk(pos);
                    const auto& someVSlot = baseConfig.GetVSlot(pos);
                    Env->Wipe(somePDisk.GetNodeId(), somePDisk.GetPDiskId(), someVSlot.GetVSlotId().GetVSlotId());
                    Env->Sim(TDuration::Seconds(10));
                }
            }
        }
    }
};



Y_UNIT_TEST_SUITE(VDiskBalancing) {

    Y_UNIT_TEST(TestRandom_Block42) {
        TRandomTest{TTestEnv(8, TBlobStorageGroupType::Erasure4Plus2Block), 1000}.RunTest();
    }
    Y_UNIT_TEST(TestRandom_Mirror3dc) {
        TRandomTest{TTestEnv(9, TBlobStorageGroupType::ErasureMirror3dc), 1000}.RunTest();
    }

}

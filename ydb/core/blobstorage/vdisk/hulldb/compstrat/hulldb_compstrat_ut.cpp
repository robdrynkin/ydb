#include "hulldb_compstrat_selector.h"
#include "hulldb_compstrat_ratio.h"
#include "hulldb_compstrat_ranks.h"
#include <util/stream/null.h>
#include <ydb/core/blobstorage/vdisk/hulldb/test/testhull_index.h>
#include <ydb/core/blobstorage/vdisk/hulldb/base/hullds_ut.h>
#include <library/cpp/testing/unittest/registar.h>

#define STR     Cnull

namespace NKikimr {

    Y_UNIT_TEST_SUITE(TBlobStorageCompStrat) {

        static constexpr ui64 ChunkSize = 128u << 20u;
        static constexpr ui32 HullCompLevel0MaxSstsAtOnce = 8u;
        static constexpr ui32 HullCompSortedPartsNum = 8u;
        static constexpr bool Level0UseDreg = true;
        using TStrategy = ::NKikimr::NHullComp::TStrategy<TKeyLogoBlob, TMemRecLogoBlob>;
        using TTask = ::NKikimr::NHullComp::TTask<TKeyLogoBlob, TMemRecLogoBlob>;

        struct TPriorityTestEnv {
            TTestContexts Contexts;
            TIntrusivePtr<THullDs> Ds = MakeIntrusive<THullDs>(Contexts.GetHullCtx());
            NHullComp::TBoundariesConstPtr Boundaries = MakeIntrusive<NHullComp::TBoundaries>(ChunkSize,
                HullCompLevel0MaxSstsAtOnce, HullCompSortedPartsNum, Level0UseDreg);
            ui64 NextSstId = 1;

            TPriorityTestEnv() {
                auto arena = std::make_shared<TRopeArena>(&TRopeArenaBackend::Allocate);
                const auto &settings = Contexts.GetLevelIndexSettings();
                Ds->LogoBlobs = MakeIntrusive<TLogoBlobsDs>(settings, arena);
                Ds->Blocks = MakeIntrusive<TBlocksDs>(settings, arena);
                Ds->Barriers = MakeIntrusive<TBarriersDs>(settings, arena);
                Ds->LogoBlobs->LoadCompleted();
                Ds->Blocks->LoadCompleted();
                Ds->Barriers->LoadCompleted();
            }

            ui64 AddSst(ui32 level, ui32 chunks) {
                auto sst = MakeIntrusive<TLogoBlobsSst>(Contexts.GetVCtx());
                sst->AssignedSstId = NextSstId++;
                sst->AllChunks.resize(chunks, 1);
                TMemRecLogoBlob memRec;
                memRec.SetNoBlob();
                sst->LoadedIndex.emplace_back(TKeyLogoBlob(TLogoBlobID(sst->AssignedSstId, 1, 1, 0, 1, 0)), memRec);
                auto &slice = *Ds->LogoBlobs->CurSlice;
                if (level == 0) {
                    slice.Level0.Put(sst);
                } else {
                    while (slice.SortedLevels.size() < level) {
                        slice.SortedLevels.emplace_back(TKeyLogoBlob());
                    }
                    slice.SortedLevels[level - 1].Put(sst);
                }
                return sst->AssignedSstId;
            }

            NHullComp::TLevelRanks GetRanks() const {
                auto snap = Ds->LogoBlobs->GetIndexSnapshot();
                return NHullComp::TLevelRanks(*Boundaries, snap.SliceSnap);
            }

            NHullComp::EAction Select(TTask &task, THashSet<ui64> tablesToCompact = {}) {
                auto snap = Ds->GetIndexSnapshot();
                NHullComp::TSelectorParams params = {Boundaries, 1.0, TInstant::Zero(), {}};
                if (!tablesToCompact.empty()) {
                    params.FullCompactionAttrs.emplace(1, TInstant::Zero(), std::move(tablesToCompact));
                }
                TStrategy strategy(snap.HullCtx, params, std::move(snap.LogoBlobsSnap), std::move(snap.BarriersSnap),
                    &task, false);
                return strategy.Select();
            }
        };

        Y_UNIT_TEST(EmptyLevelRanks) {
            TPriorityTestEnv env;
            const auto ranks = env.GetRanks();
            UNIT_ASSERT_VALUES_EQUAL(ranks.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(ranks.GetMaxRank(), 0.0);
            UNIT_ASSERT_VALUES_EQUAL(ranks.FreePartiallySortedLevelsNum, 16);
            UNIT_ASSERT_VALUES_EQUAL(ranks.VirtualLevelToCompact, 0);
        }

        Y_UNIT_TEST(LevelRanksUseChunksAndAllLevels) {
            TPriorityTestEnv env;
            env.AddSst(0, 8);
            env.AddSst(1, 100); // Partially sorted rank depends on occupied levels, not chunks.
            env.AddSst(17, 192);
            env.AddSst(18, 1024);
            const auto ranks = env.GetRanks();
            UNIT_ASSERT_VALUES_EQUAL(ranks.size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(ranks[0], 0.5);
            UNIT_ASSERT_VALUES_EQUAL(ranks[1], 1.0 / 16);
            UNIT_ASSERT_VALUES_EQUAL(ranks[2], 1.5);
            UNIT_ASSERT_VALUES_EQUAL(ranks[3], 2.0);
            UNIT_ASSERT_VALUES_EQUAL(ranks.GetMaxRank(), 2.0);
            UNIT_ASSERT_VALUES_EQUAL(ranks.VirtualLevelToCompact, 3);
        }

        Y_UNIT_TEST(FullPartiallySortedLevelsRank) {
            TPriorityTestEnv env;
            for (ui32 level = 1; level <= 16; ++level) {
                env.AddSst(level, 1);
            }
            const auto ranks = env.GetRanks();
            UNIT_ASSERT_VALUES_EQUAL(ranks.FreePartiallySortedLevelsNum, 0);
            UNIT_ASSERT_VALUES_EQUAL(ranks.GetMaxRank(), 1000000.0);
            UNIT_ASSERT_VALUES_EQUAL(ranks.VirtualLevelToCompact, 1);
        }

        Y_UNIT_TEST(LevelRanksRespectSnapshotBoundary) {
            TPriorityTestEnv env;
            env.AddSst(0, 8);
            auto snap = env.Ds->LogoBlobs->GetIndexSnapshot();
            env.AddSst(0, 16);
            const NHullComp::TLevelRanks oldRanks(*env.Boundaries, snap.SliceSnap);
            UNIT_ASSERT_VALUES_EQUAL(oldRanks[0], 0.5);
            UNIT_ASSERT_VALUES_EQUAL(env.GetRanks()[0], 1.5);
        }

        Y_UNIT_TEST(ExplicitCompactionPriorityTracksBacklog) {
            TPriorityTestEnv env;
            const ui64 explicitSst = env.AddSst(0, 1);
            TTask task;
            for (const double previousPriority : {0.0, 100.0}) {
                task.MaxRatio = previousPriority;
                UNIT_ASSERT(env.Select(task, {explicitSst}) == NHullComp::ActCompactSsts);
                UNIT_ASSERT(task.SelectStrategy == NHullComp::ESelectStrategy::Explicit);
                UNIT_ASSERT_VALUES_EQUAL(task.MaxRatio, 1.0 / 16);
            }

            // The same explicit request is selected again while unrelated SSTs accumulate.
            env.AddSst(0, 32);
            UNIT_ASSERT(env.Select(task, {explicitSst}) == NHullComp::ActCompactSsts);
            UNIT_ASSERT(task.SelectStrategy == NHullComp::ESelectStrategy::Explicit);
            UNIT_ASSERT_VALUES_EQUAL(task.MaxRatio, 33.0 / 16);
            UNIT_ASSERT_VALUES_EQUAL(task.CompactSsts.CompactionChains.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(task.CompactSsts.CompactionChains.front()->Segments.front()->AssignedSstId,
                explicitSst);
            UNIT_ASSERT(!task.FullCompactionInfo.second);

            // Pressure on a different level must also raise the explicit request's priority.
            env.AddSst(17, 512);
            UNIT_ASSERT(env.Select(task, {explicitSst}) == NHullComp::ActCompactSsts);
            UNIT_ASSERT(task.SelectStrategy == NHullComp::ESelectStrategy::Explicit);
            UNIT_ASSERT_VALUES_EQUAL(task.MaxRatio, 4.0);

            task.Clear();
            UNIT_ASSERT_VALUES_EQUAL(task.MaxRatio, 0.0);
        }

        Y_UNIT_TEST(BalanceCompactionUsesCalculatedRanks) {
            TPriorityTestEnv env;
            env.AddSst(0, 16);
            TTask task;
            UNIT_ASSERT(env.Select(task) == NHullComp::ActCompactSsts);
            UNIT_ASSERT(task.SelectStrategy == NHullComp::ESelectStrategy::BalanceLevel);
            UNIT_ASSERT_VALUES_EQUAL(task.MaxRatio, 1.0);
            UNIT_ASSERT_VALUES_EQUAL(task.CompactSsts.TargetLevel, 1);
        }


        Y_UNIT_TEST(Test1) {
            STR << "Building LevelIndex\n";
            TIntrusivePtr<THullDs> ds = NTest::GenerateDs_17Level_Logs();
            STR << "Taking Snapshot\n";
            auto snap = ds->GetIndexSnapshot();


            // calculate storage ratio
            TIntrusivePtr<TBarriersSnapshot::TBarriersEssence> barriersEssence =
                snap.BarriersSnap.CreateEssence(snap.HullCtx);
            NHullComp::TStrategyStorageRatio<TKeyLogoBlob, TMemRecLogoBlob>
                (snap.HullCtx, snap.LogoBlobsSnap, std::move(barriersEssence), true).Work();

            snap.LogoBlobsSnap.Output(STR);
            STR << "\n";


            STR << "Building Boundaries\n";
            NHullComp::TBoundariesConstPtr boundaries(new NHullComp::TBoundaries(ChunkSize,
                        HullCompLevel0MaxSstsAtOnce, HullCompSortedPartsNum, Level0UseDreg));

            STR << "Selecting Strategy\n";
            TTask task;
            NHullComp::TSelectorParams params = {boundaries, 1.0, TInstant::Seconds(0), {}};
            TStrategy strategy(snap.HullCtx, params, std::move(snap.LogoBlobsSnap), std::move(snap.BarriersSnap),
                    &task, true);
            auto action = strategy.Select();
            STR << "action = " << NHullComp::ActionToStr(action) << "\n";
        }

    }

} // NKikimr

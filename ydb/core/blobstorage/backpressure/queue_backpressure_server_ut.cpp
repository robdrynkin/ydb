#include "queue_backpressure_server.h"
#include <ydb/core/blobstorage/vdisk/common/vdisk_config.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/stream/null.h>


#define STR Cnull
#define VERBOSE_STR Cnull


namespace NKikimr {

    Y_UNIT_TEST_SUITE(TQueueBackpressureTest) {

        using namespace NBackpressure;
        using TFeedback = ::NKikimr::NBackpressure::TFeedback<ui64>;

        Y_UNIT_TEST(CreateDelete) {
            TQueueBackpressure<ui64> qb(true, 100u, 10u);

            TInstant now = Now();
            TActorId actorId(1, 1, 1, 1);
            qb.Push(5, actorId, TMessageId(0, 0), 1, now);
            qb.Push(5, actorId, TMessageId(0, 1), 1, now);
            qb.Push(5, actorId, TMessageId(0, 2), 1, now);
            qb.Push(5, actorId, TMessageId(0, 3), 1, now);
            qb.Processed(actorId, TMessageId(0, 0), 1, now);
            qb.Push(5, actorId, TMessageId(0, 4), 1, now);
            qb.Processed(actorId, TMessageId(0, 2), 1, now);
            qb.Processed(actorId, TMessageId(0, 1), 1, now);
            qb.Push(5, actorId, TMessageId(0, 4), 1, now);
            qb.Output(STR, now);
        }


        Y_UNIT_TEST(IncorrectMessageId) {
            TQueueBackpressure<ui64> qb(true, 100u, 10u);

            TInstant now = Now();
            TActorId actorId(1, 1, 1, 1);
            qb.Push(5, actorId, TMessageId(0, 0), 1, now);
            qb.Push(5, actorId, TMessageId(0, 1), 1, now);
            auto feedback = qb.Push(5, actorId, TMessageId(0, 1), 1, now);
            TString res = "{Status# 4 Notify# 1 ActualWindowSize# 2 MaxWindowSize# 20 "
                                "ExpectedMsgId# [1 2] FailedMsgId# [0 1]}";
            TStringStream str;
            feedback.Output(str);
            STR << res << "\n";
            STR << str.Str() << "\n";
            UNIT_ASSERT_STRINGS_EQUAL(str.Str(), res);
        }

        struct TWindowNotificationStats {
            ui64 Requests = 0;
            ui64 Notifications = 0;
        };

        TWindowNotificationStats RunPeriodicClients(ui32 numClients) {
            const TVDiskConfig config(TVDiskConfig::TBaseInfo::SampleForTests());
            const ui64 totalCost = config.SkeletonFrontExtGetFast_TotalCost;
            const auto percentOfCost = [&](ui64 percent) { return totalCost * percent / 100; };
            TQueueBackpressure<ui64> queue(config.SkeletonFrontQueueBackpressureCheckMsgId, totalCost,
                percentOfCost(config.WindowCostChangeToRecalculatePercent),
                percentOfCost(config.WindowMinLowWatermarkPercent),
                percentOfCost(config.WindowMaxLowWatermarkPercent),
                config.WindowPercentThreshold,
                percentOfCost(config.WindowCostChangeUntilFrozenPercent),
                percentOfCost(config.WindowCostChangeUntilDeathPercent),
                config.WindowTimeout);

            // The default minimum window is 2% of the budget, reached at 50 clients.
            // Push and Processed both contribute to the cost-change countdown: a
            // request costs 0.2% of the budget, so 50 requests turn over the 20%
            // budget used to freeze idle clients. No special client or pause is needed.
            const ui64 requestCost = totalCost / 500;
            constexpr ui32 warmupRounds = 100;
            constexpr ui32 measuredRounds = 1000;
            const TDuration period = TDuration::MilliSeconds(10);
            const TDuration latency = TDuration::MilliSeconds(5);
            const TInstant start = TInstant::Seconds(1);
            TWindowNotificationStats stats;

            for (ui32 round = 0; round < warmupRounds + measuredRounds; ++round) {
                const bool measure = round >= warmupRounds;
                const TInstant roundStart = start + period * round;
                const auto account = [&](const TFeedback& feedback, const TActorId& actorId) {
                    UNIT_ASSERT_C(feedback.Good(), feedback.first.ToString());
                    for (const auto& update : feedback.second) {
                        UNIT_ASSERT(update.Notify);
                        UNIT_ASSERT(update.Status == NKikimrBlobStorage::TWindowFeedback::WindowUpdate);
                        UNIT_ASSERT(update.ActorId != actorId);
                    }
                    if (measure) {
                        // SkeletonFront::NotifyOtherClients sends one separate
                        // TEvVWindowChange for each entry; feedback.first goes in
                        // the ordinary response and must not be counted here.
                        stats.Notifications += feedback.second.size();
                    }
                };

                // Every client issues one request per period, with evenly spaced
                // phases in the first half of the period and a fixed response latency.
                // All clients have identical rates, costs and response delays. The
                // second half completes the same requests in order. This deliberately
                // exercises periodic waves, not a continuously saturated client set.
                for (ui32 client = 0; client < numClients; ++client) {
                    const TActorId actorId(1, 1, client + 1, 1);
                    const TInstant now = roundStart + TDuration::MicroSeconds(latency.MicroSeconds() * client / numClients);
                    const auto feedback = queue.Push(client, actorId, TMessageId(0, round), requestCost, now);
                    account(feedback, actorId);
                    if (measure) {
                        ++stats.Requests;
                    }
                }
                for (ui32 client = 0; client < numClients; ++client) {
                    const TActorId actorId(1, 1, client + 1, 1);
                    const TInstant now = roundStart + latency
                        + TDuration::MicroSeconds(latency.MicroSeconds() * client / numClients);
                    const auto feedback = queue.Processed(actorId, TMessageId(0, round), requestCost, now);
                    account(feedback, actorId);
                }
            }

            Cerr << "PeriodicClients clients=" << numClients
                << " requests=" << stats.Requests
                << " windowChangeNotifications=" << stats.Notifications
                << " notificationsPerRequest=" << double(stats.Notifications) / stats.Requests
                << " notificationsPerRoundPerClientSquared="
                << double(stats.Notifications) / measuredRounds / numClients / numClients << Endl;
            return stats;
        }

        Y_UNIT_TEST(WindowChangeNotificationsWithPeriodicClients) {
            // Characterize the current immediate-notification policy. Keep this
            // baseline when adding actor-level coverage for notification coalescing.
            const auto clients50 = RunPeriodicClients(50);
            const auto clients51 = RunPeriodicClients(51);
            const auto clients100 = RunPeriodicClients(100);
            const auto clients1000 = RunPeriodicClients(1000);

            Cerr << "PeriodicClients notificationGrowth100To1000="
                << double(clients1000.Notifications) / clients100.Notifications << Endl;

            UNIT_ASSERT_VALUES_EQUAL(clients50.Notifications, 0);
            UNIT_ASSERT_C(clients51.Notifications > clients51.Requests,
                "51 periodic clients should produce more than one notification per request");
            UNIT_ASSERT_C(clients100.Notifications > 2 * clients51.Notifications,
                "100 periodic clients should produce over twice as many notifications as 51 clients");
            // The amplification must also increase after normalizing for request
            // count, rather than only because the 100-client run sends more requests.
            UNIT_ASSERT_C(clients100.Notifications * clients51.Requests
                    > clients51.Notifications * clients100.Requests,
                "Notifications per request should increase from 51 to 100 clients");
            // This workload saturates rather than growing quadratically: additional
            // clients keep the window at its minimum for longer within each wave.
            // Preserve the measured 1000-client baseline instead of extrapolating
            // the increase observed between 51 and 100 clients.
            UNIT_ASSERT_VALUES_EQUAL(clients1000.Notifications, clients100.Notifications);
        }


        struct IClient : public virtual TThrRefBase {
            virtual TFeedback Work(TQueueBackpressure<ui64> &qb) = 0;
        };

        using IClientPtr = TIntrusivePtr<IClient>;

        struct TTrivialClient : public IClient {
            TTrivialClient(ui64 id)
                : Id(id)
                , MsgId()
                , State(true)
                , ActorId(1, 1, Id, 1)
            {}

            TFeedback Work(TQueueBackpressure<ui64> &qb) {
                TInstant now = Now();
                if (State) {
                    State = !State;
                    return qb.Push(Id, ActorId, MsgId, 1, now);
                } else {
                    State = !State;
                    const TMessageId res = MsgId;
                    ++MsgId.MsgId;
                    return qb.Processed(ActorId, res, 1, now);
                }
            }

            ui64 Id;
            TMessageId MsgId;
            bool State;
            TActorId ActorId;
        };

        struct TInFlightClient : public IClient {
            TInFlightClient(ui64 id, ui32 maxInFlight)
                : Id(id)
                , MsgId()
                , MaxInFlight(maxInFlight)
                , InFlight(0)
                , FullLoadObtained(false)
                , ActorId(1, 1, Id, 1)
            {}

            TFeedback Work(TQueueBackpressure<ui64> &qb) {
                TInstant now = Now();
                if (!FullLoadObtained) {
                    // full load
                    while (true) {
                        TFeedback res = qb.Push(Id, ActorId, MsgId, 1, now);
                        if (Good(res.first.Status)) {
                            VERBOSE_STR << "UNDERLOAD: Push OK MsgId# " << MsgId.ToString() << "\n";
                            InFlight++;
                            MsgId.MsgId++;
                            if (InFlight == MaxInFlight) {
                                FullLoadObtained = true;
                                return res;
                            }
                        } else {
                            VERBOSE_STR << "UNDERLOAD: Push FAILED\n";
                            return res;
                        }
                    }
                } else {
                    if (InFlight == MaxInFlight) {
                        const TMessageId temp(MsgId.SequenceId, MsgId.MsgId - InFlight);
                        auto res = qb.Processed(ActorId, temp, 1, now);
                        Y_ABORT_UNLESS(Good(res.first.Status));
                        VERBOSE_STR << "LOAD: Processed: MsgId# " << temp.ToString() << "\n";
                        InFlight--;
                        return res;
                    } else {
                        auto res = qb.Push(Id, ActorId, MsgId, 1, now);
                        Y_ABORT_UNLESS(Good(res.first.Status));
                        VERBOSE_STR << "LOAD: Push: MsgId# " << MsgId.ToString() << "\n";
                        InFlight++;
                        MsgId.MsgId++;
                        return res;
                    }
                }
            }

            ui64 Id;
            TMessageId MsgId;
            const ui32 MaxInFlight;
            ui32 InFlight;
            bool FullLoadObtained;
            TActorId ActorId;
        };


        Y_UNIT_TEST(PerfTrivial) {
            TQueueBackpressure<ui64> qb(true, 100u, 10u);

            TVector<IClientPtr> clients;
            ui32 i = 0;
            for (i = 0; i < 5; i++) {
                clients.emplace_back(new TTrivialClient(i));
            }

            TInstant now = Now();
            for (i = 0; i < 1000000; i++) {
                for (auto &c : clients) {
                    c->Work(qb);
                }
                if (i % 100000 == 0) {
                    STR << "=========================\n";
                    qb.Output(STR, now);
                }
            }


            for (i = 0; i < 5; i++) {
                auto feedback = clients[i]->Work(qb);
                TStringStream s;
                s << "{Status# 1 Notify# 0 "
                    << "ActualWindowSize# 1 MaxWindowSize# 20 ExpectedMsgId# [0 500001] FailedMsgId# [0 0]}";
                TString res = feedback.first.ToString();
                STR << res << "\n";
                UNIT_ASSERT(res == s.Str());
            }
        }

        Y_UNIT_TEST(PerfInFlight) {
            TQueueBackpressure<ui64> qb(true, 100u, 10u);

            TInstant now = Now();

            TVector<IClientPtr> clients;
            clients.emplace_back(new TInFlightClient(0, 30));
            clients.emplace_back(new TInFlightClient(1, 10));

            ui32 i = 0;
            for (i = 0; i < 1000000; i++) {
                for (auto &c : clients) {
                    c->Work(qb);
                }
                if (i % 100000 == 0) {
                    STR << "=========================\n";
                    qb.Output(STR, now);
                }
            }

            TStringStream s;
            qb.Output(s, now);
            TString res = "MaxCost# 100 ActualCost# 38 activeWindows# 2 fadingWindows# 0 "
                                "frozenWindows# 0 deadWindows# 0\n"
                            "GlobalStat: NSuccess# 1000038 NWindowUpdate# 400005 NProcessed# 1000000 "
                                "NIncorrectMsgId# 0 NHighWatermarkOverflow# 0\n"
                            "ClientId# 0 ExpectedMsgId# [0 500029] Cost# 29 LowWatermark# 20 HighWatermark# 76 "
                                "CostChangeUntilFrozenCountdown# 20 CostChangeUntilDeathCountdown# 30\n"
                            "ClientId# 1 ExpectedMsgId# [0 500009] Cost# 9 LowWatermark# 20 HighWatermark# 23 "
                                "CostChangeUntilFrozenCountdown# 20 CostChangeUntilDeathCountdown# 30\n";
            STR << s.Str() << "\n";
            UNIT_ASSERT_STRINGS_EQUAL(s.Str(), res);
        }
    }

} // NKikimr

#include "statestorage.h"

#include <ydb/core/protos/base.pb.h>
#include <ydb/library/actors/core/interconnect.h>
#include <ydb/library/actors/interconnect/events_local.h>
#include <ydb/library/actors/interconnect/interconnect_common.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/library/services/services.pb.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {
namespace {

// Exercise the real State Storage replicas/proxy and TCP Interconnect proxy.
// Control nameservice and handshake events instead of depending on TCP retries
// or wall-clock sleeps. The remote replica never acquires a working session.
class TUnavailableReplicaFixture {
public:
    TTestActorRuntimeBase Runtime{2};
    TActorId Nameserver;
    TActorId Handshake;
    TActorId Edge;
    TActorId ICProxy;
    TActorId StorageProxy;
    const TDuration PendingTimeout = TDuration::Seconds(1);
    const TDuration ErrorSleep = TDuration::Seconds(10);

    TUnavailableReplicaFixture() {
        Runtime.SetUseRealInterconnect();
        Runtime.SetICCommonSetupper([this](ui32, TIntrusivePtr<TInterconnectProxyCommon> common) {
            common->NameserviceId = TActorId(0, "testnamesvc");
            common->Settings.MessagePendingTimeout = PendingTimeout;
            common->Settings.FirstErrorSleep = ErrorSleep;
            common->Settings.MaxErrorSleep = ErrorSleep;
        });
        Runtime.Initialize();
        for (ui32 i = 0; i < Runtime.GetNodeCount(); ++i) {
            Runtime.GetLogSettings(i)->Append(NKikimrServices::EServiceKikimr_MIN,
                NKikimrServices::EServiceKikimr_MAX,
                [](int component) { return NKikimrServices::EServiceKikimr_Name(component); });
        }

        Nameserver = Runtime.AllocateEdgeActor();
        Runtime.RegisterService(TActorId(0, "testnamesvc"), Nameserver);
        Handshake = Runtime.AllocateEdgeActor();
        Edge = Runtime.AllocateEdgeActor();
        ICProxy = Runtime.GetInterconnectProxy(0, 1);
        Runtime.SetScheduledEventFilter([this](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event,
                                               TDuration, TInstant&) {
            return event->GetRecipientRewrite() != ICProxy;
        });
        Runtime.SetDispatchTimeout(TDuration::Seconds(5));

        auto info = MakeIntrusive<TStateStorageInfo>();
        info->NToSelect = 5;
        info->Rings.resize(5);
        for (ui32 i = 0; i < 4; ++i) {
            info->Rings[i].Replicas.push_back(Runtime.Register(CreateStateStorageReplica(info, i)));
        }
        info->Rings[4].Replicas.push_back(TActorId(Runtime.GetNodeId(1), "missing-ss"));
        StorageProxy = Runtime.Register(CreateStateStorageProxy(info, nullptr, nullptr));
    }

    void StartHandshake() {
        auto request = MakeHolder<TEvHandshakeRequest>();
        request->Record.SetSerial(++Serial);
        Runtime.SendAsync(new IEventHandle(ICProxy, Handshake, request.Release()));
        auto getNode = Runtime.GrabEdgeEvent<TEvInterconnect::TEvGetNode>(Nameserver, TDuration::Seconds(1));
        UNIT_ASSERT(getNode);
        UNIT_ASSERT_VALUES_EQUAL(getNode->Get()->NodeId, Runtime.GetNodeId(1));
    }

    void ConfigureHandshake() {
        auto response = MakeHolder<TEvInterconnect::TEvNodeInfo>(Runtime.GetNodeId(1));
        response->Node = MakeHolder<TEvInterconnect::TNodeInfo>(
            Runtime.GetNodeId(1), "::1", "unavailable", "::1", 1, TNodeLocation());
        Runtime.SendAsync(new IEventHandle(ICProxy, Nameserver, response.Release()));
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvHandshakeReplyOK>(Handshake, TDuration::Seconds(1)));
    }

    void FailHandshake() {
        Runtime.SendAsync(new IEventHandle(ICProxy, Handshake, new TEvHandshakeFail(
            TEvHandshakeFail::HANDSHAKE_FAIL_PERMANENT, "unavailable State Storage node")));
    }

    TDuration Lookup(TEvStateStorage::TProxyOptions::ESigWaitMode mode) {
        const ui64 tabletId = ++TabletId;
        const auto started = Runtime.GetCurrentMonotonicTime();
        Runtime.SendAsync(new IEventHandle(StorageProxy, Edge, new TEvStateStorage::TEvLookup(
            tabletId, tabletId, TEvStateStorage::TProxyOptions(mode))));
        auto reply = Runtime.GrabEdgeEvent<TEvStateStorage::TEvInfo>(Edge, TDuration::Seconds(2));
        UNIT_ASSERT_C(reply, "State Storage lookup did not complete");
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->TabletID, tabletId);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->Cookie, tabletId);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->Status, NKikimrProto::NODATA);
        UNIT_ASSERT_VALUES_EQUAL(reply->Get()->SignatureSz, 5);
        return Runtime.GetCurrentMonotonicTime() - started;
    }

    void Elapse(TDuration duration) {
        Runtime.Schedule(new IEventHandle(Edge, Edge, new TEvents::TEvWakeup()), duration);
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvents::TEvWakeup>(Edge, duration + TDuration::Seconds(1)));
    }

private:
    ui64 Serial = 0;
    ui64 TabletId = 100;
};

void CheckFailFast(TEvStateStorage::TProxyOptions::ESigWaitMode mode) {
    TUnavailableReplicaFixture env;
    env.StartHandshake();
    env.ConfigureHandshake();

    // A quorum of local NODATA replies is insufficient: wait for nondelivery
    // from the remote replica's pending connection.
    UNIT_ASSERT_VALUES_EQUAL(env.Lookup(mode), env.PendingTimeout);

    env.FailHandshake();
    UNIT_ASSERT_VALUES_EQUAL(env.Lookup(mode), TDuration::Zero());

    // An incoming retry may move the proxy out of HoldByError before its timer
    // expires. The remembered error must still reject every new lookup, both
    // while configuring and while waiting for the handshake to finish.
    env.StartHandshake();
    for (ui32 i = 0; i < 3; ++i) {
        UNIT_ASSERT_VALUES_EQUAL(env.Lookup(mode), TDuration::Zero());
    }
    env.ConfigureHandshake();
    for (ui32 i = 0; i < 3; ++i) {
        UNIT_ASSERT_VALUES_EQUAL(env.Lookup(mode), TDuration::Zero());
    }
}

} // namespace

Y_UNIT_TEST_SUITE(TStateStorageInterconnect) {
    Y_UNIT_TEST(BootstrapLookupFailsFastAfterConnectionError) {
        CheckFailFast(TEvStateStorage::TProxyOptions::SigAsync);
    }

    Y_UNIT_TEST(ResolverLookupFailsFastAfterConnectionError) {
        CheckFailFast(TEvStateStorage::TProxyOptions::SigNone);
    }

    Y_UNIT_TEST(MessageTimeoutDoesNotMakeSubsequentLookupsFailFast) {
        TUnavailableReplicaFixture env;
        env.StartHandshake();
        env.ConfigureHandshake();

        // Expiring one message does not mean the handshake has failed.
        for (ui32 i = 0; i < 3; ++i) {
            UNIT_ASSERT_VALUES_EQUAL(env.Lookup(TEvStateStorage::TProxyOptions::SigAsync), env.PendingTimeout);
        }
    }

    Y_UNIT_TEST(ErrorBackoffExpiryAllowsAnotherSlowLookup) {
        TUnavailableReplicaFixture env;
        env.StartHandshake();
        env.ConfigureHandshake();
        UNIT_ASSERT_VALUES_EQUAL(env.Lookup(TEvStateStorage::TProxyOptions::SigAsync), env.PendingTimeout);

        env.FailHandshake();
        UNIT_ASSERT_VALUES_EQUAL(env.Lookup(TEvStateStorage::TProxyOptions::SigAsync), TDuration::Zero());
        env.Elapse(env.ErrorSleep + TDuration::MilliSeconds(1));

        // PendingActivation clears the error: a new attempt can queue again.
        env.StartHandshake();
        env.ConfigureHandshake();
        UNIT_ASSERT_VALUES_EQUAL(env.Lookup(TEvStateStorage::TProxyOptions::SigAsync), env.PendingTimeout);
    }
}

} // namespace NKikimr

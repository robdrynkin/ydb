#pragma once

#include <library/cpp/lwtrace/all.h>
#include <util/generic/hash.h>
#include <util/generic/vector.h>

namespace NActors::NTest {

// A bounded regular log: tests do not need to attach an Orbit to their events.
class TProbeLog {
public:
    struct TRow {
        TString Probe;
        THashMap<TString, TString> Params;
    };

    TProbeLog(const TString& provider, std::initializer_list<TString> probes)
        : Manager(*Singleton<NLWTrace::TProbeRegistry>(), false)
    {
        NLWTrace::TQuery query;
        query.SetPerThreadLogSize(4096);
        for (const auto& probe : probes) {
            auto* block = query.AddBlocks();
            block->MutableProbeDesc()->SetName(probe);
            block->MutableProbeDesc()->SetProvider(provider);
            block->AddAction()->MutableLogAction()->SetLogTimestamp(true);
        }
        Manager.New("test", query);
    }

    TVector<TRow> Read() {
        Manager.Stop("test");
        TReader reader;
        Manager.ReadLog("test", reader);
        return std::move(reader.Rows);
    }

private:
    struct TReader {
        TVector<TRow> Rows;

        void Push(TThread::TId, const NLWTrace::TLogItem& item) {
            TRow row{item.Probe->Event.Name, {}};
            TString values[LWTRACE_MAX_PARAMS];
            item.Probe->Event.Signature.SerializeParams(item.Params, values);
            for (size_t i = 0; i < item.SavedParamsCount; ++i) {
                row.Params.emplace(item.Probe->Event.Signature.ParamNames[i], values[i]);
            }
            Rows.push_back(std::move(row));
        }
    };

    NLWTrace::TManager Manager;
};

} // namespace NActors::NTest

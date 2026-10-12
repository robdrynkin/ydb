# BS_QUEUE diagnostics

Server counters live under the existing VDisk SkeletonFront group:

- `SkeletonFront/<queue>/ActiveClients`: ACTIVE + FADING participants in the last window recalculation; updated at recalculation, removed when the queue is destroyed.
- `SkeletonFront/<queue>/FrozenTransitions`: cumulative transitions into FROZEN, including repeated freezing after reactivation.
- `SkeletonFront/<queue>/WindowChangeSent` and `DropConnectionSent`: standalone window updates and disconnect notifications submitted for delivery.

Client counters `WindowChangeReceived` and `DropConnectionReceived` use the existing `queue=<name>` group. Sent and received counters measure different points; their difference is not a loss detector.

## LWTrace

The `BLOBSTORAGE_PROVIDER` probes `BSQueueServerOperation` and `BSQueueClientState` belong to group `BSQueue`. They work with ordinary log queries, without an Orbit on requests. For example, use this query in the node's LWTrace monitoring interface:

```protobuf
PerThreadLogSize: 4096
Blocks {
  ProbeDesc { Group: "BSQueue" }
  Action { LogAction { LogTimestamp: true MaxRecords: 10000 } }
}
```

Each server record describes one request or completion. `active/fading/frozen/totalCost/low` describe the recalculation's input population and computed Low; they are valid only when `recalculated=true`. Counts are taken before fading and adjusting windows. `totalCost` is actual cost, before the zero-cost fallback to MaxCost. `clientLow/clientHigh` are post-operation watermarks (zero if the window was removed); `clientCost` is the operation's feedback cost. `notifications` counts prepared feedback entries, not actual sends. Server `status` is `TWindowFeedback::EStatus`; deadline and error completions are distinguished by `operation`.

Client records are taken after enqueue or response processing and before the resulting Pump. They include local Waiting/InFlight counts and bytes, cost, window, connection state and pause flags. `ignored_response` covers replies rejected before queue processing; standalone updates and disconnects have their own operation names. `status` is a reply status for responses, a window feedback status for `window_change`, or UINT32_MAX when unavailable. `previousWindow` is the value on handler entry. Cost/window fields use cost-model units, not bytes. State numbers follow `TVDiskBackpressureClientActor::EState`.

`ACTORLIB_PROVIDER.ForwardEvent` (enqueue for transmission) and `ReceiveEvent` (validated incoming event, before local delivery) describe classic TCP interconnect events, including channel and serialized size. They do not measure socket transmission time. `EventDispatch` records executor delivery before the handler, with original and rewritten event type. Actor IDs are represented by their raw words plus node/pool IDs.

To select outgoing window notifications, use the query below. For reception or actor dispatch, replace `ForwardEvent` with `ReceiveEvent` or `EventDispatch`. `268633608` is `TEvBlobStorage::EvVWindowChange`; verify the enum value when using a different source version.

```protobuf
PerThreadLogSize: 4096
Blocks {
  ProbeDesc { Provider: "ACTORLIB_PROVIDER" Name: "ForwardEvent" }
  Predicate {
    Operators {
      Type: OT_EQ
      Argument { Param: "type" }
      Argument { Value: "268633608" }
    }
  }
  Action { LogAction { LogTimestamp: true MaxRecords: 10000 } }
}
```

Use a bounded log and queue/type filters for busy nodes. The per-event probes are inactive until a query enables them. Enabling them does not change the BS_QUEUE protocol or interconnect channels.

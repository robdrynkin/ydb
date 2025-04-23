#include <ydb/core/base/blobstorage.h>


namespace NKikimr::NBlobDepot {
    struct TEvKVPutResult : public TEventLocal<TEvKVPutResult, NKikimr::TEvBlobStorage::EvKVPutResult> {
        NKikimrProto::EReplyStatus Status;
        const TString Key;
        TString ErrorReason;
        std::shared_ptr<TEvBlobStorage::TExecutionRelay> ExecutionRelay;

        TEvKVPutResult(NKikimrProto::EReplyStatus status, const TString& key)
            : Status(status)
            , Key(key)
        {}


        TString Print(bool isFull) const {
            Y_UNUSED(isFull);
            TStringStream str;
            str << "TEvPutResult {Key# " << Key;
            str << " Status# " << NKikimrProto::EReplyStatus_Name(Status).data();
            if (ErrorReason.size()) {
                str << " ErrorReason# \"" << ErrorReason << "\"";
            }
            str << "}";
            return str.Str();
        }

        TString ToString() const {
            return Print(false);
        }
    };

    struct TEvKVPut : public TEventLocal<TEvKVPut, NKikimr::TEvBlobStorage::EvKVPut> {
        const TString Key;
        const TRcBuf Buffer;
        const TInstant Deadline;
        std::shared_ptr<TEvBlobStorage::TExecutionRelay> ExecutionRelay;

        TEvKVPut(const TString &key, TRcBuf &&buffer, TInstant deadline)
            : Key(key)
            , Buffer(std::move(buffer))
            , Deadline(deadline)
        {
        }

        TEvKVPut(const TString &key, const TString &buffer, TInstant deadline)
            : TEvKVPut(key, TRcBuf(buffer), deadline)
        {}


        TString Print(bool isFull) const {
            TStringStream str;
            str << "TEvPut {Key# " << Key;
            str << " Size# " << Buffer.GetSize();
            if (isFull) {
                str << " Buffer# " << Buffer.ExtractUnderlyingContainerOrCopy<TString>().Quote();
            }
            str << " Deadline# " << Deadline.MilliSeconds();
            str << "}";
            return str.Str();
        }

        TString ToString() const {
            return Print(false);
        }

        ui32 CalculateSize() const {
            return sizeof(*this) + Buffer.GetSize();
        }

        std::unique_ptr<TEvKVPutResult> MakeErrorResponse(NKikimrProto::EReplyStatus status, const TString& errorReason, TGroupId groupId) {
            Y_UNUSED(groupId);
            auto response = std::make_unique<TEvKVPutResult>(status, Key);
            response->ErrorReason = errorReason;
            response->ExecutionRelay = std::move(ExecutionRelay);
            return response;
        }
    };
}

#include "blobstorage_groupinfo.h"

namespace NKikimr {

class TDataIntegrityCheckerBase : public TBlobStorageGroupInfo::IDataIntegrityChecker {
protected:
    const TBlobStorageGroupInfo::TTopology *Top;

public:
    explicit TDataIntegrityCheckerBase(const TBlobStorageGroupInfo::TTopology *top)
        : Top(top)
    {}
};

class TDataIntegrityCheckerTrivial : public TDataIntegrityCheckerBase {
public:
    using TDataIntegrityCheckerBase::TDataIntegrityCheckerBase;

    TPartsState GetDataState(const TLogoBlobID& id, const TPartsData& partsData, char separator) const override {
        Y_UNUSED(id);
        Y_UNUSED(partsData);
        Y_UNUSED(separator);
        return {};
    }
};

class TDataIntegrityCheckerParityBlock : public TDataIntegrityCheckerBase {
public:
    using TDataIntegrityCheckerBase::TDataIntegrityCheckerBase;

    TPartsState GetDataState(const TLogoBlobID& id, const TPartsData& partsData, char separator) const override {
        const auto& type = Top->GType;
        const ui32 totalParts = type.TotalPartCount();
        const ui32 dataParts = type.DataParts();
        const auto crcMode = static_cast<TErasureType::ECrcMode>(id.CrcMode());
        const ui64 partSize = type.PartSize(TLogoBlobID(id, 1));
        TPartsState state;
        TStringStream report;
        auto error = [&](const TString& message) {
            state.IsOk = false;
            report << "ERROR: " << message << separator;
        };
        if (partsData.Parts.size() != totalParts) {
            error("incorrect number of part slots");
            state.DataInfo = report.Str();
            return state;
        }

        TStackVec<TRope, TErasureType::MaxTotalParts> basis(totalParts);
        ui32 count = 0;
        ui32 restoreMask = 0;
        report << "Layout info:" << separator;
        for (ui32 part = 0; part < totalParts; ++part) {
            const auto& copies = partsData.Parts[part];
            report << "part " << part + 1 << " disks [ ";
            for (const auto& [disk, data] : copies) {
                report << disk << " ";
                if (data.size() != partSize) {
                    error(TStringBuilder() << "incorrect size for part " << part + 1 << " on disk " << disk);
                } else if (!CheckCrcAtTheEnd(crcMode, data)) {
                    error(TStringBuilder() << "incorrect CRC for part " << part + 1 << " on disk " << disk);
                } else if (TRope::Compare(data, copies.front().second)) {
                    error(TStringBuilder() << "unequal copies of part " << part + 1);
                }
            }
            report << "]" << separator;
            if (!copies.empty()) {
                if (count++ < dataParts) {
                    basis[part] = copies.front().second;
                } else {
                    restoreMask |= 1u << part;
                }
            }
        }
        if (state.IsOk && restoreMask && partSize) {
            ErasureRestore(crcMode, type, id.BlobSize(), nullptr, basis, restoreMask);
            report << "Erasure info:" << separator;
            for (ui32 part = 0; part < totalParts; ++part) {
                if (restoreMask & (1u << part)) {
                    if (TRope::Compare(basis[part], partsData.Parts[part].front().second)) {
                        error(TStringBuilder() << "erasure mismatch for part " << part + 1);
                    } else {
                        report << "CHECK part " << part + 1 << " -> OK" << separator;
                    }
                }
            }
        }
        state.DataInfo = report.Str();
        return state;
    }
};

class TDataIntegrityCheckerMirror : public TDataIntegrityCheckerBase {
private:
    virtual ui32 DataPartsCount() const = 0;

public:
    using TDataIntegrityCheckerBase::TDataIntegrityCheckerBase;

    TPartsState GetDataState(const TLogoBlobID& id, const TPartsData& partsData, char separator) const override {
        Y_UNUSED(id);
        Y_ABORT_UNLESS(partsData.Parts.size() == 3);

        TPartsState partsState;

        struct TSeenPart {
            TRope Data;
            std::vector<ui32> DiskIdxs;
        };
        std::vector<TSeenPart> seenParts;

        // find all distinct copies of the blob
        for (ui32 partId = 0; partId < DataPartsCount(); ++partId) {
            for (const auto& [diskIdx, data] : partsData.Parts[partId]) {
                bool isNew = true;
                for (auto& seenPart : seenParts) {
                    if (!TRope::Compare(data, seenPart.Data)) {
                        seenPart.DiskIdxs.push_back(diskIdx);
                        isNew = false;
                        break;
                    }
                }
                if (isNew) {
                    seenParts.push_back({data, {diskIdx}});
                }
            }
        }

        TStringStream layoutReport;
        layoutReport << "Layout info:" << separator;

        TStringStream str;
        bool hasUnequalParts = (seenParts.size() > 1);
        ui32 ver = 0;
        for (const auto& seenPart : seenParts) {
            if (ver > 0) {
                str << ", ";
            }
            str << "ver" << ver << " disks [ ";
            for (const auto& diskIdx : seenPart.DiskIdxs) {
                str << diskIdx << " ";
            }
            str << "]";
            ++ver;
        }
        str << separator;
        layoutReport << str.Str();

        if (hasUnequalParts) {
            partsState.IsOk = false;
            layoutReport << "ERROR: There are unequal parts" << separator;
        }
        partsState.DataInfo = layoutReport.Str();

        return partsState;
    }
};

class TDataIntegrityCheckerMirror3dc : public TDataIntegrityCheckerMirror {
private:
    ui32 DataPartsCount() const override { return 3; }

public:
    using TDataIntegrityCheckerMirror::TDataIntegrityCheckerMirror;
};

class TDataIntegrityCheckerMirror3of4 : public TDataIntegrityCheckerMirror {
private:
    ui32 DataPartsCount() const override { return 2; }

public:
    using TDataIntegrityCheckerMirror::TDataIntegrityCheckerMirror;
};

} // NKikimr

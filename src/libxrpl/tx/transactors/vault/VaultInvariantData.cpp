#include <xrpl/tx/transactors/vault/VaultInvariantData.h>
//
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STNumber.h>

namespace xrpl {

VaultInvariantData::Vault
VaultInvariantData::Vault::make(SLE const& from)
{
    XRPL_ASSERT(from.getType() == ltVAULT, "VaultInvariantData::Vault::make : from Vault object");

    VaultInvariantData::Vault self;
    self.key = from.key();
    self.asset = from.at(sfAsset);
    self.pseudoId = from.getAccountID(sfAccount);
    self.owner = from.at(sfOwner);
    self.shareMPTID = from.getFieldH192(sfShareMPTID);
    self.assetsTotal = from.at(sfAssetsTotal);
    self.assetsAvailable = from.at(sfAssetsAvailable);
    self.assetsMaximum = from.at(sfAssetsMaximum);
    self.lossUnrealized = from.at(sfLossUnrealized);
    return self;
}

VaultInvariantData::Shares
VaultInvariantData::Shares::make(SLE const& from)
{
    XRPL_ASSERT(
        from.getType() == ltMPTOKEN_ISSUANCE,
        "VaultInvariantData::Shares::make : from MPTokenIssuance object");

    VaultInvariantData::Shares self;
    self.share = MPTIssue(makeMptID(from.getFieldU32(sfSequence), from.getAccountID(sfIssuer)));
    self.sharesTotal = from.at(sfOutstandingAmount);
    self.sharesMaximum = from[~sfMaximumAmount].value_or(maxMPTokenAmount);
    return self;
}

void
VaultInvariantData::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    // If `before` is empty, this means an object is being created,  in which
    // case `isDelete` must be false. Otherwise `before` and `after` are set and
    // `isDelete` indicates whether an object is being deleted or modified.
    XRPL_ASSERT(
        after != nullptr && (before != nullptr || !isDelete),
        "xrpl::ValidVault::visitEntry : some object is available");

    // Number balanceDelta will capture the difference (delta) between "before"
    // state (zero if created) and "after" state (zero if destroyed), so the
    // invariants can validate that the change in account balances matches the
    // change in vault balances, stored to deltas_ at the end of this function.
    Number balanceDelta{};

    std::int8_t sign = 0;
    if (before)
    {
        switch (before->getType())
        {
            case ltVAULT:
                beforeVault_.push_back(Vault::make(*before));
                break;
            case ltMPTOKEN_ISSUANCE:
                // At this moment we have no way of telling if this object holds
                // vault shares or something else. Save it for finalize.
                beforeMPTs_.push_back(Shares::make(*before));
                balanceDelta = static_cast<std::int64_t>(before->getFieldU64(sfOutstandingAmount));
                sign = 1;
                break;
            case ltMPTOKEN:
                balanceDelta = static_cast<std::int64_t>(before->getFieldU64(sfMPTAmount));
                sign = -1;
                break;
            case ltACCOUNT_ROOT:
            case ltRIPPLE_STATE:
                balanceDelta = before->getFieldAmount(sfBalance);
                sign = -1;
                break;
            default:;
        }
    }

    if (!isDelete && after)
    {
        switch (after->getType())
        {
            case ltVAULT:
                afterVault_.push_back(Vault::make(*after));
                break;
            case ltMPTOKEN_ISSUANCE:
                // At this moment we have no way of telling if this object holds
                // vault shares or something else. Save it for finalize.
                afterMPTs_.push_back(Shares::make(*after));
                balanceDelta -=
                    Number(static_cast<std::int64_t>(after->getFieldU64(sfOutstandingAmount)));
                sign = 1;
                break;
            case ltMPTOKEN:
                balanceDelta -= Number(static_cast<std::int64_t>(after->getFieldU64(sfMPTAmount)));
                sign = -1;
                break;
            case ltACCOUNT_ROOT:
            case ltRIPPLE_STATE:
                balanceDelta -= Number(after->getFieldAmount(sfBalance));
                sign = -1;
                break;
            default:;
        }
    }

    uint256 const key = (before ? before->key() : after->key());
    // Append to deltas if sign is non-zero, i.e. an object of an interesting
    // type has been updated. A transaction may update an object even when
    // its balance has not changed, e.g. transaction fee equals the amount
    // transferred to the account. We intentionally do not compare balanceDelta
    // against zero, to avoid missing such updates.
    if (sign != 0)
        deltas_[key] = balanceDelta * sign;
}

void
VaultInvariantData::clear()
{
    afterVault_.clear();
    afterMPTs_.clear();
    beforeVault_.clear();
    beforeMPTs_.clear();
    deltas_.clear();
}

std::optional<Number>
VaultInvariantData::deltaAssets(Asset const& vaultAsset, AccountID const& id) const
{
    auto const get =  //
        [&](auto const& it, std::int8_t sign = 1) -> std::optional<Number> {
        if (it == deltas_.end())
            return std::nullopt;

        return it->second * sign;
    };

    return std::visit(
        [&]<typename TIss>(TIss const& issue) {
            if constexpr (std::is_same_v<TIss, Issue>)
            {
                if (isXRP(issue))
                    return get(deltas_.find(keylet::account(id).key));
                return get(
                    deltas_.find(keylet::line(id, issue).key), id > issue.getIssuer() ? -1 : 1);
            }
            else if constexpr (std::is_same_v<TIss, MPTIssue>)
            {
                return get(deltas_.find(keylet::mptoken(issue.getMptID(), id).key));
            }
        },
        vaultAsset.value());
}

std::optional<Number>
VaultInvariantData::deltaAssetsTxAccount(STTx const& tx, Asset const& vaultAsset, XRPAmount fee)
    const
{
    auto ret = deltaAssets(vaultAsset, tx[sfAccount]);
    // Nothing returned or not XRP transaction
    if (!ret.has_value() || !vaultAsset.native())
        return ret;

    // Delegated transaction; no need to compensate for fees
    if (auto const delegate = tx[~sfDelegate]; delegate.has_value() && *delegate != tx[sfAccount])
        return ret;

    *ret += fee.drops();
    if (*ret == zero)
        return std::nullopt;

    return ret;
}

std::optional<Number>
VaultInvariantData::deltaShares(
    AccountID const& pseudoId,
    uint192 const& shareMPTID,
    AccountID const& id) const
{
    auto const it = [&]() {
        if (id == pseudoId)
            return deltas_.find(keylet::mptIssuance(shareMPTID).key);
        return deltas_.find(keylet::mptoken(shareMPTID, id).key);
    }();

    return it != deltas_.end() ? std::optional<Number>(it->second) : std::nullopt;
}

std::optional<VaultInvariantData::Shares>
VaultInvariantData::resolveUpdatedShares(Vault const& afterVault, ReadView const& view) const
{
    // At this moment we only know that a vault is being updated and there
    // might be some MPTokenIssuance objects which are also updated in the
    // same transaction. Find the one matching the shares to this vault.
    // Note, we expect updatedMPTs collection to be extremely small. For
    // such collections linear search is faster than lookup.
    for (auto const& e : afterMPTs_)
    {
        if (e.share.getMptID() == afterVault.shareMPTID)
            return e;
    }

    auto const sleShares = view.read(keylet::mptIssuance(afterVault.shareMPTID));

    return sleShares ? std::optional<Shares>(Shares::make(*sleShares)) : std::nullopt;
}

std::optional<VaultInvariantData::Shares>
VaultInvariantData::resolveBeforeShares(Vault const& beforeVault) const
{
    for (auto const& e : beforeMPTs_)
    {
        if (e.share.getMptID() == beforeVault.shareMPTID)
            return std::move(e);
    }
    return std::nullopt;
}

bool
VaultInvariantData::vaultHoldsNoAssets(Vault const& vault)
{
    return vault.assetsAvailable == 0 && vault.assetsTotal == 0;
}

}  // namespace xrpl

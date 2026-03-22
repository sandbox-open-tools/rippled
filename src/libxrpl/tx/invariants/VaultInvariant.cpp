#include <xrpl/tx/invariants/VaultInvariant.h>
//
#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/tx/invariants/InvariantCheckPrivilege.h>

namespace xrpl {

ValidVault::Vault
ValidVault::Vault::make(SLE const& from)
{
    XRPL_ASSERT(from.getType() == ltVAULT, "ValidVault::Vault::make : from Vault object");

    ValidVault::Vault self;
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

ValidVault::Shares
ValidVault::Shares::make(SLE const& from)
{
    XRPL_ASSERT(
        from.getType() == ltMPTOKEN_ISSUANCE,
        "ValidVault::Shares::make : from MPTokenIssuance object");

    ValidVault::Shares self;
    self.share = MPTIssue(makeMptID(from.getFieldU32(sfSequence), from.getAccountID(sfIssuer)));
    self.sharesTotal = from.at(sfOutstandingAmount);
    self.sharesMaximum = from[~sfMaximumAmount].value_or(maxMPTokenAmount);
    return self;
}

void
ValidVault::visitEntry(
    bool isDelete,
    std::shared_ptr<SLE const> const& before,
    std::shared_ptr<SLE const> const& after)
{
    // If `before` is empty, this means an object is being created, in which
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

bool
ValidVault::finalize(
    STTx const& tx,
    TER const ret,
    XRPAmount const fee,
    ReadView const& view,
    beast::Journal const& j)
{
    bool const enforce = view.rules().enabled(featureSingleAssetVault);

    if (!isTesSuccess(ret))
        return true;  // Do not perform checks

    if (afterVault_.empty() && beforeVault_.empty())
    {
        if (hasPrivilege(tx, mustModifyVault))
        {
            JLOG(j.fatal()) <<  //
                "Invariant failed: vault operation succeeded without modifying a vault";
            XRPL_ASSERT(enforce, "xrpl::ValidVault::finalize : vault noop invariant");
            return !enforce;
        }

        return true;  // Not a vault operation
    }
    if (!(hasPrivilege(tx, mustModifyVault) || hasPrivilege(tx, mayModifyVault)))
    {
        JLOG(j.fatal()) << "Invariant failed: vault updated by a wrong transaction type";
        XRPL_ASSERT(
            enforce,
            "xrpl::ValidVault::finalize : illegal vault transaction "
            "invariant");
        return !enforce;  // Also not a vault operation
    }

    if (beforeVault_.size() > 1 || afterVault_.size() > 1)
    {
        JLOG(j.fatal()) << "Invariant failed: vault operation updated more than single vault";
        XRPL_ASSERT(enforce, "xrpl::ValidVault::finalize : single vault invariant");
        return !enforce;  // That's all we can do here
    }

    auto const txnType = tx.getTxnType();

    // ttVAULT_DELETE has no after state — its invariants are in the transactor.
    // Other tx types without an after state means a vault was illegally deleted.
    if (afterVault_.empty())
    {
        if (txnType == ttVAULT_DELETE)
            return true;

        JLOG(j.fatal()) << "Invariant failed: vault deleted by a wrong transaction type";
        XRPL_ASSERT(
            enforce,
            "xrpl::ValidVault::finalize : illegal vault deletion "
            "invariant");
        return !enforce;
    }

    auto const& afterVault = afterVault_[0];
    XRPL_ASSERT(
        beforeVault_.empty() || beforeVault_[0].key == afterVault.key,
        "xrpl::ValidVault::finalize : single vault operation");

    auto const updatedShares = [&]() -> std::optional<Shares> {
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
    }();

    bool result = true;

    // Universal transaction checks
    if (!beforeVault_.empty())
    {
        auto const& beforeVault = beforeVault_[0];
        if (afterVault.asset != beforeVault.asset || afterVault.pseudoId != beforeVault.pseudoId ||
            afterVault.shareMPTID != beforeVault.shareMPTID)
        {
            JLOG(j.fatal()) << "Invariant failed: violation of vault immutable data";
            result = false;
        }
    }

    if (!updatedShares)
    {
        JLOG(j.fatal()) << "Invariant failed: updated vault must have shares";
        XRPL_ASSERT(enforce, "xrpl::ValidVault::finalize : vault has shares invariant");
        return !enforce;  // That's all we can do here
    }

    if (updatedShares->sharesTotal == 0)
    {
        if (afterVault.assetsTotal != zero)
        {
            JLOG(j.fatal()) <<  //
                "Invariant failed: updated zero sized vault must have no assets outstanding";
            result = false;
        }
        if (afterVault.assetsAvailable != zero)
        {
            JLOG(j.fatal()) <<  //
                "Invariant failed: updated zero sized vault must have no assets available";
            result = false;
        }
    }
    else if (updatedShares->sharesTotal > updatedShares->sharesMaximum)
    {
        JLOG(j.fatal())  //
            << "Invariant failed: updated shares must not exceed maximum "
            << updatedShares->sharesMaximum;
        result = false;
    }

    if (afterVault.assetsAvailable < zero)
    {
        JLOG(j.fatal()) << "Invariant failed: assets available must be positive";
        result = false;
    }

    if (afterVault.assetsAvailable > afterVault.assetsTotal)
    {
        JLOG(j.fatal()) <<  //
            "Invariant failed: assets available must not be greater than assets outstanding";
        result = false;
    }
    else if (afterVault.lossUnrealized > afterVault.assetsTotal - afterVault.assetsAvailable)
    {
        JLOG(j.fatal())  //
            << "Invariant failed: loss unrealized must not exceed "
               "the difference between assets outstanding and available";
        result = false;
    }

    if (afterVault.assetsTotal < zero)
    {
        JLOG(j.fatal()) << "Invariant failed: assets outstanding must be positive";
        result = false;
    }

    if (afterVault.assetsMaximum < zero)
    {
        JLOG(j.fatal()) << "Invariant failed: assets maximum must be positive";
        result = false;
    }

    // Thanks to this check we can simply do `assert(!beforeVault_.empty()` when
    // enforcing invariants on transaction types other than ttVAULT_CREATE
    if (beforeVault_.empty() && txnType != ttVAULT_CREATE)
    {
        JLOG(j.fatal()) <<  //
            "Invariant failed: vault created by a wrong transaction type";
        XRPL_ASSERT(enforce, "xrpl::ValidVault::finalize : vault creation invariant");
        return !enforce;  // That's all we can do here
    }

    if (!beforeVault_.empty() && afterVault.lossUnrealized != beforeVault_[0].lossUnrealized &&
        txnType != ttLOAN_MANAGE && txnType != ttLOAN_PAY)
    {
        JLOG(j.fatal()) <<  //
            "Invariant failed: vault transaction must not change loss "
            "unrealized";
        result = false;
    }

    auto const beforeShares = [&]() -> std::optional<Shares> {
        if (beforeVault_.empty())
            return std::nullopt;
        auto const& beforeVault = beforeVault_[0];

        for (auto const& e : beforeMPTs_)
        {
            if (e.share.getMptID() == beforeVault.shareMPTID)
                return std::move(e);
        }
        return std::nullopt;
    }();

    if (!beforeShares &&
        (tx.getTxnType() == ttVAULT_DEPOSIT ||   //
         tx.getTxnType() == ttVAULT_WITHDRAW ||  //
         tx.getTxnType() == ttVAULT_CLAWBACK))
    {
        JLOG(j.fatal()) <<  //
            "Invariant failed: vault operation succeeded without updating shares";
        XRPL_ASSERT(enforce, "xrpl::ValidVault::finalize : shares noop invariant");
        return !enforce;  // That's all we can do here
    }

    if (!result)
    {
        // The comment at the top of this file starting with "assert(enforce)"
        // explains this assert.
        XRPL_ASSERT(enforce, "xrpl::ValidVault::finalize : vault invariants");
        return !enforce;
    }

    return true;
}

}  // namespace xrpl

#pragma once

#include <xrpl/basics/Number.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/XRPAmount.h>

#include <optional>
#include <unordered_map>
#include <vector>

namespace xrpl {

class VaultInvariantData
{
public:
    struct Vault final
    {
        uint256 key = beast::zero;
        Asset asset;
        AccountID pseudoId;
        AccountID owner;
        uint192 shareMPTID = beast::zero;
        Number assetsTotal = 0;
        Number assetsAvailable = 0;
        Number assetsMaximum = 0;
        Number lossUnrealized = 0;

        Vault static make(SLE const&);
    };

    struct Shares final
    {
        MPTIssue share;
        std::uint64_t sharesTotal = 0;
        std::uint64_t sharesMaximum = 0;

        Shares static make(SLE const&);
    };

    void
    visitEntry(bool isDelete, SLE::const_ref before, SLE::const_ref after);

    [[nodiscard]] std::optional<Number>
    deltaAssets(Asset const& vaultAsset, AccountID const& id) const;

    [[nodiscard]] std::optional<Number>
    deltaAssetsTxAccount(
        AccountID const& account,
        std::optional<AccountID> const& delegate,
        Asset const& vaultAsset,
        XRPAmount fee) const;

    [[nodiscard]] std::optional<Number>
    deltaShares(AccountID const& pseudoId, uint192 const& shareMPTID, AccountID const& id) const;

    [[nodiscard]] std::optional<Shares>
    resolveUpdatedShares(Vault const& afterVault) const;

    [[nodiscard]] std::optional<Shares>
    resolveBeforeShares(Vault const& beforeVault) const;

    [[nodiscard]] static bool
    vaultHoldsNoAssets(Vault const& vault);

    [[nodiscard]] std::vector<Vault> const&
    afterVault() const
    {
        return afterVault_;
    }

    [[nodiscard]] std::vector<Vault> const&
    beforeVault() const
    {
        return beforeVault_;
    }

private:
    std::vector<Vault> afterVault_;
    std::vector<Shares> afterMPTs_;
    std::vector<Vault> beforeVault_;
    std::vector<Shares> beforeMPTs_;
    std::unordered_map<uint256, Number> deltas_;
};

}  // namespace xrpl

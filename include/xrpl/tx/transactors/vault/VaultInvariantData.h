#pragma once

#include <xrpl/basics/Number.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/MPTIssue.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

#include <optional>
#include <unordered_map>
#include <vector>

namespace xrpl {

class VaultInvariantData
{
    Number static constexpr zero{};

public:
    struct Vault final
    {
        uint256 key = beast::zero;
        Asset asset = {};
        AccountID pseudoId = {};
        AccountID owner = {};
        uint192 shareMPTID = beast::zero;
        Number assetsTotal = 0;
        Number assetsAvailable = 0;
        Number assetsMaximum = 0;
        Number lossUnrealized = 0;

        Vault static make(SLE const&);
    };

    struct Shares final
    {
        MPTIssue share = {};
        std::uint64_t sharesTotal = 0;
        std::uint64_t sharesMaximum = 0;

        Shares static make(SLE const&);
    };

    void
    visitEntry(
        bool isDelete,
        std::shared_ptr<SLE const> const& before,
        std::shared_ptr<SLE const> const& after);

    void
    clear();

    [[nodiscard]] std::optional<Number>
    deltaAssets(Asset const& vaultAsset, AccountID const& id) const;

    [[nodiscard]] std::optional<Number>
    deltaAssetsTxAccount(STTx const& tx, Asset const& vaultAsset, XRPAmount fee) const;

    [[nodiscard]] std::optional<Number>
    deltaShares(AccountID const& pseudoId, uint192 const& shareMPTID, AccountID const& id) const;

    [[nodiscard]] std::optional<Shares>
    resolveUpdatedShares(Vault const& afterVault, ReadView const& view) const;

    [[nodiscard]] std::optional<Shares>
    resolveBeforeShares(Vault const& beforeVault) const;

    [[nodiscard]] static bool
    vaultHoldsNoAssets(Vault const& vault);

    std::vector<Vault> const&
    afterVault() const
    {
        return afterVault_;
    }

    std::vector<Vault> const&
    beforeVault() const
    {
        return beforeVault_;
    }

    std::vector<Shares> const&
    afterMPTs() const
    {
        return afterMPTs_;
    }

    std::vector<Shares> const&
    beforeMPTs() const
    {
        return beforeMPTs_;
    }

private:
    std::vector<Vault> afterVault_;
    std::vector<Shares> afterMPTs_;
    std::vector<Vault> beforeVault_;
    std::vector<Shares> beforeMPTs_;
    std::unordered_map<uint256, Number> deltas_;
};

}  // namespace xrpl

/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/peer/address_repository.hpp>

#include <gmock/gmock.h>

namespace libp2p::peer {

  struct AddressRepositoryMock : public AddressRepository {
    ~AddressRepositoryMock() override = default;

    MOCK_METHOD1(bootstrap, void(std::function<BootstrapCallback>));

    MOCK_METHOD2(bootstrap,
                 void(const multi::Multiaddress &,
                      std::function<BootstrapCallback>));

    // address repository
    MOCK_METHOD3(addAddresses,
                 outcome::result<bool>(const PeerId &,
                                       std::span<const multi::Multiaddress>,
                                       Milliseconds));
    MOCK_METHOD3(upsertAddresses,
                 outcome::result<bool>(const PeerId &,
                                       std::span<const multi::Multiaddress>,
                                       Milliseconds));

    MOCK_METHOD2(updateAddresses,
                 outcome::result<void>(const PeerId &, Milliseconds));

    MOCK_METHOD(void,
                dialFailed,
                (const PeerId &, const Multiaddress &),
                (override));

    MOCK_CONST_METHOD1(
        getAddresses,
        outcome::result<std::vector<multi::Multiaddress>>(const PeerId &));

    MOCK_METHOD1(clear, void(const PeerId &p));

    MOCK_CONST_METHOD0(getPeers, std::unordered_set<PeerId>());

    // garbage collectable
    MOCK_METHOD0(collectGarbage, void());
  };

}  // namespace libp2p::peer

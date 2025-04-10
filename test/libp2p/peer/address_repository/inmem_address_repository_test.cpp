/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <libp2p/common/literals.hpp>
#include <libp2p/peer/address_repository.hpp>
#include <libp2p/peer/address_repository/inmem_address_repository.hpp>
#include <libp2p/peer/errors.hpp>
#include <qtils/test/outcome.hpp>
#include <thread>
#include "mock/libp2p/network/dnsaddr_resolver_mock.hpp"

using namespace libp2p::peer;
using namespace libp2p::multi;
using namespace libp2p::common;

using std::literals::chrono_literals::operator""ms;

struct InmemAddressRepository_Test : public ::testing::Test {
  void SetUp() override {
    auto dnsaddr_resolver_mock =
        std::make_shared<libp2p::network::DnsaddrResolverMock>();
    db = std::make_unique<InmemAddressRepository>(dnsaddr_resolver_mock);
    db->onAddressAdded([](const PeerId &p, const Multiaddress &ma) {
      std::cout << "added  : <" << p.toMultihash().toHex() << "> "
                << ma.getStringAddress() << '\n';
    });

    db->onAddressRemoved([](const PeerId &p, const Multiaddress &ma) {
      std::cout << "removed: <" << p.toMultihash().toHex() << "> "
                << ma.getStringAddress() << '\n';
    });
  }

  void collectGarbage() {
    std::cout << "[collectGarbage started...  ]\n";
    db->collectGarbage();
    std::cout << "[collectGarbage finished... ]\n";
  }

  std::unique_ptr<AddressRepository> db;

  const PeerId p1 = PeerId::fromHash("12051203020304"_multihash).value();
  const PeerId p2 = PeerId::fromHash("12051203FFFFFF"_multihash).value();

  const Multiaddress ma1 = "/ip4/127.0.0.1/tcp/8080"_multiaddr;
  const Multiaddress ma2 = "/ip4/127.0.0.1/tcp/8081"_multiaddr;
  const Multiaddress ma3 = "/ip4/127.0.0.1/tcp/8082"_multiaddr;
  const Multiaddress ma4 = "/ip4/127.0.0.1/tcp/8083"_multiaddr;

  template <typename... T>
  std::vector<T...> vec(T &&...arg) {
    return std::vector<T...>{arg...};
  }
};

TEST_F(InmemAddressRepository_Test, GarbageCollection) {
  // @given address repository that has 2 peers, and some addresses
  ASSERT_OUTCOME_SUCCESS(db->addAddresses(p1, std::vector<Multiaddress>{ma1, ma2}, 10ms));
  ASSERT_OUTCOME_SUCCESS(db->addAddresses(p1, std::vector<Multiaddress>{ma3, ma4}, 1000ms));
  ASSERT_OUTCOME_SUCCESS(db->upsertAddresses(p2, std::vector<Multiaddress>{ma4}, 10ms));

  // @when no collectGarbage is called
  {
    ASSERT_OUTCOME_SUCCESS(v1, db->getAddresses(p1));
    ASSERT_OUTCOME_SUCCESS(v2, db->getAddresses(p2));

    // @then initial state is initial
    EXPECT_EQ(v1.size(), 4);
    EXPECT_EQ(v2.size(), 1);
  }

  // @when first collect garbage is called
  collectGarbage();

  {
    ASSERT_OUTCOME_SUCCESS(v1, db->getAddresses(p1));
    ASSERT_OUTCOME_SUCCESS(v2, db->getAddresses(p2));

    // @then no addresses are evicted
    EXPECT_EQ(v1.size(), 4);
    EXPECT_EQ(v2.size(), 1);
  }

  // @when second collect garbage is called in 50
  std::this_thread::sleep_for(50ms);
  collectGarbage();
  // ma1 and ma2 for p1 should be evicted by now

  {
    // @then p1 has evicted 2 addresses
    ASSERT_OUTCOME_SUCCESS(v1, db->getAddresses(p1));
    EXPECT_EQ(v1.size(), 2);

    // @and p2 has been evicted completely
    auto v2 = db->getAddresses(p2);
    // peers without addresses are removed... so we can't find this peer
    ASSERT_OUTCOME_ERROR(v2, PeerError::NOT_FOUND);
  }

  // @when clear p1 addresses
  db->clear(p1);

  {
    // @then p1 is not evicted, but all its addresses are
    // since we intentionally cleared addresses of this peer, we do not evict
    // this peer from the list of known peers up to the next garbage collection
    ASSERT_OUTCOME_SUCCESS(v1, db->getAddresses(p1));
    EXPECT_EQ(v1.size(), 0);

    // @and p2 is still evicted
    auto v2 = db->getAddresses(p2);
    ASSERT_OUTCOME_ERROR(v2, PeerError::NOT_FOUND);
  }

  // @when third collect garbage is called
  collectGarbage();

  {
    // @then both p1 and p2 have been evicted completely
    // last garbage collection removed all peers that do not have addresses
    for (const auto &it : {p1, p2}) {
      auto v = db->getAddresses(it);
      ASSERT_OUTCOME_ERROR(v, PeerError::NOT_FOUND);
    }
  }
}

/**
 * @given Peer p1 has address m1 with ttl 100ms
 * @when update ttl with 1000ms, then execute collectGarbage
 * @then ttl is updated, ma1 is not evicted
 */
TEST_F(InmemAddressRepository_Test, UpdateAddress) {
  ASSERT_OUTCOME_SUCCESS(db->addAddresses(p1, std::vector<Multiaddress>{ma1}, 10ms));
  ASSERT_OUTCOME_SUCCESS(db->upsertAddresses(p1, std::vector<Multiaddress>{ma1}, 1000ms));

  {
    ASSERT_OUTCOME_SUCCESS(v, db->getAddresses(p1));
    EXPECT_EQ(v.size(), 1);
  }

  std::this_thread::sleep_for(50ms);
  collectGarbage();

  // ma1 is updated
  ASSERT_OUTCOME_SUCCESS(v, db->getAddresses(p1));
  EXPECT_EQ(v.size(), 1);
}

/**
 * @given Peer p1 has address m1 with ttl 100ms
 * @when upsert ma2 with ttl=1000ms, and execute collectGarbage
 * @then ttl of ma1 is not updated, ma1 is evicted. ma2 is inserted.
 */
TEST_F(InmemAddressRepository_Test, InsertAddress) {
  ASSERT_OUTCOME_SUCCESS(db->addAddresses(p1, std::vector<Multiaddress>{ma1}, 10ms));
  ASSERT_OUTCOME_SUCCESS(db->upsertAddresses(p1, std::vector<Multiaddress>{ma2}, 1000ms));

  {
    ASSERT_OUTCOME_SUCCESS(v, db->getAddresses(p1));
    EXPECT_EQ(v.size(), 2);
  }

  std::this_thread::sleep_for(50ms);
  collectGarbage();

  // ma1 is evicted, ma2 is not
  ASSERT_OUTCOME_SUCCESS(v, db->getAddresses(p1));
  EXPECT_EQ(v.size(), 1);
  EXPECT_EQ(v.front(), ma2);
}

/**
 * @given 2 peers in storage
 * @when get peers
 * @then 2 peers returned
 */
TEST_F(InmemAddressRepository_Test, GetPeers) {
  ASSERT_OUTCOME_SUCCESS(db->upsertAddresses(p1, {}, 1000ms));
  ASSERT_OUTCOME_SUCCESS(db->upsertAddresses(p2, {}, 1000ms));

  auto s = db->getPeers();
  EXPECT_EQ(s.size(), 2);
}

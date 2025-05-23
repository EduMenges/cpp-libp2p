/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/protocol/kademlia/impl/kademlia_impl.hpp>

#include <unordered_set>

#include <libp2p/common/types.hpp>
#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/content_identifier.hpp>
#include <libp2p/multi/content_identifier_codec.hpp>
#include <libp2p/network/connection_manager.hpp>
#include <libp2p/peer/address_repository.hpp>
#include <libp2p/protocol/kademlia/common.hpp>
#include <libp2p/protocol/kademlia/error.hpp>
#include <libp2p/protocol/kademlia/impl/add_provider_executor.hpp>
#include <libp2p/protocol/kademlia/impl/content_routing_table.hpp>
#include <libp2p/protocol/kademlia/impl/find_peer_executor.hpp>
#include <libp2p/protocol/kademlia/impl/find_providers_executor.hpp>
#include <libp2p/protocol/kademlia/impl/get_value_executor.hpp>
#include <libp2p/protocol/kademlia/impl/put_value_executor.hpp>
#include <libp2p/protocol/kademlia/message.hpp>

namespace libp2p::protocol::kademlia {

  KademliaImpl::KademliaImpl(
      const Config &config,
      std::shared_ptr<Host> host,
      std::shared_ptr<Storage> storage,
      std::shared_ptr<ContentRoutingTable> content_routing_table,
      std::shared_ptr<PeerRoutingTable> peer_routing_table,
      std::shared_ptr<Validator> validator,
      std::shared_ptr<basic::Scheduler> scheduler,
      std::shared_ptr<event::Bus> bus,
      std::shared_ptr<crypto::random::RandomGenerator> random_generator)
      : config_(config),
        host_(std::move(host)),
        storage_(std::move(storage)),
        content_routing_table_(std::move(content_routing_table)),
        peer_routing_table_(std::move(peer_routing_table)),
        validator_(std::move(validator)),
        scheduler_(std::move(scheduler)),
        bus_(std::move(bus)),
        random_generator_(std::move(random_generator)),
        self_id_(host_->getId()),
        log_("Kademlia", "kademlia") {
    BOOST_ASSERT(host_ != nullptr);
    BOOST_ASSERT(storage_ != nullptr);
    BOOST_ASSERT(content_routing_table_ != nullptr);
    BOOST_ASSERT(peer_routing_table_ != nullptr);
    BOOST_ASSERT(validator_ != nullptr);
    BOOST_ASSERT(scheduler_ != nullptr);
    BOOST_ASSERT(bus_ != nullptr);
    BOOST_ASSERT(random_generator_ != nullptr);
  }

  void KademliaImpl::start() {
    if (started_) {
      return;
    }
    started_ = true;

    content_routing_table_->start();

    // save himself into peer repo
    addPeer(host_->getPeerInfo(), true);

    // handle streams for observed protocol
    host_->setProtocolHandler(
        config_.protocols, [wp = weak_from_this()](StreamAndProtocol stream) {
          if (auto self = wp.lock()) {
            self->handleProtocol(std::move(stream));
          }
        });

    // subscribe to new connection
    new_connection_subscription_ =
        host_->getBus()
            .getChannel<event::network::OnNewConnectionChannel>()
            .subscribe([this](
                           // NOLINTNEXTLINE
                           std::weak_ptr<connection::CapableConnection> conn) {
              if (auto connection = conn.lock()) {
                // adding outbound connections only
                if (connection->isInitiator()) {
                  log_.debug("new outbound connection");
                  auto remote_peer_res = connection->remotePeer();
                  if (!remote_peer_res) {
                    return;
                  }
                  auto remote_peer_addr_res = connection->remoteMultiaddr();
                  if (!remote_peer_addr_res) {
                    return;
                  }
                  addPeer(
                      peer::PeerInfo{std::move(remote_peer_res.value()),
                                     {std::move(remote_peer_addr_res.value())}},
                      false,
                      true);
                }
              }
            });
    on_disconnected_ =
        host_->getBus()
            .getChannel<event::network::OnPeerDisconnectedChannel>()
            .subscribe([weak_self{weak_from_this()}](const PeerId &peer) {
              auto self = weak_self.lock();
              if (not self) {
                return;
              }
              std::ignore =
                  self->peer_routing_table_->update(peer, false, false);
            });

    // start random walking
    if (config_.randomWalk.enabled) {
      randomWalk();
    }
  }

  outcome::result<void> KademliaImpl::bootstrap() {
    return findRandomPeer();
  }

  outcome::result<void> KademliaImpl::putValue(Key key, Value value) {
    log_.debug("CALL: PutValue ({})", multi::detail::encodeBase58(key));

    OUTCOME_TRY(storage_->putValue(key, value));

    // `FindPeerExecutor` holds itself using `shared_from_this`
    return createFindPeerExecutor(
               key,
               [weak_self{weak_from_this()}, key, value](
                   const outcome::result<PeerInfo> &,
                   std::vector<PeerId> succeeded_peers) {
                 auto self = weak_self.lock();
                 if (not self) {
                   return;
                 }
                 std::ignore = self->createPutValueExecutor(
                                       key, value, std::move(succeeded_peers))
                                   ->start();
               })
        ->start();
  }

  outcome::result<void> KademliaImpl::getValue(const Key &key,
                                               FoundValueHandler handler) {
    log_.debug("CALL: GetValue ({})", multi::detail::encodeBase58(key));

    // Check if has actual value locally
    if (auto res = storage_->getValue(key); res.has_value()) {
      auto &[value, ts] = res.value();
      if (scheduler_->now() < ts) {
        if (handler) {
          scheduler_->schedule(
              [handler{std::move(handler)}, value{std::move(value)}]() mutable {
                handler(std::move(value));
              });
          return outcome::success();
        }
      }
    }

    auto get_value_executor = createGetValueExecutor(key, handler);

    return get_value_executor->start();
  }

  outcome::result<void> KademliaImpl::provide(const Key &key,
                                              bool need_notify) {
    log_.debug("CALL: Provide ({})", multi::detail::encodeBase58(key));

    content_routing_table_->addProvider(key, self_id_);

    if (not need_notify) {
      return outcome::success();
    }

    auto add_provider_executor = createAddProviderExecutor(key);

    return add_provider_executor->start();
  }

  outcome::result<void> KademliaImpl::findProviders(
      const Key &key, size_t limit, FoundProvidersHandler handler) {
    log_.debug("CALL: FindProviders ({})", multi::detail::encodeBase58(key));

    // Try to find locally
    auto providers = content_routing_table_->getProvidersFor(key, limit);
    if (not providers.empty()) {
      if (limit > 0 && providers.size() > limit) {
        std::vector<PeerInfo> result;
        result.reserve(limit);
        for (auto &provider : providers) {
          // Get peer info
          auto peer_info = host_->getPeerRepository().getPeerInfo(provider);
          if (peer_info.addresses.empty()) {
            continue;
          }

          // Check if connectable
          auto connectedness = host_->connectedness(peer_info);
          if (connectedness == Message::Connectedness::CAN_NOT_CONNECT) {
            continue;
          }
          result.emplace_back(std::move(peer_info));
          if (result.size() >= limit) {
            break;
          }
        }
        if (result.size() >= limit) {
          scheduler_->schedule(
              [handler = std::move(handler), result = std::move(result)] {
                handler(result);
              });

          log_.info("Found {} providers locally from host!", result.size());
          return outcome::success();
        }
      }
    }

    auto find_providers_executor =
        createGetProvidersExecutor(key, std::move(handler));

    return find_providers_executor->start();
  }

  void KademliaImpl::addPeer(const PeerInfo &peer_info,
                             bool permanent,
                             bool is_connected) {
    log_.debug("CALL: AddPeer ({})", peer_info.id.toBase58());
    for (auto &addr : peer_info.addresses) {
      log_.debug("         addr: {}", addr.getStringAddress());
    }

    if (peer_info.addresses.empty()) {
      log_.debug("{} was skipped because has not adresses",
                 peer_info.id.toBase58());
      return;
    }

    auto upsert_res =
        host_->getPeerRepository().getAddressRepository().upsertAddresses(
            peer_info.id,
            std::span(peer_info.addresses.data(), peer_info.addresses.size()),
            permanent ? peer::ttl::kPermanent : peer::ttl::kDay);
    if (not upsert_res) {
      log_.debug("{} was skipped at addind to peer routing table: {}",
                 peer_info.id.toBase58(),
                 upsert_res.error());
      return;
    }

    auto update_res =
        peer_routing_table_->update(peer_info.id, permanent, is_connected);
    if (not update_res) {
      log_.debug("{} was not added to peer routing table: {}",
                 peer_info.id.toBase58(),
                 update_res.error());
      return;
    }
    if (update_res.value()) {
      log_.debug("{} was added to peer routing table; total {} peers",
                 peer_info.id.toBase58(),
                 peer_routing_table_->size());
    } else {
      log_.trace("{} was updated to peer routing table",
                 peer_info.id.toBase58());
    }
  }

  outcome::result<void> KademliaImpl::findPeer(const peer::PeerId &peer_id,
                                               FoundPeerInfoHandler handler) {
    BOOST_ASSERT(handler);
    log_.debug("CALL: FindPeer ({})", peer_id.toBase58());

    // Try to find locally
    auto peer_info = host_->getPeerRepository().getPeerInfo(peer_id);
    if (not peer_info.addresses.empty()) {
      scheduler_->schedule(
          [handler = std::move(handler), peer_info = std::move(peer_info)] {
            handler(peer_info, {});
          });

      log_.debug("{} found locally", peer_id.toBase58());
      return outcome::success();
    }

    auto find_peer_executor =
        createFindPeerExecutor(peer_id, std::move(handler));

    return find_peer_executor->start();
  }

  void KademliaImpl::onMessage(const std::shared_ptr<Session> &session,
                               Message &&msg) {
    switch (msg.type) {
      case Message::Type::kPutValue:
        onPutValue(session, std::move(msg));
        break;
      case Message::Type::kGetValue:
        onGetValue(session, std::move(msg));
        break;
      case Message::Type::kAddProvider:
        onAddProvider(session, std::move(msg));
        break;
      case Message::Type::kGetProviders:
        onGetProviders(session, std::move(msg));
        break;
      case Message::Type::kFindNode:
        onFindNode(session, std::move(msg));
        break;
      case Message::Type::kPing:
        onPing(session, std::move(msg));
        break;
      default:
        break;
    }
  }

  void KademliaImpl::onPutValue(const std::shared_ptr<Session> &session,
                                Message &&msg) {
    if (not msg.record) {
      log_.warn("incoming PutValue failed: no record in message");
      return;
    }
    auto &[key, value, ts] = msg.record.value();

    log_.debug("MSG: PutValue ({})", multi::detail::encodeBase58(key));

    auto validation_res = validator_->validate(key, value);
    if (not validation_res) {
      log_.warn("incoming PutValue failed: {}", validation_res.error());
      return;
    }

    auto res = storage_->putValue(key, value);
    if (!res) {
      log_.warn("incoming PutValue failed: {}", res.error());
      return;
    }

    // echo request
    session->write(msg, weak_from_this());
  }

  void KademliaImpl::onGetValue(const std::shared_ptr<Session> &session,
                                Message &&msg) {
    if (msg.key.empty()) {
      log_.warn("incoming GetValue failed: empty key in message");
      return;
    }

    log_.debug("MSG: GetValue ({})", multi::detail::encodeBase58(msg.key));

    if (auto providers = content_routing_table_->getProvidersFor(msg.key);
        not providers.empty()) {
      std::vector<Message::Peer> peers;
      peers.reserve(config_.closerPeerCount);

      for (const auto &peer : providers) {
        auto info = host_->getPeerRepository().getPeerInfo(peer);
        if (info.addresses.empty()) {
          continue;
        }
        auto connectedness = host_->connectedness(info);
        peers.push_back({std::move(info), connectedness});
        if (peers.size() >= config_.closerPeerCount) {
          break;
        }
      }

      msg.provider_peers = std::move(peers);
    }

    auto res = storage_->getValue(msg.key);
    if (res) {
      auto &[value, expire] = res.value();
      msg.record = Message::Record{
          std::move(msg.key), std::move(value), std::to_string(expire.count())};
    }

    session->write(msg, weak_from_this());
  }

  void KademliaImpl::onAddProvider(const std::shared_ptr<Session> &session,
                                   Message &&msg) {
    if (not msg.provider_peers) {
      log_.warn("AddProvider failed: no provider_peers im message");
      return;
    }

    log_.debug("MSG: AddProvider ({})", multi::detail::encodeBase58(msg.key));

    auto &providers = msg.provider_peers.value();
    for (auto &provider : providers) {
      if (auto peer_id_res = session->stream()->remotePeerId()) {
        if (peer_id_res.value() == provider.info.id) {
          // Save providers who have provided themselves
          content_routing_table_->addProvider(msg.key, provider.info.id);
          addPeer(provider.info, false);
        }
      }
    }
  }

  void KademliaImpl::onGetProviders(const std::shared_ptr<Session> &session,
                                    Message &&msg) {
    if (msg.key.empty()) {
      log_.warn("GetProviders failed: empty key in message");
      return;
    }

    log_.debug("MSG: GetProviders ({})", multi::detail::encodeBase58(msg.key));

    auto peer_ids = content_routing_table_->getProvidersFor(
        msg.key, config_.closerPeerCount * 2);

    if (not peer_ids.empty()) {
      std::vector<Message::Peer> peers;
      peers.reserve(config_.closerPeerCount);

      for (const auto &p : peer_ids) {
        auto info = host_->getPeerRepository().getPeerInfo(p);
        if (info.addresses.empty()) {
          continue;
        }
        auto connectedness = host_->connectedness(info);
        peers.push_back({std::move(info), connectedness});
        if (peers.size() >= config_.closerPeerCount) {
          break;
        }
      }

      if (not peers.empty()) {
        msg.provider_peers = std::move(peers);
      }
    }

    peer_ids = peer_routing_table_->getNearestPeers(
        NodeId::hash(msg.key), config_.closerPeerCount * 2);

    if (not peer_ids.empty()) {
      std::vector<Message::Peer> peers;
      peers.reserve(config_.closerPeerCount);

      for (const auto &p : peer_ids) {
        auto info = host_->getPeerRepository().getPeerInfo(p);
        if (info.addresses.empty()) {
          continue;
        }
        auto connectedness = host_->connectedness(info);
        peers.push_back({std::move(info), connectedness});
        if (peers.size() >= config_.closerPeerCount) {
          break;
        }
      }

      if (not peers.empty()) {
        msg.closer_peers = std::move(peers);
      }
    }

    session->write(msg, weak_from_this());
  }

  void KademliaImpl::onFindNode(const std::shared_ptr<Session> &session,
                                Message &&msg) {
    if (msg.key.empty()) {
      log_.warn("FindNode failed: empty key in message");
      return;
    }

    if (msg.closer_peers) {
      for (auto &peer : msg.closer_peers.value()) {
        if (peer.conn_status != Message::Connectedness::CAN_NOT_CONNECT) {
          // Add/Update peer info
          [[maybe_unused]] auto res =
              host_->getPeerRepository().getAddressRepository().upsertAddresses(
                  peer.info.id,
                  std::span(peer.info.addresses.data(),
                            peer.info.addresses.size()),
                  peer::ttl::kDay);
        }
      }
      msg.closer_peers.reset();
    }

    log_.debug("MSG: FindNode ({})", multi::detail::encodeBase58(msg.key));

    auto ids = peer_routing_table_->getNearestPeers(
        NodeId::hash(msg.key), config_.closerPeerCount * 2);

    std::vector<Message::Peer> peers;
    peers.reserve(config_.closerPeerCount);

    for (const auto &p : ids) {
      auto info = host_->getPeerRepository().getPeerInfo(p);
      if (info.addresses.empty()) {
        continue;
      }
      auto connectedness = host_->connectedness(info);
      peers.push_back({std::move(info), connectedness});
      if (peers.size() >= config_.closerPeerCount) {
        break;
      }
    }

    if (not peers.empty()) {
      msg.closer_peers = std::move(peers);
    }

    session->write(msg, weak_from_this());
  }

  void KademliaImpl::onPing(const std::shared_ptr<Session> &session,
                            Message &&msg) {
    msg.clear();

    session->write(msg, weak_from_this());
  }

  outcome::result<void> KademliaImpl::findRandomPeer() {
    common::Hash256 hash;
    random_generator_->fillRandomly(hash);

    auto peer_id =
        peer::PeerId::fromHash(
            multi::Multihash::create(multi::HashType::sha256, hash).value())
            .value();

    FoundPeerInfoHandler handler = [wp = weak_from_this()](
                                       outcome::result<PeerInfo> res,
                                       const std::vector<PeerId> &) {
      if (auto self = wp.lock()) {
        if (res.has_value()) {
          self->addPeer(res.value(), false);
        }
      }
    };

    return findPeer(peer_id, handler);
  }

  void KademliaImpl::randomWalk() {
    BOOST_ASSERT(config_.randomWalk.enabled);

    // Doing walk
    [[maybe_unused]] auto result = findRandomPeer();

    auto iteration = random_walking_.iteration++;

    // if period end
    Time delay = config_.randomWalk.delay;
    if ((iteration % config_.randomWalk.queries_per_period) == 0) {
      delay = config_.randomWalk.interval
            - config_.randomWalk.delay * config_.randomWalk.queries_per_period;
    }

    // Schedule next walking
    random_walking_.handle =
        scheduler_->scheduleWithHandle([this] { randomWalk(); }, delay);
  }

  std::shared_ptr<Session> KademliaImpl::openSession(
      std::shared_ptr<connection::Stream> stream) {
    return std::make_shared<Session>(
        scheduler_, stream, config_.responseTimeout);
  }

  void KademliaImpl::handleProtocol(StreamAndProtocol stream_and_protocol) {
    auto &stream = stream_and_protocol.stream;

    if (stream->remotePeerId().value() == self_id_) {
      log_.debug("incoming stream with themselves");
      stream->reset();
      return;
    }

    log_.debug("incoming stream with {}",
               stream->remotePeerId().value().toBase58());

    auto session = openSession(stream);
    session->read(weak_from_this());
  }

  std::shared_ptr<PutValueExecutor> KademliaImpl::createPutValueExecutor(
      ContentId key, ContentValue value, std::vector<PeerId> addressees) {
    return std::make_shared<PutValueExecutor>(config_,
                                              host_,
                                              scheduler_,
                                              shared_from_this(),
                                              std::move(key),
                                              std::move(value),
                                              std::move(addressees));
  }

  std::shared_ptr<GetValueExecutor> KademliaImpl::createGetValueExecutor(
      ContentId key, FoundValueHandler handler) {
    return std::make_shared<GetValueExecutor>(config_,
                                              host_,
                                              scheduler_,
                                              shared_from_this(),
                                              content_routing_table_,
                                              peer_routing_table_,
                                              shared_from_this(),
                                              validator_,
                                              std::move(key),
                                              std::move(handler));
  }

  std::shared_ptr<AddProviderExecutor> KademliaImpl::createAddProviderExecutor(
      ContentId content_id) {
    return std::make_shared<AddProviderExecutor>(config_,
                                                 host_,
                                                 scheduler_,
                                                 shared_from_this(),
                                                 peer_routing_table_,
                                                 std::move(content_id));
  }

  std::shared_ptr<FindProvidersExecutor>
  KademliaImpl::createGetProvidersExecutor(ContentId content_id,
                                           FoundProvidersHandler handler) {
    return std::make_shared<FindProvidersExecutor>(config_,
                                                   host_,
                                                   scheduler_,
                                                   shared_from_this(),
                                                   peer_routing_table_,
                                                   std::move(content_id),
                                                   std::move(handler));
  }

  std::shared_ptr<FindPeerExecutor> KademliaImpl::createFindPeerExecutor(
      HashedKey key, FoundPeerInfoHandler handler) {
    return std::make_shared<FindPeerExecutor>(config_,
                                              host_,
                                              scheduler_,
                                              shared_from_this(),
                                              peer_routing_table_,
                                              std::move(key),
                                              std::move(handler));
  }

}  // namespace libp2p::protocol::kademlia

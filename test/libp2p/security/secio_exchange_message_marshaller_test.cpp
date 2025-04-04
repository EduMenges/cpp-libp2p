/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <libp2p/security/secio/exchange_message_marshaller_impl.hpp>
#include <qtils/test/outcome.hpp>

using namespace libp2p::security::secio;

class ExchangeMessageMarshallerTest : public ::testing::Test {
 protected:
  ExchangeMessageMarshallerImpl marshaller;
};

/**
 * @given a SECIO exchange message
 * @when the message is marshalled
 * @then the result of its unmarshalling equals to the source message
 */
TEST_F(ExchangeMessageMarshallerTest, BasicCase) {
  ExchangeMessage source{.epubkey = {1, 2, 3, 4, 5},
                         .signature = {6, 7, 8, 9, 10}};
  ASSERT_OUTCOME_SUCCESS(bytes, marshaller.marshal(source));
  ASSERT_OUTCOME_SUCCESS(derived, marshaller.unmarshal(bytes));
  ASSERT_EQ(source.epubkey, derived.epubkey);
  ASSERT_EQ(source.signature, derived.signature);
}

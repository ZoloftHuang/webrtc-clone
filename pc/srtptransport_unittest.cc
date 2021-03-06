/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <vector>

#include "pc/srtptransport.h"

#include "media/base/fakertp.h"
#include "p2p/base/dtlstransportinternal.h"
#include "p2p/base/fakepackettransport.h"
#include "pc/rtptransport.h"
#include "pc/rtptransporttestutil.h"
#include "pc/srtptestutil.h"
#include "rtc_base/asyncpacketsocket.h"
#include "rtc_base/gunit.h"
#include "rtc_base/ptr_util.h"
#include "rtc_base/sslstreamadapter.h"

using rtc::kTestKey1;
using rtc::kTestKey2;
using rtc::kTestKeyLen;
using rtc::SRTP_AEAD_AES_128_GCM;

namespace webrtc {
static const uint8_t kTestKeyGcm128_1[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ12";
static const uint8_t kTestKeyGcm128_2[] = "21ZYXWVUTSRQPONMLKJIHGFEDCBA";
static const int kTestKeyGcm128Len = 28;  // 128 bits key + 96 bits salt.
static const uint8_t kTestKeyGcm256_1[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqr";
static const uint8_t kTestKeyGcm256_2[] =
    "rqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA";
static const int kTestKeyGcm256Len = 44;  // 256 bits key + 96 bits salt.

class SrtpTransportTest : public testing::Test, public sigslot::has_slots<> {
 protected:
  SrtpTransportTest() {
    bool rtcp_mux_enabled = true;
    auto rtp_transport1 = rtc::MakeUnique<RtpTransport>(rtcp_mux_enabled);
    auto rtp_transport2 = rtc::MakeUnique<RtpTransport>(rtcp_mux_enabled);

    rtp_packet_transport1_ =
        rtc::MakeUnique<rtc::FakePacketTransport>("fake_packet_transport1");
    rtp_packet_transport2_ =
        rtc::MakeUnique<rtc::FakePacketTransport>("fake_packet_transport2");

    bool asymmetric = false;
    rtp_packet_transport1_->SetDestination(rtp_packet_transport2_.get(),
                                           asymmetric);

    rtp_transport1->SetRtpPacketTransport(rtp_packet_transport1_.get());
    rtp_transport2->SetRtpPacketTransport(rtp_packet_transport2_.get());

    srtp_transport1_ =
        rtc::MakeUnique<SrtpTransport>(std::move(rtp_transport1));
    srtp_transport2_ =
        rtc::MakeUnique<SrtpTransport>(std::move(rtp_transport2));

    srtp_transport1_->SignalPacketReceived.connect(
        this, &SrtpTransportTest::OnPacketReceived1);
    srtp_transport2_->SignalPacketReceived.connect(
        this, &SrtpTransportTest::OnPacketReceived2);
  }

  void OnPacketReceived1(bool rtcp,
                         rtc::CopyOnWriteBuffer* packet,
                         const rtc::PacketTime& packet_time) {
    RTC_LOG(LS_INFO) << "SrtpTransport1 Received a packet.";
    last_recv_packet1_ = *packet;
  }

  void OnPacketReceived2(bool rtcp,
                         rtc::CopyOnWriteBuffer* packet,
                         const rtc::PacketTime& packet_time) {
    RTC_LOG(LS_INFO) << "SrtpTransport2 Received a packet.";
    last_recv_packet2_ = *packet;
  }

  // With external auth enabled, SRTP doesn't write the auth tag and
  // unprotect would fail. Check accessing the information about the
  // tag instead, similar to what the actual code would do that relies
  // on external auth.
  void TestRtpAuthParams(SrtpTransport* transport, const std::string& cs) {
    int overhead;
    EXPECT_TRUE(transport->GetSrtpOverhead(&overhead));
    switch (rtc::SrtpCryptoSuiteFromName(cs)) {
      case rtc::SRTP_AES128_CM_SHA1_32:
        EXPECT_EQ(32 / 8, overhead);  // 32-bit tag.
        break;
      case rtc::SRTP_AES128_CM_SHA1_80:
        EXPECT_EQ(80 / 8, overhead);  // 80-bit tag.
        break;
      default:
        RTC_NOTREACHED();
        break;
    }

    uint8_t* auth_key = nullptr;
    int key_len = 0;
    int tag_len = 0;
    EXPECT_TRUE(transport->GetRtpAuthParams(&auth_key, &key_len, &tag_len));
    EXPECT_NE(nullptr, auth_key);
    EXPECT_EQ(160 / 8, key_len);  // Length of SHA-1 is 160 bits.
    EXPECT_EQ(overhead, tag_len);
  }

  void TestSendRecvRtpPacket(const std::string& cipher_suite_name) {
    size_t rtp_len = sizeof(kPcmuFrame);
    size_t packet_size = rtp_len + rtc::rtp_auth_tag_len(cipher_suite_name);
    rtc::Buffer rtp_packet_buffer(packet_size);
    char* rtp_packet_data = rtp_packet_buffer.data<char>();
    memcpy(rtp_packet_data, kPcmuFrame, rtp_len);
    // In order to be able to run this test function multiple times we can not
    // use the same sequence number twice. Increase the sequence number by one.
    rtc::SetBE16(reinterpret_cast<uint8_t*>(rtp_packet_data) + 2,
                 ++sequence_number_);
    rtc::CopyOnWriteBuffer rtp_packet1to2(rtp_packet_data, rtp_len,
                                          packet_size);
    rtc::CopyOnWriteBuffer rtp_packet2to1(rtp_packet_data, rtp_len,
                                          packet_size);

    char original_rtp_data[sizeof(kPcmuFrame)];
    memcpy(original_rtp_data, rtp_packet_data, rtp_len);

    rtc::PacketOptions options;
    // Send a packet from |srtp_transport1_| to |srtp_transport2_| and verify
    // that the packet can be successfully received and decrypted.
    ASSERT_TRUE(srtp_transport1_->SendRtpPacket(&rtp_packet1to2, options,
                                                cricket::PF_SRTP_BYPASS));
    if (srtp_transport1_->IsExternalAuthActive()) {
      TestRtpAuthParams(srtp_transport1_.get(), cipher_suite_name);
    } else {
      ASSERT_TRUE(last_recv_packet2_.data());
      EXPECT_EQ(0,
                memcmp(last_recv_packet2_.data(), original_rtp_data, rtp_len));
      // Get the encrypted packet from underneath packet transport and verify
      // the data is actually encrypted.
      auto fake_rtp_packet_transport = static_cast<rtc::FakePacketTransport*>(
          srtp_transport1_->rtp_packet_transport());
      EXPECT_NE(0, memcmp(fake_rtp_packet_transport->last_sent_packet()->data(),
                          original_rtp_data, rtp_len));
    }

    // Do the same thing in the opposite direction;
    ASSERT_TRUE(srtp_transport2_->SendRtpPacket(&rtp_packet2to1, options,
                                                cricket::PF_SRTP_BYPASS));
    if (srtp_transport2_->IsExternalAuthActive()) {
      TestRtpAuthParams(srtp_transport2_.get(), cipher_suite_name);
    } else {
      ASSERT_TRUE(last_recv_packet1_.data());
      EXPECT_EQ(0,
                memcmp(last_recv_packet1_.data(), original_rtp_data, rtp_len));
      auto fake_rtp_packet_transport = static_cast<rtc::FakePacketTransport*>(
          srtp_transport2_->rtp_packet_transport());
      EXPECT_NE(0, memcmp(fake_rtp_packet_transport->last_sent_packet()->data(),
                          original_rtp_data, rtp_len));
    }
  }

  void TestSendRecvRtcpPacket(const std::string& cipher_suite_name) {
    size_t rtcp_len = sizeof(kRtcpReport);
    size_t packet_size =
        rtcp_len + 4 + rtc::rtcp_auth_tag_len(cipher_suite_name);
    rtc::Buffer rtcp_packet_buffer(packet_size);
    char* rtcp_packet_data = rtcp_packet_buffer.data<char>();
    memcpy(rtcp_packet_data, kRtcpReport, rtcp_len);

    rtc::CopyOnWriteBuffer rtcp_packet1to2(rtcp_packet_data, rtcp_len,
                                           packet_size);
    rtc::CopyOnWriteBuffer rtcp_packet2to1(rtcp_packet_data, rtcp_len,
                                           packet_size);

    rtc::PacketOptions options;
    // Send a packet from |srtp_transport1_| to |srtp_transport2_| and verify
    // that the packet can be successfully received and decrypted.
    ASSERT_TRUE(srtp_transport1_->SendRtcpPacket(&rtcp_packet1to2, options,
                                                 cricket::PF_SRTP_BYPASS));
    ASSERT_TRUE(last_recv_packet2_.data());
    EXPECT_EQ(0, memcmp(last_recv_packet2_.data(), rtcp_packet_data, rtcp_len));
    // Get the encrypted packet from underneath packet transport and verify the
    // data is actually encrypted.
    auto fake_rtp_packet_transport = static_cast<rtc::FakePacketTransport*>(
        srtp_transport1_->rtp_packet_transport());
    EXPECT_NE(0, memcmp(fake_rtp_packet_transport->last_sent_packet()->data(),
                        rtcp_packet_data, rtcp_len));

    // Do the same thing in the opposite direction;
    ASSERT_TRUE(srtp_transport2_->SendRtcpPacket(&rtcp_packet2to1, options,
                                                 cricket::PF_SRTP_BYPASS));
    ASSERT_TRUE(last_recv_packet1_.data());
    EXPECT_EQ(0, memcmp(last_recv_packet1_.data(), rtcp_packet_data, rtcp_len));
    fake_rtp_packet_transport = static_cast<rtc::FakePacketTransport*>(
        srtp_transport2_->rtp_packet_transport());
    EXPECT_NE(0, memcmp(fake_rtp_packet_transport->last_sent_packet()->data(),
                        rtcp_packet_data, rtcp_len));
  }

  void TestSendRecvPacket(bool enable_external_auth,
                          int cs,
                          const uint8_t* key1,
                          int key1_len,
                          const uint8_t* key2,
                          int key2_len,
                          const std::string& cipher_suite_name) {
    EXPECT_EQ(key1_len, key2_len);
    EXPECT_EQ(cipher_suite_name, rtc::SrtpCryptoSuiteToName(cs));
    if (enable_external_auth) {
      srtp_transport1_->EnableExternalAuth();
      srtp_transport2_->EnableExternalAuth();
    }
    std::vector<int> extension_ids;
    EXPECT_TRUE(srtp_transport1_->SetRtpParams(
        cs, key1, key1_len, extension_ids, cs, key2, key2_len, extension_ids));
    EXPECT_TRUE(srtp_transport2_->SetRtpParams(
        cs, key2, key2_len, extension_ids, cs, key1, key1_len, extension_ids));
    EXPECT_TRUE(srtp_transport1_->SetRtcpParams(
        cs, key1, key1_len, extension_ids, cs, key2, key2_len, extension_ids));
    EXPECT_TRUE(srtp_transport2_->SetRtcpParams(
        cs, key2, key2_len, extension_ids, cs, key1, key1_len, extension_ids));
    EXPECT_TRUE(srtp_transport1_->IsSrtpActive());
    EXPECT_TRUE(srtp_transport2_->IsSrtpActive());
    if (rtc::IsGcmCryptoSuite(cs)) {
      EXPECT_FALSE(srtp_transport1_->IsExternalAuthActive());
      EXPECT_FALSE(srtp_transport2_->IsExternalAuthActive());
    } else if (enable_external_auth) {
      EXPECT_TRUE(srtp_transport1_->IsExternalAuthActive());
      EXPECT_TRUE(srtp_transport2_->IsExternalAuthActive());
    }
    TestSendRecvRtpPacket(cipher_suite_name);
    TestSendRecvRtcpPacket(cipher_suite_name);
  }

  void TestSendRecvPacketWithEncryptedHeaderExtension(
      const std::string& cs,
      const std::vector<int>& encrypted_header_ids) {
    size_t rtp_len = sizeof(kPcmuFrameWithExtensions);
    size_t packet_size = rtp_len + rtc::rtp_auth_tag_len(cs);
    rtc::Buffer rtp_packet_buffer(packet_size);
    char* rtp_packet_data = rtp_packet_buffer.data<char>();
    memcpy(rtp_packet_data, kPcmuFrameWithExtensions, rtp_len);
    // In order to be able to run this test function multiple times we can not
    // use the same sequence number twice. Increase the sequence number by one.
    rtc::SetBE16(reinterpret_cast<uint8_t*>(rtp_packet_data) + 2,
                 ++sequence_number_);
    rtc::CopyOnWriteBuffer rtp_packet1to2(rtp_packet_data, rtp_len,
                                          packet_size);
    rtc::CopyOnWriteBuffer rtp_packet2to1(rtp_packet_data, rtp_len,
                                          packet_size);

    char original_rtp_data[sizeof(kPcmuFrameWithExtensions)];
    memcpy(original_rtp_data, rtp_packet_data, rtp_len);

    rtc::PacketOptions options;
    // Send a packet from |srtp_transport1_| to |srtp_transport2_| and verify
    // that the packet can be successfully received and decrypted.
    ASSERT_TRUE(srtp_transport1_->SendRtpPacket(&rtp_packet1to2, options,
                                                cricket::PF_SRTP_BYPASS));
    ASSERT_TRUE(last_recv_packet2_.data());
    EXPECT_EQ(0, memcmp(last_recv_packet2_.data(), original_rtp_data, rtp_len));
    // Get the encrypted packet from underneath packet transport and verify the
    // data and header extension are actually encrypted.
    auto fake_rtp_packet_transport = static_cast<rtc::FakePacketTransport*>(
        srtp_transport1_->rtp_packet_transport());
    EXPECT_NE(0, memcmp(fake_rtp_packet_transport->last_sent_packet()->data(),
                        original_rtp_data, rtp_len));
    CompareHeaderExtensions(
        reinterpret_cast<const char*>(
            fake_rtp_packet_transport->last_sent_packet()->data()),
        fake_rtp_packet_transport->last_sent_packet()->size(),
        original_rtp_data, rtp_len, encrypted_header_ids, false);

    // Do the same thing in the opposite direction;
    ASSERT_TRUE(srtp_transport2_->SendRtpPacket(&rtp_packet2to1, options,
                                                cricket::PF_SRTP_BYPASS));
    ASSERT_TRUE(last_recv_packet1_.data());
    EXPECT_EQ(0, memcmp(last_recv_packet1_.data(), original_rtp_data, rtp_len));
    fake_rtp_packet_transport = static_cast<rtc::FakePacketTransport*>(
        srtp_transport2_->rtp_packet_transport());
    EXPECT_NE(0, memcmp(fake_rtp_packet_transport->last_sent_packet()->data(),
                        original_rtp_data, rtp_len));
    CompareHeaderExtensions(
        reinterpret_cast<const char*>(
            fake_rtp_packet_transport->last_sent_packet()->data()),
        fake_rtp_packet_transport->last_sent_packet()->size(),
        original_rtp_data, rtp_len, encrypted_header_ids, false);
  }

  void TestSendRecvEncryptedHeaderExtension(int cs,
                                            const uint8_t* key1,
                                            int key1_len,
                                            const uint8_t* key2,
                                            int key2_len,
                                            const std::string& cs_name) {
    std::vector<int> encrypted_headers;
    encrypted_headers.push_back(kHeaderExtensionIDs[0]);
    // Don't encrypt header ids 2 and 3.
    encrypted_headers.push_back(kHeaderExtensionIDs[1]);
    EXPECT_EQ(key1_len, key2_len);
    EXPECT_EQ(cs_name, rtc::SrtpCryptoSuiteToName(cs));
    EXPECT_TRUE(srtp_transport1_->SetRtpParams(cs, key1, key1_len,
                                               encrypted_headers, cs, key2,
                                               key2_len, encrypted_headers));
    EXPECT_TRUE(srtp_transport2_->SetRtpParams(cs, key2, key2_len,
                                               encrypted_headers, cs, key1,
                                               key1_len, encrypted_headers));
    EXPECT_TRUE(srtp_transport1_->IsSrtpActive());
    EXPECT_TRUE(srtp_transport2_->IsSrtpActive());
    EXPECT_FALSE(srtp_transport1_->IsExternalAuthActive());
    EXPECT_FALSE(srtp_transport2_->IsExternalAuthActive());
    TestSendRecvPacketWithEncryptedHeaderExtension(cs_name, encrypted_headers);
  }

  std::unique_ptr<SrtpTransport> srtp_transport1_;
  std::unique_ptr<SrtpTransport> srtp_transport2_;

  std::unique_ptr<rtc::FakePacketTransport> rtp_packet_transport1_;
  std::unique_ptr<rtc::FakePacketTransport> rtp_packet_transport2_;

  rtc::CopyOnWriteBuffer last_recv_packet1_;
  rtc::CopyOnWriteBuffer last_recv_packet2_;
  int sequence_number_ = 0;
};

class SrtpTransportTestWithExternalAuth
    : public SrtpTransportTest,
      public testing::WithParamInterface<bool> {};

TEST_P(SrtpTransportTestWithExternalAuth,
       SendAndRecvPacket_AES_CM_128_HMAC_SHA1_80) {
  bool enable_external_auth = GetParam();
  TestSendRecvPacket(enable_external_auth, rtc::SRTP_AES128_CM_SHA1_80,
                     kTestKey1, kTestKeyLen, kTestKey2, kTestKeyLen,
                     rtc::CS_AES_CM_128_HMAC_SHA1_80);
}

TEST_F(SrtpTransportTest,
       SendAndRecvPacketWithHeaderExtension_AES_CM_128_HMAC_SHA1_80) {
  TestSendRecvEncryptedHeaderExtension(rtc::SRTP_AES128_CM_SHA1_80, kTestKey1,
                                       kTestKeyLen, kTestKey2, kTestKeyLen,
                                       rtc::CS_AES_CM_128_HMAC_SHA1_80);
}

TEST_P(SrtpTransportTestWithExternalAuth,
       SendAndRecvPacket_AES_CM_128_HMAC_SHA1_32) {
  bool enable_external_auth = GetParam();
  TestSendRecvPacket(enable_external_auth, rtc::SRTP_AES128_CM_SHA1_32,
                     kTestKey1, kTestKeyLen, kTestKey2, kTestKeyLen,
                     rtc::CS_AES_CM_128_HMAC_SHA1_32);
}

TEST_F(SrtpTransportTest,
       SendAndRecvPacketWithHeaderExtension_AES_CM_128_HMAC_SHA1_32) {
  TestSendRecvEncryptedHeaderExtension(rtc::SRTP_AES128_CM_SHA1_32, kTestKey1,
                                       kTestKeyLen, kTestKey2, kTestKeyLen,
                                       rtc::CS_AES_CM_128_HMAC_SHA1_32);
}

TEST_P(SrtpTransportTestWithExternalAuth,
       SendAndRecvPacket_SRTP_AEAD_AES_128_GCM) {
  bool enable_external_auth = GetParam();
  TestSendRecvPacket(enable_external_auth, rtc::SRTP_AEAD_AES_128_GCM,
                     kTestKeyGcm128_1, kTestKeyGcm128Len, kTestKeyGcm128_2,
                     kTestKeyGcm128Len, rtc::CS_AEAD_AES_128_GCM);
}

TEST_F(SrtpTransportTest,
       SendAndRecvPacketWithHeaderExtension_SRTP_AEAD_AES_128_GCM) {
  TestSendRecvEncryptedHeaderExtension(
      rtc::SRTP_AEAD_AES_128_GCM, kTestKeyGcm128_1, kTestKeyGcm128Len,
      kTestKeyGcm128_2, kTestKeyGcm128Len, rtc::CS_AEAD_AES_128_GCM);
}

TEST_P(SrtpTransportTestWithExternalAuth,
       SendAndRecvPacket_SRTP_AEAD_AES_256_GCM) {
  bool enable_external_auth = GetParam();
  TestSendRecvPacket(enable_external_auth, rtc::SRTP_AEAD_AES_256_GCM,
                     kTestKeyGcm256_1, kTestKeyGcm256Len, kTestKeyGcm256_2,
                     kTestKeyGcm256Len, rtc::CS_AEAD_AES_256_GCM);
}

TEST_F(SrtpTransportTest,
       SendAndRecvPacketWithHeaderExtension_SRTP_AEAD_AES_256_GCM) {
  TestSendRecvEncryptedHeaderExtension(
      rtc::SRTP_AEAD_AES_256_GCM, kTestKeyGcm256_1, kTestKeyGcm256Len,
      kTestKeyGcm256_2, kTestKeyGcm256Len, rtc::CS_AEAD_AES_256_GCM);
}

// Run all tests both with and without external auth enabled.
INSTANTIATE_TEST_CASE_P(ExternalAuth,
                        SrtpTransportTestWithExternalAuth,
                        ::testing::Values(true, false));

// Test directly setting the params with bogus keys.
TEST_F(SrtpTransportTest, TestSetParamsKeyTooShort) {
  std::vector<int> extension_ids;
  EXPECT_FALSE(srtp_transport1_->SetRtpParams(
      rtc::SRTP_AES128_CM_SHA1_80, kTestKey1, kTestKeyLen - 1, extension_ids,
      rtc::SRTP_AES128_CM_SHA1_80, kTestKey1, kTestKeyLen - 1, extension_ids));
  EXPECT_FALSE(srtp_transport1_->SetRtcpParams(
      rtc::SRTP_AES128_CM_SHA1_80, kTestKey1, kTestKeyLen - 1, extension_ids,
      rtc::SRTP_AES128_CM_SHA1_80, kTestKey1, kTestKeyLen - 1, extension_ids));
}

}  // namespace webrtc

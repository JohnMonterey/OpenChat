#include "security/DeviceIdentity.h"

#include "protocol/CanonicalCborCodec.h"
#include "RelayCrypto.h"

#include <QCryptographicHash>
#include <QtTest/QTest>

using namespace OpenChat;

namespace {

template <typename Id>
Id idFromHex(const char *hex) {
  const auto id = Id::fromBytes(QByteArray::fromHex(hex));
  Q_ASSERT(id.has_value());
  return *id;
}

// An envelope whose canonical signing input the relay would verify. The
// signature field is left empty: encodeForSignature covers the envelope with
// that field cleared, which is exactly what the sender signs.
CiphertextEnvelopeV1 sampleEnvelope() {
  const QByteArray ciphertext("relay-interop-ciphertext");
  return CiphertextEnvelopeV1{
      1,
      idFromHex<EnvelopeId>("000102030405060708090a0b0c0d0e0f"),
      idFromHex<AccountId>("101112131415161718191a1b1c1d1e1f"),
      idFromHex<DeviceId>("202122232425262728292a2b2c2d2e2f"),
      idFromHex<DeviceId>("303132333435363738393a3b3c3d3e3f"),
      idFromHex<ConversationId>("404142434445464748494a4b4c4d4e4f"),
      EnvelopeMessageKind::MlsPrivateMessage,
      1'700'000'000'000,
      1'700'000'060'000,
      idFromHex<EnvelopeId>("505152535455565758595a5b5c5d5e5f"),
      ciphertext,
      QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256),
      {}};
}

} // namespace

class DeviceIdentityTest final : public QObject {
  Q_OBJECT

private slots:
  void signEnvelopeVerifiesWithRelayVerifier();
  void signEnvelopeRejectsTamperedInput();
  void signEnvelopeCarriesNoDomainPrefix();
  void signEnvelopeRejectsEmptyInput();
};

void DeviceIdentityTest::signEnvelopeVerifiesWithRelayVerifier() {
  auto identityResult = DeviceIdentity::generate();
  QVERIFY(identityResult.hasValue());
  const DeviceIdentity identity = std::move(identityResult).value();
  const DevicePublicCredential credential = identity.publicCredential();

  const QByteArray signingInput = encodeForSignature(sampleEnvelope());
  QVERIFY(!signingInput.isEmpty());

  auto signature = identity.signEnvelope(signingInput);
  QVERIFY(signature.hasValue());
  QCOMPARE(signature.value().size(), qsizetype(64));

  // The relay checks a RAW Ed25519 signature over exactly the canonical signing
  // input (no domain prefix). Cross-verifying with its real verifier over the
  // very bytes the signer covered proves byte-for-byte interop.
  QVERIFY(Relay::verifyEd25519(credential.signingPublicKey, signingInput,
                               signature.value()));
}

void DeviceIdentityTest::signEnvelopeRejectsTamperedInput() {
  auto identityResult = DeviceIdentity::generate();
  QVERIFY(identityResult.hasValue());
  const DeviceIdentity identity = std::move(identityResult).value();
  const DevicePublicCredential credential = identity.publicCredential();

  const QByteArray signingInput = encodeForSignature(sampleEnvelope());
  auto signature = identity.signEnvelope(signingInput);
  QVERIFY(signature.hasValue());

  QByteArray tampered = signingInput;
  tampered[0] = static_cast<char>(tampered.at(0) ^ 0x01);
  QVERIFY(!Relay::verifyEd25519(credential.signingPublicKey, tampered,
                                signature.value()));
}

void DeviceIdentityTest::signEnvelopeCarriesNoDomainPrefix() {
  auto identityResult = DeviceIdentity::generate();
  QVERIFY(identityResult.hasValue());
  const DeviceIdentity identity = std::move(identityResult).value();

  const QByteArray input("shared-bytes-for-both-signers");
  auto envelopeSignature = identity.signEnvelope(input);
  QVERIFY(envelopeSignature.hasValue());
  // signChallenge domain-separates and length-frames the same bytes before
  // signing, so its output must differ from the raw envelope signature.
  auto challengeSignature = identity.signChallenge(input, "openchat-auth-v1");
  QVERIFY(challengeSignature.hasValue());
  QVERIFY(envelopeSignature.value() != challengeSignature.value());
}

void DeviceIdentityTest::signEnvelopeRejectsEmptyInput() {
  auto identityResult = DeviceIdentity::generate();
  QVERIFY(identityResult.hasValue());
  const DeviceIdentity identity = std::move(identityResult).value();
  auto signature = identity.signEnvelope(QByteArray{});
  QVERIFY(!signature.hasValue());
  QCOMPARE(signature.error(), DeviceIdentityError::InvalidInput);
}

QTEST_GUILESS_MAIN(DeviceIdentityTest)
#include "tst_deviceidentity.moc"

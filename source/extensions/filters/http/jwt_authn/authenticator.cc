#include "source/extensions/filters/http/jwt_authn/authenticator.h"

#include "envoy/http/async_client.h"

#include "source/common/common/assert.h"
#include "source/common/common/base64.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/logger.h"
#include "source/common/http/message_impl.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/tracing/http_tracer_impl.h"

#include "jwt_verify_lib/jwt.h"
#include "jwt_verify_lib/verify.h"

using ::google::jwt_verify::CheckAudience;
using ::google::jwt_verify::Status;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace JwtAuthn {
namespace {

/**
 * Object to implement Authenticator interface.
 */
class AuthenticatorImpl : public Logger::Loggable<Logger::Id::jwt>,
                          public Authenticator,
                          public Common::JwksFetcher::JwksReceiver {
public:
  AuthenticatorImpl(const CheckAudience* check_audience,
                    const absl::optional<std::string>& provider, bool allow_failed,
                    bool allow_missing, JwksCache& jwks_cache,
                    Upstream::ClusterManager& cluster_manager,
                    CreateJwksFetcherCb create_jwks_fetcher_cb, TimeSource& time_source)
      : jwks_cache_(jwks_cache), cm_(cluster_manager),
        create_jwks_fetcher_cb_(create_jwks_fetcher_cb), check_audience_(check_audience),
        provider_(provider), is_allow_failed_(allow_failed), is_allow_missing_(allow_missing),
        time_source_(time_source) {}

  // Following functions are for JwksFetcher::JwksReceiver interface
  void onJwksSuccess(google::jwt_verify::JwksPtr&& jwks) override;
  void onJwksError(Failure reason) override;
  // Following functions are for Authenticator interface.
  void verify(Http::HeaderMap& headers, Tracing::Span& parent_span,
              std::vector<JwtLocationConstPtr>&& tokens,
              SetExtractedJwtDataCallback set_extracted_jwt_data_cb,
              AuthenticatorCallback callback) override;
  void onDestroy() override;

  TimeSource& timeSource() { return time_source_; }

private:
  // Returns the name of the authenticator. For debug logging only.
  std::string name() const;

  // Verify with a specific public key.
  void verifyKey();

  // Handle Good Jwt either Cache JWT or verified public key.
  void handleGoodJwt(bool cache_hit);

  // Calls the callback with status.
  void doneWithStatus(const Status& status);

  // Start verification process. It will continue to eliminate tokens with invalid claims until it
  // finds one to verify with key.
  void startVerify();

  // The jwks cache object.
  JwksCache& jwks_cache_;
  // the cluster manager object.
  Upstream::ClusterManager& cm_;

  // The callback used to create a JwksFetcher instance.
  CreateJwksFetcherCb create_jwks_fetcher_cb_;

  // The Jwks fetcher object
  Common::JwksFetcherPtr fetcher_;

  // The token data
  std::vector<JwtLocationConstPtr> tokens_;
  JwtLocationConstPtr curr_token_;
  // The JWT object.
  std::unique_ptr<::google::jwt_verify::Jwt> owned_jwt_;
  // The JWKS data object
  JwksCache::JwksData* jwks_data_{};
  // The HTTP request headers
  Http::HeaderMap* headers_{};
  // The active span for the request
  Tracing::Span* parent_span_{&Tracing::NullSpan::instance()};
  // The callback function called to set the extracted payload and header from a verified JWT.
  SetExtractedJwtDataCallback set_extracted_jwt_data_cb_;
  // The on_done function.
  AuthenticatorCallback callback_;
  // check audience object.
  const CheckAudience* check_audience_;
  // specific provider or not when it is allow missing or failed.
  const absl::optional<std::string> provider_;
  const bool is_allow_failed_;
  const bool is_allow_missing_;
  TimeSource& time_source_;
  ::google::jwt_verify::Jwt* jwt_{};
};

std::string AuthenticatorImpl::name() const {
  if (provider_) {
    return provider_.value() + (is_allow_missing_ ? "-OPTIONAL" : "");
  }
  if (is_allow_failed_) {
    return "_IS_ALLOW_FAILED_";
  }
  if (is_allow_missing_) {
    return "_IS_ALLOW_MISSING_";
  }
  return "_UNKNOWN_";
}

void AuthenticatorImpl::verify(Http::HeaderMap& headers, Tracing::Span& parent_span,
                               std::vector<JwtLocationConstPtr>&& tokens,
                               SetExtractedJwtDataCallback set_extracted_jwt_data_cb,
                               AuthenticatorCallback callback) {
  ASSERT(!callback_);
  headers_ = &headers;
  parent_span_ = &parent_span;
  tokens_ = std::move(tokens);
  set_extracted_jwt_data_cb_ = std::move(set_extracted_jwt_data_cb);
  callback_ = std::move(callback);

  ENVOY_LOG(debug, "{}: JWT authentication starts (allow_failed={}), tokens size={}", name(),
            is_allow_failed_, tokens_.size());
 if (tokens_.empty()) {
    ENVOY_LOG(info,"token is empty");
    if(provider_.has_value()) {
       ENVOY_LOG(info, "provider has_value() - provider_ is: {} --> *provider_ is: {}", provider_.value(),*provider_);
      if(!jwks_cache_.stats().jwks_fetch_success_.name().empty()) {
        ENVOY_LOG(info,"jwks_cache_ is not nullptr");
        ENVOY_LOG(info,"jwks_cache_ jwks_fetch_success_.name() is: {}",jwks_cache_.stats().jwks_fetch_success_.name().c_str());
       // jwks_data_ = jwks_cache_.findByProvider(*provider_);
      } else {
        ENVOY_LOG(info,"jwks_cache_ is EMPTY");
      }
    }else {
      ENVOY_LOG(info,"provider_ has not value");
    }
    doneWithStatus(Status::JwtMissed);
    return;
  }
  ENVOY_LOG(info,"token contain value: {}", tokens_.data()->get()->token());

  startVerify();
}

void AuthenticatorImpl::startVerify() {
  ASSERT(!tokens_.empty());
  ENVOY_LOG(debug, "{}: startVerify: tokens size {}", name(), tokens_.size());
  curr_token_ = std::move(tokens_.back());
  tokens_.pop_back();
  
  ENVOY_LOG(info,"provider.has_value()={}",provider_.has_value());
  if (provider_.has_value()) {
    jwks_data_ = jwks_cache_.findByProvider(*provider_);
    jwt_ = jwks_data_->getJwtCache().lookup(curr_token_->token());
    if (jwt_ != nullptr) {
      handleGoodJwt(/*cache_hit=*/true);
      return;
    }
  }

  ENVOY_LOG(debug, "{}: Parse Jwt {}", name(), curr_token_->token());
  owned_jwt_ = std::make_unique<::google::jwt_verify::Jwt>();
  Status status = owned_jwt_->parseFromString(curr_token_->token());
  jwt_ = owned_jwt_.get();

  if (status != Status::Ok) {
    doneWithStatus(status);
    return;
  }

  ENVOY_LOG(debug, "{}: Verifying JWT token of issuer {}", name(), jwt_->iss_);
  // Check if `iss` is allowed.
  if (!curr_token_->isIssuerAllowed(jwt_->iss_)) {
    doneWithStatus(Status::JwtUnknownIssuer);
    return;
  }

  // Issuer is configured
  ENVOY_LOG(info,"!provider.has_value()={}",provider_.has_value());
  if (!provider_.has_value()) {
    jwks_data_ = jwks_cache_.findByIssuer(jwt_->iss_);
  }
  // When `provider` is valid, findByProvider should never return nullptr.
  // Only when `allow_missing` or `allow_failed` is used, `provider` is invalid,
  // and this authenticator is checking tokens from all providers. In this case,
  // Jwt `iss` field is used to find the first provider with the issuer.
  // If not found, use the first provider without issuer specified.
  // If still no found, fail the request with UnknownIssuer error.
  if (!jwks_data_) {
    doneWithStatus(Status::JwtUnknownIssuer);
    return;
  }

  // Default is 60 seconds
  uint64_t clock_skew_seconds = ::google::jwt_verify::kClockSkewInSecond;
  if (jwks_data_->getJwtProvider().clock_skew_seconds() > 0) {
    clock_skew_seconds = jwks_data_->getJwtProvider().clock_skew_seconds();
  }
  const uint64_t unix_timestamp = DateUtil::nowToSeconds(timeSource());
  status = jwt_->verifyTimeConstraint(unix_timestamp, clock_skew_seconds);
  if (status != Status::Ok) {
    doneWithStatus(status);
    return;
  }

  // Check if audience is allowed
  const bool is_allowed = check_audience_ ? check_audience_->areAudiencesAllowed(jwt_->audiences_)
                                          : jwks_data_->areAudiencesAllowed(jwt_->audiences_);
  if (!is_allowed) {
    doneWithStatus(Status::JwtAudienceNotAllowed);
    return;
  }

  auto jwks_obj = jwks_data_->getJwksObj();
  if (jwks_obj != nullptr && !jwks_data_->isExpired()) {
    // TODO(qiwzhang): It would seem there's a window of error whereby if the JWT issuer
    // has started signing with a new key that's not in our cache, then the
    // verification will fail even though the JWT is valid. A simple fix
    // would be to check the JWS kid header field; if present check we have
    // the key cached, if we do proceed to verify else try a new JWKS retrieval.
    // JWTs without a kid header field in the JWS we might be best to get each
    // time? This all only matters for remote JWKS.

    verifyKey();
    return;
  }

  // TODO(potatop): potential optimization.
  // Only one remote jwks will be fetched, verify will not continue until it is completed. This is
  // fine for provider name requirements, as each provider has only one issuer, but for allow
  // missing or failed there can be more than one issuers. This can be optimized; the same remote
  // jwks fetching can be shared by two requests.
  if (jwks_data_->getJwtProvider().has_remote_jwks()) {
    if (!fetcher_) {
      fetcher_ = create_jwks_fetcher_cb_(cm_, jwks_data_->getJwtProvider().remote_jwks());
    }
    fetcher_->fetch(*parent_span_, *this);
    return;
  }
  // No valid keys for this issuer. This may happen as a result of incorrect local
  // JWKS configuration.
  doneWithStatus(Status::JwksNoValidKeys);
}

void AuthenticatorImpl::onJwksSuccess(google::jwt_verify::JwksPtr&& jwks) {
  jwks_cache_.stats().jwks_fetch_success_.inc();
  const Status status = jwks_data_->setRemoteJwks(std::move(jwks))->getStatus();
  if (status != Status::Ok) {
    doneWithStatus(status);
  } else {
    verifyKey();
  }
}

void AuthenticatorImpl::onJwksError(Failure) {
  jwks_cache_.stats().jwks_fetch_failed_.inc();
  doneWithStatus(Status::JwksFetchFail);
}

void AuthenticatorImpl::onDestroy() {
  if (fetcher_) {
    fetcher_->cancel();
  }
}

// Verify with a specific public key.
void AuthenticatorImpl::verifyKey() {
  const Status status =
      ::google::jwt_verify::verifyJwtWithoutTimeChecking(*jwt_, *jwks_data_->getJwksObj());

  if (status != Status::Ok) {
    doneWithStatus(status);
    return;
  }
  handleGoodJwt(/*cache_hit=*/false);
}

void AuthenticatorImpl::handleGoodJwt(bool cache_hit) {
  // Forward the payload
  const auto& provider = jwks_data_->getJwtProvider();

  if (!provider.forward_payload_header().empty()) {
    if (provider.pad_forward_payload_header()) {
      std::string payload_with_padding = jwt_->payload_str_base64url_;
      Base64::completePadding(payload_with_padding);
      headers_->addCopy(Http::LowerCaseString(provider.forward_payload_header()),
                        payload_with_padding);
    } else {
      headers_->addCopy(Http::LowerCaseString(provider.forward_payload_header()),
                        jwt_->payload_str_base64url_);
    }
  }

  if (!provider.forward()) {
    // TODO(potatop) remove JWT from queries.
    // Remove JWT from headers.
    curr_token_->removeJwt(*headers_);
  }

  if (set_extracted_jwt_data_cb_) {
    if (!provider.header_in_metadata().empty()) {
      set_extracted_jwt_data_cb_(provider.header_in_metadata(), jwt_->header_pb_);
    }

    if (!provider.payload_in_metadata().empty()) {
      set_extracted_jwt_data_cb_(provider.payload_in_metadata(), jwt_->payload_pb_);
    }
  }
  if (provider_ && !cache_hit) {
    // move the ownership of "owned_jwt_" into the function.
    jwks_data_->getJwtCache().insert(curr_token_->token(), std::move(owned_jwt_));
  }
  doneWithStatus(Status::Ok);
}

void AuthenticatorImpl::doneWithStatus(const Status& status) {
  ENVOY_LOG(debug, "{}: JWT token verification completed with: {}", name(),
            ::google::jwt_verify::getStatusString(status));

  if(Status::Ok != status) {
    //Forward the failed status to dynamic metadata
    ENVOY_LOG(info, "status is: {}",::google::jwt_verify::getStatusString(status));
    ENVOY_LOG(info, "jwks_data_ is not nullptr? {}",jwks_data_!= nullptr);
    //if((jwks_data_ != nullptr) && !jwks_data_->getJwksObj()->keys().empty()){
    //ENVOY_LOG(info,
    //          "### inside if and status in jwks_data is: {}",
    //          jwks_data_->getJwksObj()->keys().data()->get()->hmac_key_);}
    //if ((jwks_data_ != nullptr) && ((jwks_data_->getJwksObj()->getStatus() == Status::JwtMissed) ||
    //  !jwks_data_->getJwtProvider().failed_status_in_metadata().empty())) {
    if (jwks_data_ != nullptr) {
      ENVOY_LOG(info, "getJwksObj is empty? {}",jwks_data_->getJwksObj()->keys().empty());
      ENVOY_LOG(info, "jwks_data_ has value: {]", jwks_data_->getJwksObj()->keys().data()->get()->alg_);
      if(!jwks_data_->getJwtProvider().failed_status_in_metadata().empty()) {
        ENVOY_LOG(info,"!jwks_data_->getJwtProvider().failed_status_in_metadata().empty() == {}",!jwks_data_->getJwtProvider().failed_status_in_metadata().empty());
      ProtobufWkt::Struct failed_status;
      auto& failed_status_fields = *failed_status.mutable_fields();
      failed_status_fields["status"].set_string_value(std::to_string(enumToInt(status)));
      ENVOY_LOG(debug, "Writing to metada failure reason: {}", google::jwt_verify::getStatusString(status));
      set_extracted_jwt_data_cb_(jwks_data_->getJwtProvider().failed_status_in_metadata(), failed_status);
    }
  }
  }

  // If a request has multiple tokens, all of them must be valid. Otherwise it may have
  // following security hole: a request has a good token and a bad one, it will pass
  // verification, forwarded to the backend, and the backend may mistakenly use the bad
  // token as the good one that passed the verification.

  // Unless allowing failed or missing, all tokens must be verified successfully.
  if ((Status::Ok != status && !is_allow_failed_ && !is_allow_missing_) || tokens_.empty()) {
    tokens_.clear();
    if (is_allow_failed_) {
      ENVOY_LOG(info, "inside doneWithStatus while is_allow_failed_ is:{}",is_allow_failed_);
      callback_(Status::Ok);
    } else if (is_allow_missing_ && status == Status::JwtMissed) {
      ENVOY_LOG(info, "inside doneWithStatus while is_allow_missing_ is:{}",is_allow_missing_);
      callback_(Status::Ok);
    } else {
      ENVOY_LOG(info, "inside doneWithStatus else");
      callback_(status);
    }

    callback_ = nullptr;
    return;
  }

  startVerify();
}

} // namespace

AuthenticatorPtr Authenticator::create(const CheckAudience* check_audience,
                                       const absl::optional<std::string>& provider,
                                       bool allow_failed, bool allow_missing, JwksCache& jwks_cache,
                                       Upstream::ClusterManager& cluster_manager,
                                       CreateJwksFetcherCb create_jwks_fetcher_cb,
                                       TimeSource& time_source) {
  return std::make_unique<AuthenticatorImpl>(check_audience, provider, allow_failed, allow_missing,
                                             jwks_cache, cluster_manager, create_jwks_fetcher_cb,
                                             time_source);
}

} // namespace JwtAuthn
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

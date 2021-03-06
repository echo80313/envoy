#include "common/router/retry_state_impl.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "common/common/assert.h"
#include "common/common/utility.h"
#include "common/grpc/common.h"
#include "common/http/codes.h"
#include "common/http/headers.h"
#include "common/http/utility.h"

namespace Envoy {
namespace Router {

// These are defined in envoy/router/router.h, however during certain cases the compiler is
// refusing to use the header version so allocate space here.
const uint32_t RetryPolicy::RETRY_ON_5XX;
const uint32_t RetryPolicy::RETRY_ON_GATEWAY_ERROR;
const uint32_t RetryPolicy::RETRY_ON_CONNECT_FAILURE;
const uint32_t RetryPolicy::RETRY_ON_RETRIABLE_4XX;
const uint32_t RetryPolicy::RETRY_ON_RETRIABLE_STATUS_CODES;
const uint32_t RetryPolicy::RETRY_ON_GRPC_CANCELLED;
const uint32_t RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED;
const uint32_t RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED;
const uint32_t RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE;

RetryStatePtr RetryStateImpl::create(const RetryPolicy& route_policy,
                                     Http::HeaderMap& request_headers,
                                     const Upstream::ClusterInfo& cluster, Runtime::Loader& runtime,
                                     Runtime::RandomGenerator& random,
                                     Event::Dispatcher& dispatcher,
                                     Upstream::ResourcePriority priority) {
  RetryStatePtr ret;

  // We short circuit here and do not bother with an allocation if there is no chance we will retry.
  if (request_headers.EnvoyRetryOn() || request_headers.EnvoyRetryGrpcOn() ||
      route_policy.retryOn()) {
    ret.reset(new RetryStateImpl(route_policy, request_headers, cluster, runtime, random,
                                 dispatcher, priority));
  }

  request_headers.removeEnvoyRetryOn();
  request_headers.removeEnvoyRetryGrpcOn();
  request_headers.removeEnvoyMaxRetries();
  return ret;
}

RetryStateImpl::RetryStateImpl(const RetryPolicy& route_policy, Http::HeaderMap& request_headers,
                               const Upstream::ClusterInfo& cluster, Runtime::Loader& runtime,
                               Runtime::RandomGenerator& random, Event::Dispatcher& dispatcher,
                               Upstream::ResourcePriority priority)
    : cluster_(cluster), runtime_(runtime), random_(random), dispatcher_(dispatcher),
      priority_(priority), retry_host_predicates_(route_policy.retryHostPredicates()),
      retry_priority_(route_policy.retryPriority()),
      retriable_status_codes_(route_policy.retriableStatusCodes()) {

  retry_on_ = route_policy.retryOn();
  retries_remaining_ = std::max(retries_remaining_, route_policy.numRetries());
  const uint32_t base = runtime_.snapshot().getInteger("upstream.base_retry_backoff_ms", 25);
  // Cap the max interval to 10 times the base interval to ensure reasonable backoff intervals.
  backoff_strategy_ = std::make_unique<JitteredBackOffStrategy>(base, base * 10, random_);
  host_selection_max_attempts_ = route_policy.hostSelectionMaxAttempts();

  // Merge in the headers.
  if (request_headers.EnvoyRetryOn()) {
    retry_on_ |= parseRetryOn(request_headers.EnvoyRetryOn()->value().c_str());
  }
  if (request_headers.EnvoyRetryGrpcOn()) {
    retry_on_ |= parseRetryGrpcOn(request_headers.EnvoyRetryGrpcOn()->value().c_str());
  }
  if (retry_on_ != 0 && request_headers.EnvoyMaxRetries()) {
    const char* max_retries = request_headers.EnvoyMaxRetries()->value().c_str();
    uint64_t temp;
    if (StringUtil::atoull(max_retries, temp)) {
      // The max retries header takes precedence if set.
      retries_remaining_ = temp;
    }
  }
  if (request_headers.EnvoyRetriableStatusCodes()) {
    for (const auto code : StringUtil::splitToken(
             request_headers.EnvoyRetriableStatusCodes()->value().getStringView(), ",")) {
      uint64_t out;
      if (StringUtil::atoull(std::string(code).c_str(), out)) {
        retriable_status_codes_.emplace_back(out);
      }
    }
  }
}

RetryStateImpl::~RetryStateImpl() { resetRetry(); }

void RetryStateImpl::enableBackoffTimer() {
  if (!retry_timer_) {
    retry_timer_ = dispatcher_.createTimer([this]() -> void { callback_(); });
  }

  // We use a fully jittered exponential backoff algorithm.
  retry_timer_->enableTimer(std::chrono::milliseconds(backoff_strategy_->nextBackOffMs()));
}

uint32_t RetryStateImpl::parseRetryOn(absl::string_view config) {
  uint32_t ret = 0;
  for (const auto retry_on : StringUtil::splitToken(config, ",")) {
    if (retry_on == Http::Headers::get().EnvoyRetryOnValues._5xx) {
      ret |= RetryPolicy::RETRY_ON_5XX;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.GatewayError) {
      ret |= RetryPolicy::RETRY_ON_GATEWAY_ERROR;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.ConnectFailure) {
      ret |= RetryPolicy::RETRY_ON_CONNECT_FAILURE;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.Retriable4xx) {
      ret |= RetryPolicy::RETRY_ON_RETRIABLE_4XX;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.RefusedStream) {
      ret |= RetryPolicy::RETRY_ON_REFUSED_STREAM;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnValues.RetriableStatusCodes) {
      ret |= RetryPolicy::RETRY_ON_RETRIABLE_STATUS_CODES;
    }
  }

  return ret;
}

uint32_t RetryStateImpl::parseRetryGrpcOn(absl::string_view retry_grpc_on_header) {
  uint32_t ret = 0;
  for (const auto retry_on : StringUtil::splitToken(retry_grpc_on_header, ",")) {
    if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.Cancelled) {
      ret |= RetryPolicy::RETRY_ON_GRPC_CANCELLED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.DeadlineExceeded) {
      ret |= RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.ResourceExhausted) {
      ret |= RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.Unavailable) {
      ret |= RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE;
    } else if (retry_on == Http::Headers::get().EnvoyRetryOnGrpcValues.Internal) {
      ret |= RetryPolicy::RETRY_ON_GRPC_INTERNAL;
    }
  }

  return ret;
}

void RetryStateImpl::resetRetry() {
  if (callback_) {
    cluster_.resourceManager(priority_).retries().dec();
    callback_ = nullptr;
  }
}

RetryStatus RetryStateImpl::shouldRetry(bool would_retry, DoRetryCallback callback) {
  // If a callback is armed from a previous shouldRetry and we don't need to
  // retry this particular request, we can infer that we did a retry earlier
  // and it was successful.
  if (callback_ && !would_retry) {
    cluster_.stats().upstream_rq_retry_success_.inc();
  }

  resetRetry();

  if (retries_remaining_ == 0) {
    return RetryStatus::NoRetryLimitExceeded;
  }

  retries_remaining_--;
  if (!would_retry) {
    return RetryStatus::No;
  }

  if (!cluster_.resourceManager(priority_).retries().canCreate()) {
    cluster_.stats().upstream_rq_retry_overflow_.inc();
    return RetryStatus::NoOverflow;
  }

  if (!runtime_.snapshot().featureEnabled("upstream.use_retry", 100)) {
    return RetryStatus::No;
  }

  ASSERT(!callback_);
  callback_ = callback;
  cluster_.resourceManager(priority_).retries().inc();
  cluster_.stats().upstream_rq_retry_.inc();
  enableBackoffTimer();
  return RetryStatus::Yes;
}

RetryStatus RetryStateImpl::shouldRetryHeaders(const Http::HeaderMap& response_headers,
                                               DoRetryCallback callback) {
  return shouldRetry(wouldRetryFromHeaders(response_headers), callback);
}

RetryStatus RetryStateImpl::shouldRetryReset(Http::StreamResetReason reset_reason,
                                             DoRetryCallback callback) {
  return shouldRetry(wouldRetryFromReset(reset_reason), callback);
}

bool RetryStateImpl::wouldRetryFromHeaders(const Http::HeaderMap& response_headers) {
  if (response_headers.EnvoyOverloaded() != nullptr) {
    return false;
  }

  // We never retry if the request is rate limited.
  if (response_headers.EnvoyRateLimited() != nullptr) {
    return false;
  }

  if (retry_on_ & RetryPolicy::RETRY_ON_5XX) {
    if (Http::CodeUtility::is5xx(Http::Utility::getResponseStatus(response_headers))) {
      return true;
    }
  }

  if (retry_on_ & RetryPolicy::RETRY_ON_GATEWAY_ERROR) {
    if (Http::CodeUtility::isGatewayError(Http::Utility::getResponseStatus(response_headers))) {
      return true;
    }
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_RETRIABLE_4XX)) {
    Http::Code code = static_cast<Http::Code>(Http::Utility::getResponseStatus(response_headers));
    if (code == Http::Code::Conflict) {
      return true;
    }
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_RETRIABLE_STATUS_CODES)) {
    for (auto code : retriable_status_codes_) {
      if (Http::Utility::getResponseStatus(response_headers) == code) {
        return true;
      }
    }
  }

  if (retry_on_ &
      (RetryPolicy::RETRY_ON_GRPC_CANCELLED | RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED |
       RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED | RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE |
       RetryPolicy::RETRY_ON_GRPC_INTERNAL)) {
    absl::optional<Grpc::Status::GrpcStatus> status = Grpc::Common::getGrpcStatus(response_headers);
    if (status) {
      if ((status.value() == Grpc::Status::Canceled &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_CANCELLED)) ||
          (status.value() == Grpc::Status::DeadlineExceeded &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_DEADLINE_EXCEEDED)) ||
          (status.value() == Grpc::Status::ResourceExhausted &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_RESOURCE_EXHAUSTED)) ||
          (status.value() == Grpc::Status::Unavailable &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_UNAVAILABLE)) ||
          (status.value() == Grpc::Status::Internal &&
           (retry_on_ & RetryPolicy::RETRY_ON_GRPC_INTERNAL))) {
        return true;
      }
    }
  }

  return false;
}

bool RetryStateImpl::wouldRetryFromReset(const Http::StreamResetReason reset_reason) {
  // First check "never retry" conditions so we can short circuit (we never
  // retry if the reset reason is overflow).
  if (reset_reason == Http::StreamResetReason::Overflow) {
    return false;
  }

  if (retry_on_ & (RetryPolicy::RETRY_ON_5XX | RetryPolicy::RETRY_ON_GATEWAY_ERROR)) {
    // Currently we count an upstream reset as a "5xx" (since it will result in
    // one). We may eventually split this out into its own type. I.e.,
    // RETRY_ON_RESET.
    return true;
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_REFUSED_STREAM) &&
      reset_reason == Http::StreamResetReason::RemoteRefusedStreamReset) {
    return true;
  }

  if ((retry_on_ & RetryPolicy::RETRY_ON_CONNECT_FAILURE) &&
      reset_reason == Http::StreamResetReason::ConnectionFailure) {
    return true;
  }

  return false;
}

} // namespace Router
} // namespace Envoy

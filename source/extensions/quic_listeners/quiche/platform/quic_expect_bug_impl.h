#pragma once

// NOLINT(namespace-envoy)
//
// This file is part of the QUICHE platform implementation, and is not to be
// consumed or referenced directly by other Envoy code. It serves purely as a
// porting layer for QUICHE.

#include "extensions/quic_listeners/quiche/platform/quic_logging_impl.h"
#include "extensions/quic_listeners/quiche/platform/quic_mock_log_impl.h"

#define EXPECT_QUIC_BUG_IMPL(statement, regex)                                                     \
  EXPECT_QUIC_DFATAL_IMPL(statement, testing::ContainsRegex(regex))

#define EXPECT_QUIC_PEER_BUG_IMPL(statement, regex)                                                \
  EXPECT_QUIC_LOG_IMPL(statement, ERROR, testing::ContainsRegex(regex))

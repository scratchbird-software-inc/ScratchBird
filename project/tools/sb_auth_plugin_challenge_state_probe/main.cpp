// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"

int main() {
  using namespace sb_auth_probe;
  auto valid = Request<EngineContinueAuthChallengeRequest>("webauthn");
  const auto valid_r = EngineContinueAuthChallenge(valid);
  auto expired = valid;
  expired.option_envelopes.push_back("challenge_expired:true");
  const auto expired_r = EngineContinueAuthChallenge(expired);
  auto replay = valid;
  replay.option_envelopes.push_back("challenge_replayed:true");
  const auto replay_r = EngineContinueAuthChallenge(replay);
  auto limit = valid;
  limit.option_envelopes.push_back("attempt_limit_exceeded:true");
  const auto limit_r = EngineContinueAuthChallenge(limit);
  return Finish({
      {"challenge_ok", valid_r.ok && valid_r.challenge_accepted},
      {"expired_rejected",
       HasDiagnostic(expired_r, "SECURITY.AUTHENTICATION.FAILED",
                     "challenge_expired")},
      {"replay_rejected",
       HasDiagnostic(replay_r, "SECURITY.AUTHENTICATION.FAILED",
                     "challenge_replay_denied")},
      {"limit_rejected",
       HasDiagnostic(limit_r, "SECURITY.AUTHENTICATION.FAILED",
                     "challenge_attempt_limit_exceeded")},
  });
}

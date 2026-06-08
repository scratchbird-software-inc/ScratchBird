// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto ok=Request<EngineContinueAuthChallengeRequest>("webauthn"); auto ok_r=EngineContinueAuthChallenge(ok); auto expired=Request<EngineContinueAuthChallengeRequest>("webauthn"); expired.option_envelopes.push_back("challenge_expired:true"); auto exp_r=EngineContinueAuthChallenge(expired); auto replay=Request<EngineContinueAuthChallengeRequest>("webauthn"); replay.option_envelopes.push_back("challenge_replayed:true"); auto rep_r=EngineContinueAuthChallenge(replay); auto limit=Request<EngineContinueAuthChallengeRequest>("webauthn"); limit.option_envelopes.push_back("attempt_limit_exceeded:true"); auto lim_r=EngineContinueAuthChallenge(limit); return Finish({{"challenge_ok",ok_r.ok&&ok_r.challenge_accepted},{"expired_rejected",!exp_r.ok},{"replay_rejected",!rep_r.ok},{"limit_rejected",!lim_r.ok}});}

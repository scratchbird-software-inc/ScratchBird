# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "scratchbird/client"
require "scratchbird/connection"
require "scratchbird/errors"
require "scratchbird/protocol"
require "scratchbird/result"
require "scratchbird/scram"
require "scratchbird/sql"
require "scratchbird/statement"
require "scratchbird/types"
require "scratchbird/config"
require "scratchbird/metadata"

module Scratchbird
  def self.connect(uri_or_opts)
    Connection.new(uri_or_opts)
  end

  def self.probe_auth_surface(uri_or_opts = nil)
    cfg =
      case uri_or_opts
      when Config
        uri_or_opts
      when String
        Config.parse(uri_or_opts)
      when Hash
        cfg = Config.new
        uri_or_opts.each do |key, value|
          key_s = key.to_s
          setter = "#{key_s}="
          if cfg.respond_to?(setter)
            cfg.public_send(setter, value)
          else
            Config.apply_param(cfg, key_s, value)
          end
        end
        cfg
      when nil
        Config.new
      else
        raise ArgumentError, "unsupported connection options"
      end

    Client.new(cfg).probe_auth_surface
  end
end

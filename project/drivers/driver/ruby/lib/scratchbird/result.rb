# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

module Scratchbird
  Column = Struct.new(:name, :type_oid, :type_modifier, :format, :nullable, keyword_init: true)
  FieldSummary = Struct.new(:name, :type_oid, :format, :nullable, keyword_init: true)
  ResultSetSummary = Struct.new(:rows, :rowcount, :fields, :command, :last_insert_id, keyword_init: true)
  BatchItemSummary = Struct.new(:index, :rowcount, :fields, :command, :last_insert_id, keyword_init: true)
  BatchSummary = Struct.new(:items, :total_rowcount, keyword_init: true)

  class Result
    attr_reader :columns, :rowcount, :command_tag, :last_insert_id

    def initialize(columns, rows, rowcount, command_tag = "", last_insert_id = 0)
      @columns = (columns || []).map do |col|
        Column.new(
          name: col[:name],
          type_oid: col[:type_oid],
          type_modifier: col[:type_modifier],
          format: col[:format],
          nullable: col[:nullable]
        )
      end
      @rows = rows || []
      @rowcount = rowcount.to_i
      @command_tag = command_tag.to_s
      @last_insert_id = last_insert_id.to_i
    end

    def rows
      @rows
    end

    def fields
      @columns.map(&:name)
    end

    def each
      return enum_for(:each) unless block_given?
      @rows.each { |row| yield row }
    end

    def each_hash
      return enum_for(:each_hash) unless block_given?
      @rows.each do |row|
        yield to_hash(row)
      end
    end

    def first
      @rows.first
    end

    private

    def to_hash(row)
      data = {}
      @columns.each_with_index do |col, idx|
        key = col.name || idx
        data[key] = row[idx]
      end
      data
    end
  end
end

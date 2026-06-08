# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

module Scratchbird
  module Sql
    NormalizedQuery = Struct.new(:sql, :params, keyword_init: true)

    def self.normalize(sql, params = nil)
      return NormalizedQuery.new(sql: sql, params: []) if params.nil?
      if params.is_a?(Array)
        if sql.include?("?")
          return rewrite_positional(sql, params)
        end
        return NormalizedQuery.new(sql: sql, params: params)
      end
      unless has_named_params?(sql)
        raise ArgumentError, "named parameters provided but query has no named placeholders"
      end
      rewrite_named(sql, params)
    end

    def self.normalize_prepared_sql(sql)
      return rewrite_placeholder_sql(sql) if sql.include?("?")
      return rewrite_named_placeholder_sql(sql) if has_named_params?(sql)

      sql
    end

    def self.normalize_callable(sql, params = nil)
      callable_sql = normalize_callable_sql(sql)
      normalize(callable_sql, params)
    end

    def self.normalize_callable_sql(sql)
      trimmed = sql.to_s.strip
      return sql unless trimmed.start_with?("{") && trimmed.end_with?("}")

      inner = trimmed[1...-1].to_s.strip
      return sql if inner.empty?

      if inner.start_with?("?")
        after_question = inner[1..].to_s.lstrip
        if after_question.start_with?("=")
          after_equals = after_question[1..].to_s.lstrip
          if starts_with_call?(after_equals)
            invocation = parse_callable_invocation(after_equals[4..].to_s.lstrip)
            args = invocation[:has_parens] ? invocation[:args] : ""
            return "select #{invocation[:routine]}(#{args}) as return_value"
          end
        end
      end

      if starts_with_call?(inner)
        invocation = parse_callable_invocation(inner[4..].to_s.lstrip)
        return invocation[:has_parens] ? "call #{invocation[:routine]}(#{invocation[:args]})" : "call #{invocation[:routine]}"
      end

      sql
    end

    def self.has_named_params?(sql)
      in_string = false
      i = 0
      while i + 1 < sql.length
        ch = sql[i]
        if ch == "'"
          in_string = !in_string
          i += 1
          next
        end
        unless in_string
          if (ch == ":" || ch == "@") && ident_start?(sql[i + 1])
            return true
          end
        end
        i += 1
      end
      false
    end

    def self.rewrite_named(sql, params)
      lookup = {}
      params.each do |key, value|
        next unless key.is_a?(String) || key.is_a?(Symbol)
        lookup[key.to_s.sub(/\A[@:]/, "")] = value
      end
      out = +""
      ordered = []
      in_string = false
      i = 0
      while i < sql.length
        ch = sql[i]
        if ch == "'"
          in_string = !in_string
          out << ch
          i += 1
          next
        end
        unless in_string
          if (ch == ":" || ch == "@") && i + 1 < sql.length && ident_start?(sql[i + 1])
            j = i + 1
            j += 1 while j < sql.length && ident_part?(sql[j])
            name = sql[(i + 1)...j]
            raise ArgumentError, "missing named parameter: #{name}" unless lookup.key?(name)
            ordered << lookup[name]
            out << "$#{ordered.length}"
            i = j
            next
          end
        end
        out << ch
        i += 1
      end
      NormalizedQuery.new(sql: out, params: ordered)
    end

    def self.rewrite_positional(sql, params)
      out = +""
      ordered = []
      idx = 0
      in_string = false
      i = 0
      while i < sql.length
        ch = sql[i]
        if ch == "'"
          in_string = !in_string
          out << ch
          i += 1
          next
        end
        if !in_string && ch == "?"
          raise ArgumentError, "not enough parameters" if idx >= params.length
          ordered << params[idx]
          idx += 1
          out << "$#{ordered.length}"
          i += 1
          next
        end
        out << ch
        i += 1
      end
      raise ArgumentError, "too many parameters" if idx < params.length
      NormalizedQuery.new(sql: out, params: ordered)
    end

    def self.rewrite_placeholder_sql(sql)
      out = +""
      index = 0
      in_string = false
      i = 0
      while i < sql.length
        ch = sql[i]
        if ch == "'"
          in_string = !in_string
          out << ch
          i += 1
          next
        end
        if !in_string && ch == "?"
          index += 1
          out << "$#{index}"
          i += 1
          next
        end
        out << ch
        i += 1
      end
      out
    end

    def self.rewrite_named_placeholder_sql(sql)
      out = +""
      in_string = false
      index = 0
      i = 0
      while i < sql.length
        ch = sql[i]
        if ch == "'"
          in_string = !in_string
          out << ch
          i += 1
          next
        end
        unless in_string
          if (ch == ":" || ch == "@") && i + 1 < sql.length && ident_start?(sql[i + 1])
            j = i + 1
            j += 1 while j < sql.length && ident_part?(sql[j])
            index += 1
            out << "$#{index}"
            i = j
            next
          end
        end
        out << ch
        i += 1
      end
      out
    end

    def self.ident_start?(ch)
      /[A-Za-z_]/.match?(ch)
    end

    def self.ident_part?(ch)
      /[A-Za-z0-9_]/.match?(ch)
    end

    def self.starts_with_call?(value)
      value.to_s[0, 4].to_s.casecmp("call").zero?
    end

    def self.parse_callable_invocation(value)
      text = value.to_s
      open_paren = text.index("(")
      if open_paren.nil?
        routine = text.strip
        raise ArgumentError, "invalid JDBC escape call syntax" if routine.empty?
        return { routine: routine, args: "", has_parens: false }
      end

      in_single = false
      in_double = false
      depth = 0
      close_paren = nil
      i = open_paren
      while i < text.length
        ch = text[i]
        if ch == "'" && !in_double
          if in_single && i + 1 < text.length && text[i + 1] == "'"
            i += 2
            next
          end
          in_single = !in_single
          i += 1
          next
        end
        if ch == '"' && !in_single
          if in_double && i + 1 < text.length && text[i + 1] == '"'
            i += 2
            next
          end
          in_double = !in_double
          i += 1
          next
        end
        if in_single || in_double
          i += 1
          next
        end
        if ch == "("
          depth += 1
          i += 1
          next
        end
        if ch == ")"
          depth -= 1
          if depth == 0
            close_paren = i
            break
          end
        end
        i += 1
      end

      raise ArgumentError, "invalid JDBC escape call syntax" if close_paren.nil?

      routine = text[0...open_paren].to_s.strip
      raise ArgumentError, "invalid JDBC escape call syntax" if routine.empty?

      trailing = text[(close_paren + 1)..].to_s.strip
      raise ArgumentError, "invalid JDBC escape call syntax" unless trailing.empty?

      args = text[(open_paren + 1)...close_paren].to_s.strip
      { routine: routine, args: args, has_parens: true }
    end
  end
end

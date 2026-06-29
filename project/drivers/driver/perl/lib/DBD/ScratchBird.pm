# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

package DBD::ScratchBird;

use strict;
use warnings;
use bytes;

our $VERSION = '0.01';
our $drh;

sub driver {
    my ($class, $attr) = @_;
    return $drh if defined $drh;
    require DBI;
    $class .= '::dr';
    $drh = DBI::_new_drh(
        $class,
        {
            Name        => 'ScratchBird',
            Version     => $VERSION,
            Attribution => 'ScratchBird DBI driver',
        },
    );
    return $drh;
}

sub CLONE {
    undef $drh;
}

package DBD::ScratchBird::dr;

use strict;
use warnings;

our $imp_data_size = 0;

sub connect {
    my ($drh, $dsn, $user, $auth, $attr) = @_;
    require DBI;
    my $cfg = DBD::ScratchBird::Util::parse_dsn($dsn);
    $cfg->{user} = defined $user ? $user : ($cfg->{user} // '');
    $cfg->{password} = defined $auth ? $auth : ($cfg->{password} // '');
    my $socket = eval { DBD::ScratchBird::Util::open_transport($cfg) };
    if (!$socket) {
        my $message = $@ || 'ScratchBird transport open failed';
        $drh->set_err(1, $message, DBD::ScratchBird::Util::sqlstate_from_error($message));
        return;
    }
    my $state = eval { DBD::ScratchBird::Util::startup_auth($socket, $cfg) };
    if (!$state) {
        my $message = $@ || 'ScratchBird startup/auth failed';
        eval { close $socket };
        $drh->set_err(1, $message, DBD::ScratchBird::Util::sqlstate_from_error($message));
        return;
    }
    my $dbh = DBI::_new_dbh(
        $drh,
        {
            Name => $cfg->{database} // '',
        },
    );
    $dbh->{scratchbird_private_config} = $cfg;
    $dbh->{scratchbird_private_socket} = $socket;
    $dbh->{scratchbird_private_state} = $state;
    $dbh->STORE(Active => 1);
    return $dbh;
}

sub data_sources {
    return;
}

package DBD::ScratchBird::db;

use strict;
use warnings;

our $imp_data_size = 0;

sub prepare {
    my ($dbh, $statement, @attr) = @_;
    require DBI;
    return DBI::_new_sth(
        $dbh,
        {
            Statement => $statement,
            NUM_OF_FIELDS => 0,
            NAME => [],
            scratchbird_sql => $statement,
            scratchbird_rows => [],
            scratchbird_pos => 0,
            scratchbird_rowcount => -1,
        },
    );
}

sub commit {
    my ($dbh) = @_;
    my $ok = eval {
        DBD::ScratchBird::Util::txn_control($dbh, 'commit');
        1;
    };
    if (!$ok) {
        $dbh->set_err(1, $@ || 'ScratchBird commit failed', DBD::ScratchBird::Util::sqlstate_from_error($@));
        return;
    }
    return 1;
}

sub rollback {
    my ($dbh) = @_;
    my $ok = eval {
        DBD::ScratchBird::Util::txn_control($dbh, 'rollback');
        1;
    };
    if (!$ok) {
        $dbh->set_err(1, $@ || 'ScratchBird rollback failed', DBD::ScratchBird::Util::sqlstate_from_error($@));
        return;
    }
    return 1;
}

sub savepoint {
    my ($dbh, $name) = @_;
    my $ok = eval {
        DBD::ScratchBird::Util::txn_control($dbh, 'savepoint', $name);
        1;
    };
    if (!$ok) {
        $dbh->set_err(1, $@ || 'ScratchBird savepoint failed', DBD::ScratchBird::Util::sqlstate_from_error($@));
        return;
    }
    return 1;
}

sub release_savepoint {
    my ($dbh, $name) = @_;
    my $ok = eval {
        DBD::ScratchBird::Util::txn_control($dbh, 'release_savepoint', $name);
        1;
    };
    if (!$ok) {
        $dbh->set_err(1, $@ || 'ScratchBird release savepoint failed', DBD::ScratchBird::Util::sqlstate_from_error($@));
        return;
    }
    return 1;
}

sub rollback_to_savepoint {
    my ($dbh, $name) = @_;
    my $ok = eval {
        DBD::ScratchBird::Util::txn_control($dbh, 'rollback_to_savepoint', $name);
        1;
    };
    if (!$ok) {
        $dbh->set_err(1, $@ || 'ScratchBird rollback to savepoint failed', DBD::ScratchBird::Util::sqlstate_from_error($@));
        return;
    }
    return 1;
}

sub begin_work {
    my ($dbh) = @_;
    my $ok = eval {
        DBD::ScratchBird::Util::txn_control($dbh, 'begin');
        1;
    };
    if (!$ok) {
        $dbh->set_err(1, $@ || 'ScratchBird begin failed', DBD::ScratchBird::Util::sqlstate_from_error($@));
        return;
    }
    return 1;
}

sub disconnect {
    my ($dbh) = @_;
    my $socket = delete $dbh->{scratchbird_private_socket};
    if ($socket) {
        eval { close $socket };
    }
    $dbh->STORE(Active => 0);
    return 1;
}

sub ping {
    my ($dbh) = @_;
    return $dbh->FETCH('Active') ? 1 : 0;
}

sub FETCH {
    my ($dbh, $attrib) = @_;
    return $dbh->SUPER::FETCH($attrib);
}

sub STORE {
    my ($dbh, $attrib, $value) = @_;
    if ($attrib eq 'AutoCommit') {
        $value = $value ? -901 : -900;
    }
    return $dbh->{$attrib} = $value if $attrib =~ /^scratchbird_/;
    return $dbh->SUPER::STORE($attrib, $value);
}

sub table_info {
    my ($dbh, $catalog, $schema, $table, $type, $attr) = @_;
    return $dbh->prepare('SELECT * FROM sys.tables');
}

sub column_info {
    my ($dbh, $catalog, $schema, $table, $column) = @_;
    return $dbh->prepare('SELECT * FROM sys.columns');
}

package DBD::ScratchBird::st;

use strict;
use warnings;

our $imp_data_size = 0;

sub execute {
    my ($sth, @bind_values) = @_;
    if (@bind_values) {
        $sth->set_err(1, '07002: ScratchBird Perl DBI execution currently accepts SQL text without client-side bind rewriting', '07002');
        return;
    }
    my $dbh = $sth->{Database};
    my $result = eval { DBD::ScratchBird::Util::execute_statement($dbh, $sth->{scratchbird_sql}) };
    if (!$result) {
        my $message = $@ || 'ScratchBird execute failed';
        $sth->set_err(1, $message, DBD::ScratchBird::Util::sqlstate_from_error($message));
        return;
    }
    $sth->{scratchbird_rows} = $result->{rows} || [];
    $sth->{scratchbird_pos} = 0;
    $sth->{scratchbird_rowcount} = $result->{rowcount};
    $sth->{NAME} = $result->{columns} || [];
    $sth->{NUM_OF_FIELDS} = scalar @{$result->{columns} || []};
    return $result->{rowcount} > 0 ? $result->{rowcount} : '0E0';
}

sub fetchrow_arrayref {
    my ($sth) = @_;
    my $rows = $sth->{scratchbird_rows} || [];
    my $pos = $sth->{scratchbird_pos} || 0;
    return if $pos >= @{$rows};
    $sth->{scratchbird_pos} = $pos + 1;
    return $rows->[$pos];
}

sub fetch {
    my ($sth) = @_;
    return $sth->fetchrow_arrayref;
}

sub finish {
    return 1;
}

sub rows {
    my ($sth) = @_;
    return defined $sth->{scratchbird_rowcount} ? $sth->{scratchbird_rowcount} : -1;
}

sub FETCH {
    my ($sth, $attrib) = @_;
    return $sth->SUPER::FETCH($attrib);
}

sub STORE {
    my ($sth, $attrib, $value) = @_;
    return $sth->{$attrib} = $value
        if $attrib eq 'NAME'
        || $attrib eq 'NUM_OF_FIELDS'
        || $attrib =~ /^scratchbird_/;
    return $sth->SUPER::STORE($attrib, $value);
}

package DBD::ScratchBird::Util;

use strict;
use warnings;
use Socket qw(SOCK_STREAM);

sub parse_dsn {
    my ($dsn) = @_;
    my %cfg = (
        host => '127.0.0.1',
        port => 3092,
        sslmode => 'require',
        transport => 'inet',
        role => '',
        ipc_path => '',
    );
    for my $part (split /;/, ($dsn // '')) {
        next if $part eq '';
        my ($key, $value) = split /=/, $part, 2;
        next unless defined $key;
        $cfg{$key} = defined $value ? $value : '';
    }
    $cfg{database} = $cfg{dbname} if !defined $cfg{database} && defined $cfg{dbname};
    return \%cfg;
}

sub open_transport {
    my ($cfg) = @_;
    my $transport = $cfg->{transport} // 'inet';
    if ($transport eq 'embedded') {
        die "0A000: embedded transport is unavailable in the Perl lane because no ScratchBird C++ library boundary is exposed";
    }
    if ($transport eq 'ipc') {
        die "08001: ipc_path is required for local IPC transport" unless defined $cfg->{ipc_path} && length $cfg->{ipc_path};
        require IO::Socket::UNIX;
        my $socket = IO::Socket::UNIX->new(Type => SOCK_STREAM, Peer => $cfg->{ipc_path});
        die "08001: local IPC connect failed: $!" unless $socket;
        return $socket;
    }
    if ($transport eq 'inet') {
        if (($cfg->{sslmode} // 'require') ne 'disable') {
            eval { require IO::Socket::SSL; 1 }
                or die "0A000: Perl ScratchBird DBI lane requires IO::Socket::SSL for TLS transport";
            my $sslmode = $cfg->{sslmode} // 'require';
            my %tls_options = (
                PeerHost => $cfg->{host},
                PeerPort => int($cfg->{port} // 3092),
                SSL_hostname => $cfg->{host},
            );
            if ($sslmode eq 'verify-ca' || $sslmode eq 'verify-full') {
                die "08001: sslrootcert is required for sslmode=$sslmode"
                    unless defined $cfg->{sslrootcert} && length $cfg->{sslrootcert};
                $tls_options{SSL_ca_file} = $cfg->{sslrootcert};
                $tls_options{SSL_verify_mode} = IO::Socket::SSL::SSL_VERIFY_PEER();
                $tls_options{SSL_verifycn_scheme} = 'http' if $sslmode eq 'verify-full';
                $tls_options{SSL_verifycn_scheme} = 'none' if $sslmode eq 'verify-ca';
            } else {
                $tls_options{SSL_verify_mode} = IO::Socket::SSL::SSL_VERIFY_NONE();
                $tls_options{SSL_verifycn_scheme} = 'none';
            }
            my $socket = IO::Socket::SSL->new(
                %tls_options,
            );
            die "08001: TLS TCP connect failed: " . IO::Socket::SSL::errstr() unless $socket;
            return $socket;
        }
        require IO::Socket::INET;
        my $socket = IO::Socket::INET->new(
            PeerHost => $cfg->{host},
            PeerPort => int($cfg->{port} // 3092),
            Proto => 'tcp',
        );
        die "08001: TCP connect failed: $!" unless $socket;
        return $socket;
    }
    die "08001: unsupported ScratchBird transport: $transport";
}

use constant {
    SBWP_VERSION => 0x0101,
    MSG_STARTUP => 0x01,
    MSG_AUTH_RESPONSE => 0x02,
    MSG_QUERY => 0x03,
    MSG_SYNC => 0x09,
    MSG_TERMINATE => 0x0c,
    MSG_COPY_DATA => 0x0d,
    MSG_COPY_DONE => 0x0e,
    MSG_COPY_FAIL => 0x0f,
    MSG_SBLR_EXECUTE => 0x10,
    MSG_TXN_BEGIN => 0x15,
    MSG_TXN_COMMIT => 0x16,
    MSG_TXN_ROLLBACK => 0x17,
    MSG_TXN_SAVEPOINT => 0x18,
    MSG_TXN_RELEASE => 0x19,
    MSG_TXN_ROLLBACK_TO => 0x1a,
    MSG_AUTH_REQUEST => 0x40,
    MSG_AUTH_OK => 0x41,
    MSG_AUTH_CONTINUE => 0x42,
    MSG_READY => 0x43,
    MSG_ROW_DESCRIPTION => 0x44,
    MSG_DATA_ROW => 0x45,
    MSG_COMMAND_COMPLETE => 0x46,
    MSG_ERROR => 0x48,
    MSG_NOTICE => 0x49,
    MSG_PARAMETER_STATUS => 0x4f,
    MSG_COPY_IN_RESPONSE => 0x51,
    MSG_COPY_OUT_RESPONSE => 0x52,
    MSG_COPY_BOTH_RESPONSE => 0x53,
    MSG_SBLR_COMPILED => 0x57,
    MSG_TXN_STATUS => 0x5c,
    AUTH_OK => 0,
    AUTH_PASSWORD => 1,
    FEATURE_STREAMING => 1 << 1,
    FEATURE_SBLR => 1 << 2,
    FEATURE_NOTIFICATIONS => 1 << 4,
    FEATURE_QUERY_PLAN => 1 << 5,
    FEATURE_SAVEPOINTS => 1 << 9,
    QUERY_FLAG_BINARY_RESULT => 0x04,
    QUERY_FLAG_RETURN_SBLR => 0x10,
};

sub startup_auth {
    my ($socket, $cfg) = @_;
    my $state = {
        attachment_id => "\0" x 16,
        txn_id => 0,
        sequence => 0,
        parameters => {},
    };
    my %params = (
        database => $cfg->{database} // '',
        user => $cfg->{user} // '',
        client_flags => '256',
        application_name => 'ScratchBirdPerl',
    );
    $params{role} = $cfg->{role} if defined $cfg->{role} && length $cfg->{role};
    my $features = FEATURE_SBLR | FEATURE_NOTIFICATIONS | FEATURE_QUERY_PLAN | FEATURE_STREAMING | FEATURE_SAVEPOINTS;
    send_message($socket, $state, MSG_STARTUP, build_startup_payload($features, 0, \%params), force_zero => 1);
    while (1) {
        my ($type, undef, undef, $attachment, $txn_id, $payload) = recv_message($socket);
        next if handle_async($state, $type, $attachment, $txn_id, $payload);
        if ($type == MSG_AUTH_REQUEST) {
            die "08006: authentication request is truncated" unless length($payload) >= 1;
            my $method = unpack('C', substr($payload, 0, 1));
            next if $method == AUTH_OK;
            if ($method == AUTH_PASSWORD) {
                send_message($socket, $state, MSG_AUTH_RESPONSE, $cfg->{password} // '', force_zero => 1);
                next;
            }
            die "0A000: Perl driver supports PASSWORD authentication for this release path; requested method $method requires a broker or dedicated auth plugin";
        }
        if ($type == MSG_AUTH_CONTINUE) {
            die "0A000: Perl driver authentication continuation requires broker or external ceremony support for this release path";
        }
        if ($type == MSG_AUTH_OK) {
            $state->{attachment_id} = $attachment;
            $state->{txn_id} = $txn_id;
            next;
        }
        if ($type == MSG_READY) {
            apply_ready($state, $payload);
            return $state;
        }
        raise_error($payload) if $type == MSG_ERROR;
    }
}

sub execute_statement {
    my ($dbh, $sql) = @_;
    my $cfg = $dbh->{scratchbird_private_config};
    if (first_sql_token($sql) eq 'copy') {
        return execute_copy_from_stdin($dbh, $sql, copy_input_for_statement($sql));
    }
    if (($cfg->{parser_mode} // 'server-parser') eq 'server-parser') {
        return execute_query($dbh, $sql, 0);
    }
    my $compiled = compile_sblr($dbh, $sql);
    return execute_sblr($dbh, $compiled);
}

sub execute_query {
    my ($dbh, $sql, $extra_flags) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    my $flags = QUERY_FLAG_BINARY_RESULT | ($extra_flags || 0);
    my $payload = pack('V V V', $flags, 0, 0) . $sql . "\0";
    send_message($socket, $state, MSG_QUERY, $payload);
    return read_result($dbh);
}

sub execute_copy_from_stdin {
    my ($dbh, $sql, $copy_input) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    my $payload = pack('V V V', QUERY_FLAG_BINARY_RESULT, 0, 0) . $sql . "\0";
    send_message($socket, $state, MSG_QUERY, $payload);
    return read_result($dbh, $copy_input);
}

sub compile_sblr {
    my ($dbh, $sql) = @_;
    execute_query($dbh, $sql, QUERY_FLAG_RETURN_SBLR);
    my $state = $dbh->{scratchbird_private_state};
    die "HY000: parser did not return compiled SBLR"
        unless defined $state->{last_sblr};
    my $compiled = delete $state->{last_sblr};
    return $compiled;
}

sub execute_sblr {
    my ($dbh, $compiled) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    my $bytecode = $compiled->{bytecode} || '';
    my $payload = pack('Q< V v v', $compiled->{hash}, length($bytecode), 0, 0) . $bytecode;
    send_message($socket, $state, MSG_SBLR_EXECUTE, $payload);
    send_message($socket, $state, MSG_SYNC, '');
    return read_result($dbh);
}

sub txn_control {
    my ($dbh, $action, $name) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    if ($action eq 'begin') {
        return 1 if transaction_active($state);
        send_message($socket, $state, MSG_TXN_BEGIN, pack('v C C C C C C V', 0, 0, 0, 1, 0, 0, 0, 0));
    } elsif ($action eq 'commit') {
        return 1 unless transaction_active($state);
        send_message($socket, $state, MSG_TXN_COMMIT, "\0\0\0\0");
    } elsif ($action eq 'rollback') {
        return 1 unless transaction_active($state);
        send_message($socket, $state, MSG_TXN_ROLLBACK, "\0\0\0\0");
    } elsif ($action eq 'savepoint') {
        send_message($socket, $state, MSG_TXN_SAVEPOINT, txn_savepoint_payload($name));
    } elsif ($action eq 'release_savepoint') {
        send_message($socket, $state, MSG_TXN_RELEASE, txn_savepoint_payload($name));
    } elsif ($action eq 'rollback_to_savepoint') {
        send_message($socket, $state, MSG_TXN_ROLLBACK_TO, txn_savepoint_payload($name));
    } else {
        die "HY000: unknown transaction action $action";
    }
    drain_until_ready($dbh);
    drain_immediate_reopen_boundary($dbh) if $action eq 'commit' || $action eq 'rollback';
    return 1;
}

sub transaction_active {
    my ($state) = @_;
    return ($state->{txn_id} || 0) != 0;
}

sub txn_savepoint_payload {
    my ($name) = @_;
    $name //= '';
    $name =~ s/\A\s+//;
    $name =~ s/\s+\z//;
    die "3B000: savepoint name is required" unless length $name;
    return pack('V', length($name)) . $name;
}

sub read_result {
    my ($dbh, $copy_input) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    my @columns;
    my @types;
    my @rows;
    my $rowcount = -1;
    my $tag = '';
    my $response_started = 0;
    my $ignored_stray_ready = 0;
    while (1) {
        my ($type, undef, undef, $attachment, $txn_id, $payload) = recv_message($socket);
        next if handle_async($state, $type, $attachment, $txn_id, $payload);
        if ($type == MSG_ROW_DESCRIPTION) {
            $response_started = 1;
            my $desc = parse_row_description($payload);
            @columns = @{$desc->{columns}};
            @types = @{$desc->{types}};
        } elsif ($type == MSG_DATA_ROW) {
            $response_started = 1;
            push @rows, parse_data_row($payload, scalar(@columns), \@types);
        } elsif ($type == MSG_COMMAND_COMPLETE) {
            $response_started = 1;
            my $complete = parse_command_complete($payload);
            $rowcount = $complete->{rows};
            $tag = $complete->{tag};
        } elsif ($type == MSG_COPY_IN_RESPONSE) {
            die "HY000: COPY FROM STDIN requires SB_COPY_INPUT rows in the script"
                unless defined $copy_input && length $copy_input;
            send_message($socket, $state, MSG_COPY_DATA, $copy_input);
            send_message($socket, $state, MSG_COPY_DONE, '');
        } elsif ($type == MSG_COPY_DONE) {
            next;
        } elsif ($type == MSG_COPY_FAIL) {
            raise_copy_fail($payload);
        } elsif ($type == MSG_READY) {
            apply_ready($state, $payload);
            if (!$response_started && !$ignored_stray_ready) {
                $ignored_stray_ready = 1;
                next;
            }
            return {
                columns => \@columns,
                rows => \@rows,
                rowcount => $rowcount >= 0 ? $rowcount : scalar(@rows),
                command_tag => $tag,
            };
        } elsif ($type == MSG_ERROR) {
            raise_error($payload);
        }
    }
}

sub copy_input_for_statement {
    my ($sql) = @_;
    my $marker = '-- SB_COPY_INPUT ';
    my $payload = '';
    for my $line (split /\n/, ($sql // '')) {
        $line =~ s/\r\z//;
        $line =~ s/\A[ \t]+//;
        next unless index($line, $marker) == 0;
        $payload .= substr($line, length($marker)) . "\n";
    }
    return $payload;
}

sub first_sql_token {
    my ($sql) = @_;
    my @tokens = grep { length($_) } map {
        my $line = $_;
        $line =~ s/--.*\z//;
        $line =~ s/\A\s+//;
        $line =~ s/\s+\z//;
        split /\s+/, $line, 2;
    } split /\n/, lc($sql // '');
    return $tokens[0] // '';
}

sub drain_until_ready {
    my ($dbh) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    while (1) {
        my ($type, undef, undef, $attachment, $txn_id, $payload) = recv_message($socket);
        next if handle_async($state, $type, $attachment, $txn_id, $payload);
        if ($type == MSG_READY) {
            apply_ready($state, $payload);
            return;
        }
        raise_error($payload) if $type == MSG_ERROR;
    }
}

sub drain_immediate_reopen_boundary {
    my ($dbh) = @_;
    my $socket = $dbh->{scratchbird_private_socket};
    my $state = $dbh->{scratchbird_private_state};
    while (socket_has_immediate_frame($socket)) {
        my ($type, undef, undef, $attachment, $txn_id, $payload) = recv_message($socket);
        next if handle_async($state, $type, $attachment, $txn_id, $payload);
        if ($type == MSG_READY) {
            apply_ready($state, $payload);
            next;
        }
        raise_error($payload) if $type == MSG_ERROR;
        die "08006: unexpected ScratchBird frame while draining transaction boundary";
    }
}

sub socket_has_immediate_frame {
    my ($socket) = @_;
    if ($socket && $socket->can('pending')) {
        my $pending = eval { $socket->pending };
        return 1 if defined $pending && $pending > 0;
    }
    require IO::Select;
    my $selector = IO::Select->new($socket);
    return scalar($selector->can_read(0)) ? 1 : 0;
}

sub send_message {
    my ($socket, $state, $type, $payload, %opts) = @_;
    $payload //= '';
    my $attachment = $opts{force_zero} ? ("\0" x 16) : ($state->{attachment_id} || ("\0" x 16));
    my $txn_id = $opts{force_zero} ? 0 : ($state->{txn_id} || 0);
    my $seq = $state->{sequence} || 0;
    my $header = pack('a4 C C C C V V a16 Q<', 'SBWP', 1, 1, $type, 0, length($payload), $seq, $attachment, $txn_id);
    $state->{sequence} = ($seq + 1) & 0xffffffff;
    print {$socket} $header . $payload or die "08006: socket write failed: $!";
}

sub recv_message {
    my ($socket) = @_;
    my $header = read_exact($socket, 40);
    my ($magic, $major, $minor, $type, $flags, $length, $sequence, $attachment, $txn_id) =
        unpack('a4 C C C C V V a16 Q<', $header);
    die "08006: invalid SBWP protocol magic" unless $magic eq 'SBWP';
    die "08006: unsupported SBWP protocol version" unless $major == 1 && $minor == 1;
    my $payload = $length ? read_exact($socket, $length) : '';
    return ($type, $flags, $sequence, $attachment, $txn_id, $payload);
}

sub read_exact {
    my ($socket, $length) = @_;
    my $out = '';
    while (length($out) < $length) {
        my $needed = $length - length($out);
        my $chunk = '';
        my $read = read($socket, $chunk, $needed);
        die "08006: socket read failed: $!" unless defined $read;
        die "08006: connection closed while reading from ScratchBird" if $read == 0;
        $out .= $chunk;
    }
    return $out;
}

sub build_startup_payload {
    my ($features, $required, $params) = @_;
    my $out = pack('v v V Q< Q< Q< a16 a16 a16 V', SBWP_VERSION, SBWP_VERSION, 0, $features, $required, 0, "\x11" x 16, "\0" x 16, "\0" x 16, scalar keys %{$params});
    for my $key (sort keys %{$params}) {
        $out .= lp_string($key);
        $out .= "\x01\0";
        my $value = $params->{$key} // '';
        $out .= pack('V', length($value)) . $value;
    }
    $out .= pack('V', 0);
    return $out;
}

sub handle_async {
    my ($state, $type, $attachment, $txn_id, $payload) = @_;
    if ($type == MSG_PARAMETER_STATUS) {
        for my $pair (@{parse_parameter_status($payload)}) {
            $state->{parameters}->{$pair->[0]} = $pair->[1];
            if ($pair->[0] eq 'attachment_id') {
                my $uuid = uuid_bytes($pair->[1]);
                $state->{attachment_id} = $uuid if defined $uuid;
            } elsif ($pair->[0] eq 'current_txn_id' && $pair->[1] =~ /^\d+$/) {
                $state->{txn_id} = int($pair->[1]);
            }
        }
        return 1;
    }
    if ($type == MSG_NOTICE) {
        return 1;
    }
    if ($type == MSG_TXN_STATUS) {
        $state->{txn_id} = unpack('Q<', substr($payload, 4, 8)) if length($payload) >= 12;
        return 1;
    }
    if ($type == MSG_SBLR_COMPILED) {
        die "08006: compiled SBLR payload is truncated" unless length($payload) >= 16;
        my ($hash, $version, $length) = unpack('Q< V V', substr($payload, 0, 16));
        die "08006: compiled SBLR bytecode is truncated" if 16 + $length > length($payload);
        $state->{last_sblr} = {
            hash => $hash,
            version => $version,
            bytecode => substr($payload, 16, $length),
        };
        return 1;
    }
    return 0;
}

sub parse_parameter_status {
    my ($payload) = @_;
    my @values;
    if (length($payload) >= 8) {
        my $count = unpack('V', substr($payload, 0, 4));
        my $offset = 4;
        if ($count > 0 && $count <= 256) {
            my $ok = eval {
                for (1 .. $count) {
                    my ($name, $next) = read_lp_string($payload, $offset);
                    $offset = $next + 3;
                    my ($value, $after) = read_lp_string($payload, $offset);
                    $offset = $after;
                    push @values, [$name, $value];
                }
                $offset == length($payload);
            };
            return \@values if $ok;
            @values = ();
        }
    }
    my ($name, $offset) = read_lp_string($payload, 0);
    my ($value, undef) = read_lp_string($payload, $offset);
    return [[$name, $value]];
}

sub parse_row_description {
    my ($payload) = @_;
    if (is_p1_row_description($payload)) {
        return parse_p1_row_description($payload);
    }
    die "08006: row description is truncated" unless length($payload) >= 4;
    my $count = unpack('v', substr($payload, 0, 2));
    my $offset = 4;
    my @columns;
    my @types;
    for my $idx (1 .. $count) {
        my ($name, $next) = read_lp_string($payload, $offset);
        $offset = $next;
        die "08006: row description field is truncated" if $offset + 18 > length($payload);
        $offset += 4;
        $offset += 2;
        push @types, unpack('V', substr($payload, $offset, 4));
        $offset += 4;
        $offset += 2 + 4 + 1 + 1 + 2;
        push @columns, length($name) ? $name : "column$idx";
    }
    return { columns => \@columns, types => \@types };
}

sub is_p1_row_description {
    my ($payload) = @_;
    return length($payload) >= 72
        && unpack('v', substr($payload, 0, 2)) == 1
        && unpack('C', substr($payload, 3, 1)) == 1;
}

sub parse_p1_row_description {
    my ($payload) = @_;
    my $count = unpack('l<', substr($payload, 4, 4));
    die "08006: P1 row description column count is invalid" if $count < 0;
    my $offset = 72;
    my @columns;
    my @types;
    for my $idx (0 .. $count - 1) {
        my $fixed_column_bytes = 4 + 4 + 8 + 144 + 56;
        die "08006: P1 row description is truncated" if $offset + $fixed_column_bytes > length($payload);
        my $ordinal = unpack('l<', substr($payload, $offset, 4));
        $offset += 4;
        $offset += 1;
        my $format = unpack('C', substr($payload, $offset, 1)) == 1 ? 0 : 1;
        $offset += 1;
        my $nullable = unpack('C', substr($payload, $offset, 1)) == 1 ? 1 : 0;
        $offset += 1;
        $offset += 1;
        $offset += 8;
        my $type_oid = oid_from_canonical_type_ref($payload, $offset);
        $offset += 144;
        $offset += 16 * 3;
        $offset += 4;
        $offset += 2;
        $offset += 2;
        my ($name, $next) = read_nullable_text($payload, $offset);
        $offset = $next;
        push @columns, length($name) ? $name : 'column' . ($idx + 1);
        push @types, $type_oid;
    }
    return { columns => \@columns, types => \@types };
}

sub oid_from_canonical_type_ref {
    my ($payload, $offset) = @_;
    return 25 if $offset + 4 > length($payload);
    my $family = unpack('v', substr($payload, $offset, 2));
    my $code = unpack('v', substr($payload, $offset + 2, 2));
    return 16 if $family == 1 && $code == 1;
    return 23 if $family == 2 && $code == 3;
    return 20 if $family == 2 && $code == 4;
    return 1700 if $family == 4 && $code == 1;
    return 701 if $family == 6 && $code == 2;
    return 25 if $family == 8 && $code == 1;
    return 25;
}

sub read_nullable_text {
    my ($payload, $offset) = @_;
    die "08006: nullable text is truncated" if $offset + 5 > length($payload);
    my $tag = unpack('C', substr($payload, $offset, 1));
    $offset += 1;
    my $length = unpack('l<', substr($payload, $offset, 4));
    $offset += 4;
    die "08006: nullable text length is invalid" if $length < 0;
    return ('', $offset) if $tag == 0;
    die "08006: nullable text payload is truncated" if $offset + $length > length($payload);
    return (substr($payload, $offset, $length), $offset + $length);
}

sub parse_data_row {
    my ($payload, $expected, $types) = @_;
    die "08006: data row is truncated" unless length($payload) >= 4;
    my ($count, $null_bytes) = unpack('v v', substr($payload, 0, 4));
    die "08006: data row column count mismatch" if $expected && $count != $expected;
    my $offset = 4 + $null_bytes;
    my $bitmap = substr($payload, 4, $null_bytes);
    my @values;
    for my $idx (0 .. $count - 1) {
        my $byte_index = int($idx / 8);
        my $bit_index = $idx % 8;
        my $is_null = $byte_index < length($bitmap) && (unpack('C', substr($bitmap, $byte_index, 1)) & (1 << $bit_index));
        if ($is_null) {
            push @values, undef;
            next;
        }
        die "08006: data row value is truncated" if $offset + 4 > length($payload);
        my $len = unpack('l<', substr($payload, $offset, 4));
        $offset += 4;
        if ($len < 0) {
            push @values, undef;
            next;
        }
        die "08006: data row value payload is truncated" if $offset + $len > length($payload);
        my $data = substr($payload, $offset, $len);
        $offset += $len;
        push @values, decode_value($types->[$idx] || 0, $data);
    }
    return \@values;
}

sub parse_command_complete {
    my ($payload) = @_;
    die "08006: command complete is truncated" unless length($payload) >= 20;
    my $rows = unpack('Q<', substr($payload, 4, 8));
    my $tag = substr($payload, 20);
    $tag =~ s/\0.*\z//s;
    return { rows => $rows, tag => $tag };
}

sub decode_value {
    my ($oid, $data) = @_;
    return undef unless defined $data;
    return unpack('C', $data) ? 1 : 0 if $oid == 16 && length($data) >= 1;
    return unpack('l<', $data) if $oid == 23 && length($data) == 4;
    return unpack('q<', $data) if $oid == 20 && length($data) == 8;
    return unpack('f<', $data) if $oid == 700 && length($data) == 4;
    return unpack('d<', $data) if $oid == 701 && length($data) == 8;
    return $data;
}

sub apply_ready {
    my ($state, $payload) = @_;
    if (length($payload) >= 76) {
        my $marker = unpack('C', substr($payload, 56, 1));
        if ($marker == 0x49 || $marker == 0x54 || $marker == 0x45 || $marker == 0x52 || $marker == 0x41) {
            my $txn_id = unpack('Q<', substr($payload, 48, 8));
            $state->{txn_id} = ($marker == 0x54 || $marker == 0x45) ? $txn_id : 0;
            return;
        }
    }
    return unless length($payload) >= 20;
    my $status = unpack('C', substr($payload, 0, 1));
    my $txn_id = unpack('Q<', substr($payload, 4, 8));
    $state->{txn_id} = $status ? $txn_id : 0;
}

sub raise_error {
    my ($payload) = @_;
    my $sqlstate = 'HY000';
    my $message = 'ScratchBird server returned an error';
    my $offset = 0;
    while ($offset < length($payload)) {
        my $code = substr($payload, $offset, 1);
        $offset++;
        last if $code eq "\0";
        my $nul = index($payload, "\0", $offset);
        last if $nul < 0;
        my $value = substr($payload, $offset, $nul - $offset);
        $sqlstate = $value if $code eq 'C';
        $message = $value if $code eq 'M';
        $message .= "\nDETAIL: $value" if $code eq 'D';
        $offset = $nul + 1;
    }
    die "$sqlstate: $message";
}

sub raise_copy_fail {
    my ($payload) = @_;
    my $message = 'COPY failed';
    if (length($payload) >= 4) {
        my $len = unpack('V', substr($payload, 0, 4));
        if ($len > 0 && 4 + $len <= length($payload)) {
            $message = substr($payload, 4, $len);
        }
    }
    die "HY000: $message";
}

sub lp_string {
    my ($text) = @_;
    $text //= '';
    return pack('V', length($text)) . $text;
}

sub read_lp_string {
    my ($payload, $offset) = @_;
    die "08006: length-prefixed string is truncated" if $offset + 4 > length($payload);
    my $len = unpack('V', substr($payload, $offset, 4));
    my $start = $offset + 4;
    die "08006: length-prefixed string payload is truncated" if $start + $len > length($payload);
    return (substr($payload, $start, $len), $start + $len);
}

sub uuid_bytes {
    my ($text) = @_;
    $text //= '';
    $text =~ s/-//g;
    return undef unless $text =~ /\A[0-9A-Fa-f]{32}\z/;
    return pack('H*', $text);
}

sub sqlstate_from_error {
    my ($message) = @_;
    return $1 if defined $message && $message =~ /\A([0-9A-Z]{5}):/;
    return 'HY000';
}

1;

__END__

=head1 NAME

DBD::ScratchBird - ScratchBird DBI driver route surface

=head1 DESCRIPTION

This lane exposes the Perl DBI driver shape and native transport setup. It
fails closed for unsupported transport or SBWP statement execution until the
Perl protocol executor is present.

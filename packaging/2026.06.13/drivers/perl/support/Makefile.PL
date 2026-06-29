# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

use strict;
use warnings;
use ExtUtils::MakeMaker;

WriteMakefile(
    NAME          => 'DBD::ScratchBird',
    VERSION_FROM  => 'lib/DBD/ScratchBird.pm',
    ABSTRACT      => 'ScratchBird DBI driver',
    AUTHOR        => 'ScratchBird Software Inc.',
    LICENSE       => 'mozilla_2_0',
    MIN_PERL_VERSION => '5.020',
    PREREQ_PM     => {
        'DBI'        => '1.643',
        'IO::Socket::SSL' => '2.000',
        'JSON::PP'   => '4.00',
    },
    test => {
        TESTS => 't/*.t',
    },
);

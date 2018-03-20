#!/usr/bin/env perl

#  Copyright (C) 2011 DeNA Co.,Ltd.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the Free Software
#  Foundation, Inc.,
#  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

use strict;
use warnings FATAL => 'all';

package MHA::NodeConst;

our $VERSION         = '0.56';
our $MGR_MIN_VERSION = '0.54';
our $SSH_OPT_ALIVE =
"-o ServerAliveInterval=60 -o ServerAliveCountMax=20 -o StrictHostKeyChecking=no -o ConnectionAttempts=5 -o PasswordAuthentication=no -o BatchMode=yes";

# apply_diff_relay_logs status codes
our $Target_Has_Received_All_Relay_Logs = 2;
our $Relay_Log_Not_Found                = 10;
our $Relay_Pos_Not_Found                = 12;
our $Applying_SQL_File_Failed           = 22;

1;

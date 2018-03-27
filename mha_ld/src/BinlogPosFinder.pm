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

package MHA::BinlogPosFinder;

use strict;
use warnings FATAL => 'all';

use MHA::NodeConst;

sub new {
  my $class = shift;
  my $self  = {
    name              => undef,
    master_version    => undef,
    server_id         => undef,
    mb_arg            => "",
    relay_dir         => undef,
    target_rmlp       => undef,
    end_log           => undef,
    handle_raw_binlog => undef,
    debug             => undef,
    @_,
  };
  return bless $self, $class;
}

sub find_starting_relay_pos { }
sub get_starting_master_pos { }

sub is_match_rotate_event($$) {
  my $self = shift;
  my $line = shift;

  if ( $line =~
    m/^#.*server id (\d+).*end_log_pos.*Rotate to\s+(\S+)\s+pos:\s+(\d+)\s+/ )
  {
    my $sid  = $1;
    my $mpos = $3;
    return 0 if ( $self->{server_id} == $sid );
    return 1 if ( $self->{target_rmlp} == $mpos );
  }
  return 0;
}

1;

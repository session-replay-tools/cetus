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

package MHA::BinlogPosFinderElp;

use strict;
use warnings FATAL => 'all';

use MHA::NodeConst;
use MHA::NodeUtil;
use MHA::BinlogHeaderParser;
use MHA::BinlogManager;

use base 'MHA::BinlogPosFinder';

sub get_starting_master_pos($$) {
  my $self   = shift;
  my $parser = shift;
  return $parser->get_starting_mlp();
}

sub find_starting_relay_pos($$) {
  my $self       = shift;
  my $relay_file = shift;
  my $start_rlp  = 0;
  my $found      = 0;
  my %status     = ();

  my $relay_dir = $self->{relay_dir};

  my $parser = new MHA::BinlogHeaderParser(
    dir            => $relay_dir,
    file           => $relay_file,
    self_server_id => $self->{server_id},
    target_rmlp    => $self->{target_rmlp},
    debug          => $self->{debug},
  );
  $parser->open_binlog();
  ( $found, $start_rlp ) = $parser->find_target_relay_pos();
  $parser->close_binlog();

  if ($found) {
    $status{status}    = 0;
    $status{start_rlf} = $relay_file;
    $status{start_rlp} = $start_rlp;
    if ( $start_rlp == $parser->{binlog_size} ) {
      if ( $relay_file eq $self->{end_log} ) {
        $status{status} = $MHA::NodeConst::Target_Has_Received_All_Relay_Logs;
      }
      else {
        $status{start_rlf} = MHA::BinlogManager::get_post_file($relay_file);
        $status{start_rlp} = 4;
      }
    }
    return %status;
  }
  else {
    print "Target pos NOT found!\n";
    $status{status} = $MHA::NodeConst::Relay_Pos_Not_Found;
    return %status;
  }
}

1;

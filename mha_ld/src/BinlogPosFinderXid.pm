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

package MHA::BinlogPosFinderXid;

use strict;
use warnings FATAL => 'all';

use Carp qw(croak);
use MHA::NodeConst;
use MHA::NodeUtil;
use MHA::BinlogHeaderParser;
use MHA::BinlogManager;

use base 'MHA::BinlogPosFinder';

sub get_starting_master_pos($$) {
  my $self   = shift;
  my $parser = shift;
  return $parser->get_starting_mlp_xid();
}

sub find_starting_relay_pos($$) {
  my $self       = shift;
  my $relay_file = shift;

  my $last_absolute_mpos = 0;
  my $prev_mpos          = 0;
  my $in_relative        = 0;
  my $real_mpos          = 0;
  my %status             = ();
  my $start_rlp;
  my $start_rlf;
  my $found;
  my $parser;
  my ( $relay_prefix, $start_relay_num ) =
    MHA::BinlogManager::get_head_and_number($relay_file);

  my $relay_dir = $self->{relay_dir};

  print
"Finding target relay pos by using Xid, not end_log_pos. This is because master's version is 5.0.45 or lower, or master's version is unknown. It is recommended upgrading MySQL 5.0.latest, 5.1 or later.\n";

  for ( my $i = $start_relay_num ; ; $i++ ) {
    my $from_file = $relay_prefix . "." . sprintf( "%06d", ($i) );
    last if ( !-f "$relay_dir/$from_file" );
    $parser = new MHA::BinlogHeaderParser(
      dir            => $relay_dir,
      file           => $from_file,
      self_server_id => $self->{server_id},
      target_rmlp    => $self->{target_rmlp},
      debug          => $self->{debug},
    );
    $parser->open_binlog();
    (
      $found, $start_rlp, $last_absolute_mpos, $prev_mpos, $in_relative,
      $real_mpos
      )
      = $parser->find_target_relay_pos_xid( $last_absolute_mpos, $prev_mpos,
      $in_relative, $real_mpos );
    $parser->close_binlog();
    if ($found) {
      $start_rlf = $from_file;
      last;
    }
  }
  if ($found) {
    $status{status}    = 0;
    $status{start_rlf} = $start_rlf;
    $status{start_rlp} = $start_rlp;
    if ( $start_rlp == $parser->{binlog_size} ) {
      if ( $start_rlf eq $self->{end_log} ) {
        $status{status} = $MHA::NodeConst::Target_Has_Received_All_Relay_Logs;
      }
      else {
        $status{start_rlf} = MHA::BinlogManager::get_post_file($start_rlf);
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

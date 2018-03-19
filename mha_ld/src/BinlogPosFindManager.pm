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

package MHA::BinlogPosFindManager;

use strict;
use warnings FATAL => 'all';

use English qw(-no_match_vars);
use MHA::BinlogManager;
use MHA::BinlogHeaderParser;
use MHA::BinlogPosFinder;
use MHA::BinlogPosFinderXid;
use MHA::BinlogPosFinderElp;
use MHA::NodeConst;
use MHA::NodeUtil;

sub new {
  my $class = shift;
  my $self  = {
    binlog_manager    => undef,
    server_id         => undef,
    target_mlf        => undef,
    target_rmlp       => undef,
    master_version    => undef,
    find_logname_only => undef,
    finder            => undef,
    debug             => undef,
    @_,
  };
  return bless $self, $class;
}

sub create_finder_elp($) {
  my $self = shift;
  return new MHA::BinlogPosFinderElp(
    name              => "BinlogPosFinderElp",
    master_version    => $self->{master_version},
    server_id         => $self->{server_id},
    relay_dir         => $self->{binlog_manager}->{dir},
    target_rmlp       => $self->{target_rmlp},
    end_log           => $self->{binlog_manager}->{end_log},
    handle_raw_binlog => $self->{binlog_manager}->{handle_raw_binlog},
    debug             => $self->{debug},
  );
}

sub create_finder_xid($) {
  my $self = shift;
  return new MHA::BinlogPosFinderXid(
    name              => "BinlogPosFinderXid",
    master_version    => $self->{master_version},
    server_id         => $self->{server_id},
    relay_dir         => $self->{binlog_manager}->{dir},
    target_rmlp       => $self->{target_rmlp},
    end_log           => $self->{binlog_manager}->{end_log},
    handle_raw_binlog => $self->{binlog_manager}->{handle_raw_binlog},
    debug             => $self->{debug},
  );
}

# Recursively reading relay log files in descending order
sub find_target_relay_log($$$$) {
  no warnings qw(recursion);
  my $self                             = shift;
  my $relay_file                       = shift;
  my $master_file_on_upper_relay_log   = shift;
  my $post_relay_has_init_rotate_event = shift;

  my $relay_dir             = $self->{binlog_manager}->{dir};
  my %status                = ();
  my $has_init_rotate_event = 0;

  # master log file on this relay log
  # master log pos at the starting of this relay log
  my ( $current_mlf, $starting_mlp );
  print "Reading $relay_file\n";

  my $parser = new MHA::BinlogHeaderParser(
    dir            => $relay_dir,
    file           => $relay_file,
    self_server_id => $self->{server_id},
    master_version => $self->{master_version},
    debug          => $self->{debug},
  );
  $parser->open_binlog();
  $parser->parse_init_headers();
  if ( $parser->{has_real_rotate_event} ) {
    $current_mlf = $parser->{current_mlf};
    $has_init_rotate_event = 1 if ( $parser->{has_real_init_rotate_event} );
  }

  $self->{master_version} = $parser->{master_version}
    unless ( $self->{master_version} );

  if (
    (
      !$self->{finder}
      || ( $self->{finder} && $self->{finder}->{name} eq "BinlogPosFinderXid" )
    )
    && $self->{master_version}
    && MHA::NodeUtil::mysql_version_ge( $self->{master_version}, "5.0.48" )
    )
  {
    $self->{finder} = $self->create_finder_elp();
  }
  else {
    $self->{finder} = $self->create_finder_xid() unless ( $self->{finder} );
  }

  # master's rotate event not found in this relay log
  unless ($current_mlf) {

# i.e. If master's init rotate event "Rotate to mysqld-bin.000123 (pos 4)" is written in the relay log mysqld-relay-bin.000444 and if mysqld-relay-bin.000443 does not have master's rotate event, mysqld-relay-bin.000443 should have binlog events from mysqld-bin.000122 (mysqld-bin.000123 - 1). If mysqld-relay-bin.000442 does not have master's rotate event, mysqld-relay-bin.000443 should have binlog events from mysqld-bin.000122.
    if ( $post_relay_has_init_rotate_event == 1 ) {
      $current_mlf =
        MHA::BinlogManager::get_prev_file($master_file_on_upper_relay_log);
    }
    else {
      $current_mlf = $master_file_on_upper_relay_log;
    }
  }
  $starting_mlp = $self->{finder}->get_starting_master_pos($parser);
  $parser->close_binlog();
  if ( $current_mlf && defined($starting_mlp) ) {
    print
      " $relay_file contains master $current_mlf from position $starting_mlp\n";
  }

# Target position should be within this relay log
# Determining starting relay log position
# Starting relay log position equals to just after the end_log_pos that equals to $read_master_log_pos.
  if ( $current_mlf
    && defined($starting_mlp)
    && $current_mlf eq $self->{target_mlf}
    && $starting_mlp <= $self->{target_rmlp} )
  {
    if ( $self->{find_logname_only} ) {
      $status{status}    = 0;
      $status{start_rlf} = $relay_file;
      return %status;
    }
    return $self->{finder}->find_starting_relay_pos($relay_file);
  }
  else {
    $self->find_target_relay_log(
      MHA::BinlogManager::get_prev_file($relay_file),
      $current_mlf, $has_init_rotate_event );
  }
}

1;

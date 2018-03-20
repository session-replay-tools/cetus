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

package MHA::BinlogHeaderParser;

use strict;
use warnings FATAL => 'all';

use English qw(-no_match_vars);
use Carp qw(croak);
use File::Basename;
use File::Path;
use Errno();

sub new {
  my $class = shift;
  my $self  = {
    dir                        => undef,
    file                       => undef,
    self_server_id             => undef,
    target_rmlp                => undef,
    target_mlf                 => undef,
    master_version             => undef,
    current_mlf                => undef,
    starting_mlp               => undef,
    fp                         => undef,
    current_pos                => 4,
    prev_pos                   => 4,
    binlog_size                => undef,
    has_real_rotate_event      => 0,
    has_real_init_rotate_event => 0,
    has_checksum_algo          => 0,
    debug                      => 0,
    @_,
  };
  return bless $self, $class;
}

our $ROTATE_EVENT                 = 4;
our $FORMAT_DESCRIPTION_EVENT     = 15;
our $XID_EVENT                    = 16;
our $LOG_EVENT_MINIMAL_HEADER_LEN = 19;
our $PREVIOUS_GTIDS_LOG_EVENT     = 35;
our $MAX_ALLOWED_PACKET           = 1024 * 1024 * 1024;

sub open_binlog($) {
  my $self = shift;
  croak "binlog dir must be set.\n"  unless ( $self->{dir} );
  croak "binlog file must be set.\n" unless ( $self->{file} );
  open( $self->{fp}, "<", "$self->{dir}/$self->{file}" )
    or croak "$!:$self->{dir}/$self->{file}";
  binmode $self->{fp};
  $self->{binlog_size} = -s "$self->{dir}/$self->{file}";
}

sub close_binlog($) {
  my $self = shift;
  close( $self->{fp} );
}

sub unpack_header {
  my $self = shift;
  my $pos  = shift;
  $pos = $self->{current_pos} if ( !defined($pos) );
  my $after_pos = shift;
  $after_pos = $pos if ( !defined($after_pos) );
  my $fp        = $self->{fp};
  my $file_size = $self->{binlog_size};
  return unpack_header_util( $fp, $file_size, $pos, $after_pos );
}

sub unpack_header_util {
  my $fp        = shift;
  my $file_size = shift;
  my $pos       = shift;
  my $after_pos = shift;
  $after_pos = $pos if ( !defined($after_pos) );
  if ( $pos >= $file_size || $after_pos >= $file_size ) {
    warn "Invalid position is set. pos=$pos after_pos=$after_pos\n";
    return;
  }
  my $event_type;
  my $server_id;
  my $event_length;
  my $end_log_pos;
  seek( $fp, $pos + 4, 0 );
  read( $fp, $event_type, 1 );
  seek( $fp, $pos + 5, 0 );
  read( $fp, $server_id, 4 );
  seek( $fp, $pos + 9, 0 );
  read( $fp, $event_length, 4 );
  seek( $fp, $pos + 13, 0 );
  read( $fp, $end_log_pos, 4 );
  seek( $fp, $after_pos, 0 );
  $event_type   = unpack( 'C', $event_type );
  $server_id    = unpack( 'V', $server_id );
  $event_length = unpack( 'V', $event_length );
  $end_log_pos  = unpack( 'V', $end_log_pos );

  if ( $event_length > $MAX_ALLOWED_PACKET ) {
    croak
"Event too large: pos: $pos, event_length: $event_length, event_type: $event_type\n";
  }

  if ( $event_length < $LOG_EVENT_MINIMAL_HEADER_LEN ) {
    croak
"Event too small: pos: $pos, event_length: $event_length, event_type: $event_type\n";
  }
  return ( $event_type, $server_id, $event_length, $end_log_pos );
}

sub get_server_version_from_fde($$) {
  my $self      = shift;
  my $pos       = shift;
  my $is_master = shift;
  my $server_version;
  my $fp = $self->{fp};
  my ( $event_type, $server_id, $event_length, $c ) =
    $self->unpack_header($pos);
  if ( $event_type == $FORMAT_DESCRIPTION_EVENT ) {
    seek( $fp, $pos + 21, 0 );
    read( $fp, $server_version, 50 );
    $server_version =~ s/\0//g;
    print " Master Version is $server_version\n" if ($is_master);
    if ( MHA::NodeUtil::mysql_version_ge( $server_version, "5.6.0" ) ) {
      seek( $fp, $pos + $event_length - 5, 0 );
      read( $fp, $self->{checksum_algo}, 1 );
      if ( $self->{checksum_algo} ne "\0" ) {
        print " Binlog Checksum enabled\n";
        $self->{checksum_algo} = 1;
      }
      else {
        $self->{checksum_algo} = 0;
      }
    }
    if ($is_master) {
      $self->{master_version} = $server_version;
    }
  }
  else {
    croak
      "This is not format description event! ev type=$event_type, pos=$pos.\n";
  }
  seek( $fp, $pos, 0 );
  return $server_version;
}

sub parse_master_rotate_event($$) {
  my $self = shift;
  my $pos  = shift;
  my $fp   = $self->{fp};
  my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->unpack_header($pos);

  if ( $event_type == $ROTATE_EVENT ) {

    # real rotate event
    if ( $end_log_pos == 0 || $end_log_pos == $self->{current_pos} ) {
      my $offset     = 19;
      my $header_len = 8;
      my $rotate_pos;
      seek( $fp, $pos + $offset, 0 );
      read( $fp, $rotate_pos, $header_len );
      $end_log_pos = unpack( 'V', $rotate_pos );
      seek( $fp, $pos + $offset + $header_len, 0 );
      my $read_length = $event_length - $offset - $header_len;

      if ( $self->{checksum_algo} ) {
        $read_length -= 4;
      }
      read( $fp, $self->{current_mlf}, $read_length );
      $self->{has_real_rotate_event} = 1;
      $self->{has_real_init_rotate_event} = 1 if ( $end_log_pos <= 4 );
      print
" parse_master_rotate_event(real rotate event): file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length current mlf=$self->{current_mlf} elp=$end_log_pos\n"
        if ( $self->{debug} );
    }
    else {
      print
" parse_master_rotate_event(non-real rotate event): file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length elp=$end_log_pos\n"
        if ( $self->{debug} );
    }
  }
  else {
    croak "This is not rotate event! ev type=$event_type, pos=$pos.\n";
  }
  seek( $fp, $pos, 0 );
  $self->{starting_mlp} = $end_log_pos unless ( $self->{starting_mlp} );
  return $end_log_pos;
}

sub parse_forward_header($) {
  my $self = shift;
  my $fp   = $self->{fp};
  my $pos  = $self->{current_pos};
  return if ( $self->{current_pos} >= $self->{binlog_size} );

  my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->unpack_header($pos);
  $self->{prev_pos}    = $self->{current_pos};
  $self->{current_pos} = $self->{current_pos} + $event_length;
  return ( $event_type, $server_id, $event_length, $end_log_pos );
}

sub parse_init_headers {
  my $self    = shift;
  my $do_dump = shift;
  my $outfile = shift;

  while ( my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->parse_forward_header() )
  {
    print
"parse_init_headers: file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length nextmpos=$end_log_pos prevrelay=$self->{prev_pos} cur(post)relay=$self->{current_pos}\n"
      if ( $self->{debug} );
    if ( $event_type == $FORMAT_DESCRIPTION_EVENT ) {
      if ( $self->{self_server_id} && $server_id != $self->{self_server_id} ) {
        $self->{starting_mlp} = $end_log_pos
          if ( !$self->{starting_mlp} && $end_log_pos > 0 );
        if ( !$self->{master_version} ) {
          $self->get_server_version_from_fde( $self->{prev_pos}, 1 );
        }
      }
      else {
        $self->get_server_version_from_fde( $self->{prev_pos}, 0 );
      }
    }
    elsif ( $event_type == $ROTATE_EVENT ) {
      if ( $self->{self_server_id} && $server_id != $self->{self_server_id} ) {
        $end_log_pos = $self->parse_master_rotate_event( $self->{prev_pos} );
      }
      if ($do_dump) {
        print
"dumping rotate event from $self->{prev_pos} to $self->{current_pos}.\n"
          if ( $self->{debug} );
        my $buf;
        open( my $out, ">>", $outfile ) or croak "$!:$outfile";
        binmode $out;
        seek( $self->{fp}, $self->{prev_pos}, 0 );
        read( $self->{fp}, $buf, $self->{current_pos} - $self->{prev_pos} );
        print $out $buf;
        close($out);
      }
    }
    elsif ( $event_type == $PREVIOUS_GTIDS_LOG_EVENT ) {
      print " Got previous gtids log event: $self->{current_pos}.\n"
        if ( $self->{debug} );
    }
    else {
      return $self->{prev_pos};
    }
  }
  return $self->{current_pos};
}

sub get_starting_mlp($) {
  my $self = shift;
  if ( $self->{starting_mlp} ) {
    print "starting_mlp: $self->{starting_mlp}\n" if ( $self->{debug} );
    return $self->{starting_mlp};
  }
  while ( my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->parse_forward_header() )
  {
    print
"get_starting_mlp: file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length next=$end_log_pos\n"
      if ( $self->{debug} );
    next if ( $server_id == $self->{self_server_id} );
    next if ( $event_type == $FORMAT_DESCRIPTION_EVENT );
    next if ( $event_type == $ROTATE_EVENT );
    next if ( $end_log_pos <= 0 );
    $self->{starting_mlp} = $end_log_pos;
    return $self->{starting_mlp};
  }
  return;
}

sub get_starting_mlp_xid($) {
  my $self = shift;
  while ( my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->parse_forward_header() )
  {
    print
"get_starting_mlp_xid: file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length next=$end_log_pos\n"
      if ( $self->{debug} );
    next if ( $server_id == $self->{self_server_id} );
    next if ( $event_type == $FORMAT_DESCRIPTION_EVENT );
    next if ( $event_type == $ROTATE_EVENT );
    next if ( $end_log_pos <= 0 );
    if ( $event_type == $XID_EVENT ) {
      $self->{starting_mlp} = $end_log_pos;
      return $self->{starting_mlp};
    }
    else {
      if ( $self->{starting_mlp} && $self->{starting_mlp} <= $end_log_pos ) {
        return $self->{starting_mlp};
      }
      else {
        return;
      }
    }
  }
  return;
}

sub find_target_relay_pos($) {
  my $self  = shift;
  my $found = 0;
  while ( my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->parse_forward_header() )
  {
    print
"find_target_relay_pos: file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length next=$end_log_pos\n"
      if ( $self->{debug} );
    next if ( $server_id == $self->{self_server_id} );
    next if ( $event_type == $FORMAT_DESCRIPTION_EVENT );
    if ( $event_type == $ROTATE_EVENT ) {
      $end_log_pos = $self->parse_master_rotate_event( $self->{prev_pos} );
    }
    if ( $end_log_pos == $self->{target_rmlp} ) {
      $found = 1;
      return ( $found, $self->{current_pos} );
    }
  }
  $found = 0;
  return $found;
}

sub find_target_relay_pos_xid($$$$$) {
  my $self               = shift;
  my $last_absolute_mpos = shift;
  my $prev_mpos          = shift;
  my $in_relative        = shift;
  my $real_mpos          = shift;

  my $found = 0;
  my $start_rlp;

  while ( my ( $event_type, $server_id, $event_length, $end_log_pos ) =
    $self->parse_forward_header() )
  {
    print
"find_target_relay_pos_xid: file=$self->{file} event_type=$event_type server_id=$server_id length=$event_length next=$end_log_pos\n"
      if ( $self->{debug} );
    next if ( $event_type == $FORMAT_DESCRIPTION_EVENT );
    next if ( $server_id == $self->{self_server_id} );
    if ( $event_type == $ROTATE_EVENT ) {
      $end_log_pos = $self->parse_master_rotate_event( $self->{prev_pos} );
    }
    if ( $end_log_pos == $event_length
      || ( $end_log_pos < $prev_mpos && !$in_relative ) )
    {

      # Entering relative mode
      $last_absolute_mpos = $prev_mpos;
      $in_relative        = 1;
      $real_mpos          = $last_absolute_mpos + $end_log_pos;
    }
    elsif ( $end_log_pos < $prev_mpos && $in_relative ) {

      # void event
    }
    elsif ( $in_relative && $event_type != $XID_EVENT ) {

      # Continuing relative mode
      $real_mpos = $last_absolute_mpos + $end_log_pos;
    }
    elsif ( $event_type == $XID_EVENT ) {

      # Exiting relative mode
      $real_mpos          = $end_log_pos;
      $last_absolute_mpos = $end_log_pos;
      $in_relative        = 0;
    }
    else {

      # Not in relative mode
      $real_mpos          = $end_log_pos;
      $last_absolute_mpos = $end_log_pos;
    }
    $prev_mpos = $end_log_pos;
    if ( $real_mpos == $self->{target_rmlp} ) {
      $found     = 1;
      $start_rlp = $self->{current_pos};
      last;
    }
  }
  return ( $found, $start_rlp, $last_absolute_mpos, $prev_mpos, $in_relative,
    $real_mpos );
}

1;

